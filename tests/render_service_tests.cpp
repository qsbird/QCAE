#include "qcae/render_service.hpp"
#include "qcae/operations.hpp"
#include "qcae/ipc_model.hpp"
#include "qcae/render_wire.hpp"
#include "runtime_test_support.hpp"
#include <QCoreApplication>
#include <iostream>

namespace {
using namespace qcae;
using runtime_test::check;
using runtime_test::good;

void polygon_requires_negotiated_wire_and_empty_ack_keeps_versions() {
    auto store = std::make_shared<runtime_test::Store>();
    RecordApplication application(runtime_test::options(store));
    const Caller caller{"display-contract"};
    auto info = good(application.create_document(caller, "Display contract", "create"));
    good(application.execute(
        caller,
        runtime_test::at(info),
        "test.seed",
        "seed",
        [](const DocumentView& view,
           const RecordIdentityAllocator&) -> Result<RecordPreparedOperation> {
            EditSession edit(view);
            edit.put(records::Material{EntityId("shape"), "Display fixture", 210000, .3});
            const std::array<std::array<double, 3>, 4> positions{
                {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}}};
            for (std::size_t i = 0; i < positions.size(); ++i)
                edit.put(records::Node{EntityId("node-" + std::to_string(i)), positions[i], {}});
            return {Status::success,
                    RecordPreparedOperation{
                        edit.prepare(), "Seed", EntityId("shape"), "seed", 0, false},
                    {}};
        },
        "seed"));
    info = good(application.current_document());
    const auto original_history = good(application.history(info.document));
    const auto snapshot = good(application.snapshot(info.document));
    SelectionService selections;
    const auto view = good(
        selections.create_view(snapshot.records, caller, {EntityId("node-0")}, "fixed-camera"));
    RenderContributions contributions;
    contributions.add(
        {RecordTraits<records::Node>::type_id, "point", [](const Record& record) -> RenderItem {
             const auto& node = record->get<records::Node>();
             return RenderPoint{node.id, node.position, true};
         }});
    // An existing scalar record is projected as a four-point synthetic area only
    // in this test; production adds no persistent area or element type here.
    contributions.add(
        {RecordTraits<records::Material>::type_id, "area", [](const Record& record) -> RenderItem {
             return RenderCellSource{
                 record->get<records::Material>().id,
                 RenderCellKind::polygon,
                 {EntityId("node-0"), EntityId("node-1"), EntityId("node-2"), EntityId("node-3")}};
         }});
    contributions.freeze();
    ipc::ResourceStore resources;
    ipc::RenderService service(application, selections, resources, std::move(contributions));
    QJsonObject parameters{{"view_session_id", QString::fromStdString(view.id)},
                           {"expected_view_revision", QString::number(view.view_revision)}};
    QJsonObject request{
        {"api_version",
         QString::fromUtf8(api_version.data(), static_cast<qsizetype>(api_version.size()))},
        {"request_id", "display-test"},
        {"operation", "view.render_resource"},
        {"requested_version", 1},
        {"document_id", QString::fromStdString(info.document.id.value)},
        {"document_epoch", QString::fromStdString(info.document.epoch.value)},
        {"expected_revision", QString::number(info.revision)},
        {"parameters", parameters}};
    const auto unsupported = [&] {
        const auto response = service.dispatch(request, caller);
        check(response.value("status") == "failed" &&
                  response.value("error").toObject().value("code") == "UNSUPPORTED_CAPABILITY",
              "A legacy display request silently discarded an area cell");
    };
    unsupported();
    parameters.insert("base_revision", QString::number(info.revision));
    parameters.insert("base_view_revision", QString::number(view.view_revision));
    parameters.insert("allow_inline_empty", true);
    request.insert("parameters", parameters);
    unsupported(); // A guessed baseline after failed full cannot bypass the area requirement.
    parameters.remove("base_revision");
    parameters.remove("base_view_revision");
    parameters.insert("render_wire_version", 3);
    request.insert("parameters", parameters);
    auto response = service.dispatch(request, caller);
    check(response.value("status") == "success", "Negotiated generic cell request failed");
    const auto manifest_json = response.value("data").toObject().value("manifest").toObject();
    const ResourceVersion version{info.document, info.revision, view.id, view.view_revision};
    const auto manifest = good(resources.describe(
        caller, manifest_json.value("resource_id").toString().toStdString(), version));
    check(manifest.media_type == "qcae.render.packet.v3",
          "Generic packet has a mismatched media version");
    const auto chunk = good(resources.read(caller, manifest.resource_id, manifest.version, 0));
    const auto packet = good(transport::decode_render_packet(chunk.bytes));
    check(packet.cells.size() == 1 && packet.cells[0].entity == EntityId("shape") &&
              packet.cells[0].points.size() == 4,
          "Negotiated resource lost the area topology or stable identity");
    parameters.insert("base_revision", QString::number(info.revision));
    parameters.insert("base_view_revision", QString::number(view.view_revision));
    request.insert("parameters", parameters);
    response = service.dispatch(request, caller);
    const auto data = response.value("data").toObject();
    const auto ack = data.value("acknowledgement").toObject();
    check(response.value("status") == "success" && data.value("mode") == "version_only" &&
              !data.contains("manifest") &&
              ack.value("document_id") == request.value("document_id") &&
              ack.value("document_epoch") == request.value("document_epoch") &&
              ack.value("revision") == ack.value("base_revision") &&
              ack.value("view_revision") == ack.value("base_view_revision"),
          "Empty negotiated acknowledgement omitted exact authority or created a resource");
    const auto final_history = good(application.history(info.document));
    check(good(application.current_document()).revision == info.revision &&
              final_history.cursor == original_history.cursor &&
              final_history.items.size() == original_history.items.size(),
          "Display compatibility or empty acknowledgement mutated document history");

    const auto old_selection = good(selections.select(snapshot.records, caller, view.id, {}));
    const auto baseline = info.revision;
    good(application.execute(
        caller,
        runtime_test::at(info),
        "test.material.edit",
        "material-input",
        [](const DocumentView& input,
           const RecordIdentityAllocator&) -> Result<RecordPreparedOperation> {
            EditSession edit(input);
            auto material =
                input.find<records::Material>(EntityId("shape"))->get<records::Material>();
            material.young_modulus_mpa = 205000;
            edit.put(std::move(material));
            return {Status::success,
                    RecordPreparedOperation{
                        edit.prepare(), "Material", EntityId("shape"), "material-input", 0, false},
                    {}};
        },
        "material"));
    info = good(application.current_document());
    const auto current_snapshot = good(application.snapshot(info.document));
    const auto committed_history = good(application.history(info.document));
    request.insert("expected_revision", QString::number(info.revision));
    const auto old_path = service.dispatch(request, caller);
    check(old_path.value("status") == "conflict",
          "An unselected model rebase silently changed a legacy view");
    parameters.insert("allow_model_rebase", true);
    parameters.insert("include_changed_rows", true);
    const auto expect_rejected = [&](QJsonObject candidate,
                                     std::optional<Caller> actor = std::nullopt) {
        auto invalid = request;
        invalid.insert("parameters", candidate);
        const auto rejected = service.dispatch(invalid, actor ? *actor : caller);
        check(rejected.value("status") != "success", "Invalid display model rebase succeeded");
        const auto stored =
            good(selections.inspect_view(current_snapshot.records, caller, view.id));
        check(stored.model_revision == baseline && stored.view_revision == view.view_revision &&
                  stored.hidden_ids == view.hidden_ids &&
                  stored.camera_fingerprint == view.camera_fingerprint,
              "A rejected rebase changed the view before checking all fences");
    };
    for (const auto* field : {"allow_model_rebase", "include_changed_rows"}) {
        auto wrong_type = parameters;
        wrong_type.insert(field, "true");
        expect_rejected(wrong_type);
    }
    auto missing_base = parameters;
    missing_base.remove("base_revision");
    expect_rejected(missing_base);
    auto wrong_base = parameters;
    wrong_base.insert("base_revision", QString::number(baseline - 1));
    expect_rejected(wrong_base);
    auto wrong_view = parameters;
    wrong_view.insert("expected_view_revision", QString::number(view.view_revision + 1));
    expect_rejected(wrong_view);
    auto wrong_base_view = parameters;
    wrong_base_view.insert("base_view_revision", QString::number(view.view_revision + 1));
    expect_rejected(wrong_base_view);
    expect_rejected(parameters, Caller{"other-caller"});
    request.insert("parameters", parameters);
    response = service.dispatch(request, caller);
    const auto refreshed = response.value("data").toObject();
    const auto next_ack = refreshed.value("acknowledgement").toObject();
    const auto next_view = good(selections.get_view(current_snapshot.records, caller, view.id));
    check(response.value("status") == "success" && refreshed.value("mode") == "version_only" &&
              !refreshed.contains("manifest") && next_view.model_revision == info.revision &&
              next_view.view_revision == view.view_revision + 1 &&
              next_view.hidden_ids == view.hidden_ids &&
              next_view.camera_fingerprint == view.camera_fingerprint &&
              next_ack.value("base_revision") == QString::number(baseline) &&
              next_ack.value("base_view_revision") == QString::number(view.view_revision) &&
              next_ack.value("revision") == QString::number(info.revision) &&
              next_ack.value("view_revision") == QString::number(next_view.view_revision),
          "Empty model rebase lost its exact from/to versions, camera or hidden state");
    const auto changed_rows = refreshed.value("changed_rows").toArray();
    const auto row_version = refreshed.value("rows_version").toObject();
    check(refreshed.value("rows_complete") == true && !refreshed.value("refresh_tree").toBool() &&
              changed_rows.size() == 1 &&
              changed_rows[0].toObject().value("entity_id") == "shape" &&
              changed_rows[0].toObject().value("young_modulus_mpa") == 205000 &&
              row_version.value("revision") == QString::number(info.revision) &&
              row_version.value("view_revision") == QString::number(next_view.view_revision),
          "Changed entity rows did not use the same immutable render input version");
    check(!selections.page(current_snapshot.records, caller, old_selection.id, 0, 100).ok() &&
              !selections
                   .combine(current_snapshot.records,
                            caller,
                            view.id,
                            old_selection.id,
                            old_selection.id,
                            SetOperation::union_)
                   .ok(),
          "A material-only rebase accepted a selection from the old view version");
    const auto repeated = service.dispatch(request, caller);
    check(repeated.value("status") == "conflict" &&
              good(selections.get_view(current_snapshot.records, caller, view.id)).view_revision ==
                  next_view.view_revision,
          "An identical stale rebase advanced the view a second time");
    const std::vector<std::string> repeated_ids{"shape", "shape"}, two_ids{"shape", "node-0"};
    check(!ipc::entity_rows_json(current_snapshot, repeated_ids).ok(),
          "Duplicate changed-row identities were accepted");
    const auto bounded = good(ipc::entity_rows_json(current_snapshot, two_ids, 1));
    const auto byte_bounded = good(ipc::entity_rows_json(current_snapshot, two_ids, 1000, 64));
    check(!bounded.complete && bounded.rows.isEmpty() && !byte_bounded.complete &&
              byte_bounded.rows.isEmpty(),
          "Truncated entity rows were advertised as complete");
    const auto after_rebase = good(application.history(info.document));
    check(good(application.current_document()).revision == info.revision &&
              after_rebase.cursor == committed_history.cursor &&
              after_rebase.items.size() == committed_history.items.size(),
          "Model rebase or rows altered committed model history");
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        polygon_requires_negotiated_wire_and_empty_ack_keeps_versions();
        std::cout << "PASS: negotiated cell resources, legacy refusal and empty authority "
                     "acknowledgement\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
