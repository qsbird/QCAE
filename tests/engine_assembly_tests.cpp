#include "engine_test_contribution.hpp"
#include "qcae/core.hpp"
#include "qcae/ipc_api.hpp"
#include "qcae/nastran_contribution.hpp"
#include <QJsonArray>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace {
struct IncompleteCodec final : qcae::IModelCodec, qcae::IProfileProvider {
    qcae::ProfileDefinition profile;
    const qcae::ProfileDefinition& definition() const noexcept override {
        return profile;
    }
    qcae::ImportOutcome decode(const qcae::ImportRequest&) const override {
        return {};
    }
    qcae::ExportOutcome
    encode(const qcae::Model&, const qcae::EntityId&, const qcae::ProfileRef&) const override {
        return {};
    }
};
void check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
void rejects_duplicate_contribution() {
    auto contributions = qcae::ipc::default_engine_contributions();
    contributions.push_back(contributions.front());
    bool rejected{};
    try {
        (void)qcae::ipc::assemble_engine(contributions);
    } catch (const qcae::RecordError&) {
        rejected = true;
    }
    check(rejected, "duplicate contribution must be rejected");
}
void rejects_duplicate_operation() {
    auto contributions = qcae::ipc::default_engine_contributions();
    const auto extra = contribution_test::contribution();
    contributions.push_back(extra);
    contributions.push_back({"test.duplicate-operation", {}, extra.operations});
    auto assembly = qcae::ipc::assemble_engine(contributions);
    qcae::MemoryApplication facade({}, {}, {}, {}, assembly.records);
    bool rejected{};
    try {
        qcae::ipc::TypedHost host(facade.record_application(), {}, assembly.operations);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    check(rejected, "duplicate operation must prevent host publication");
}
void rejects_reserved_operations() {
    using namespace qcae;
    using namespace qcae::operations;
    MemoryApplication facade;
    for (const auto* id : {"entity.fields",
                           "task.status",
                           "task.cancel",
                           "task.reconcile",
                           "project.create",
                           "entity.query",
                           "runtime.handshake"}) {
        bool rejected{};
        try {
            ipc::TypedHost host(
                facade.record_application(),
                {},
                [id](OperationRegistry& registry,
                     RecordApplication&,
                     std::function<TaskService&()>) {
                    return registry.declare_unavailable(
                        {id, 1, "test.reserved.v1", OperationEffect::read_only, {}, {}},
                        "Reserved-name probe");
                });
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        check(rejected, "contributed declarations must not shadow existing host operations");
    }
}
void freezes_registry_and_shares_application() {
    auto contributions = qcae::ipc::default_engine_contributions();
    contributions.push_back(contribution_test::contribution());
    auto assembly = qcae::ipc::assemble_engine(contributions);
    check(assembly.records->frozen(), "assembled records must be frozen");
    check(assembly.records->find(qcae::RecordTraits<contribution_test::Relation>::type_id),
          "test record must reach assembled registry");
    qcae::MemoryApplication facade({}, {}, {}, {}, assembly.records);
    const auto created = facade.create_document({"test"}, "Assembled", "create");
    check(created.ok(), "common project operation must remain usable");
    const auto view = facade.record_application().snapshot(created.value->document);
    check(view.ok() && view.value->records.registry() == assembly.records,
          "compatibility facade must use exactly the assembled registry");
    qcae::ipc::TypedHost host(facade.record_application(), {}, std::move(assembly.operations));
    check(host.supports("test.relation.create"), "contributed operation must reach typed host");
    const auto standard = qcae::ipc::assemble_engine(qcae::ipc::default_engine_contributions());
    check(!standard.records->find(qcae::RecordTraits<contribution_test::Relation>::type_id),
          "production records must exclude test contribution");
    qcae::ipc::TypedHost without(facade.record_application(), {}, standard.operations);
    check(!without.supports("test.relation.create"), "disabled operation must be absent");
}
void selects_only_installed_services() {
    using namespace qcae;
    const auto defaults = ipc::default_engine_contributions();
    const std::array platform{defaults.front()};
    const auto common = ipc::assemble_engine(platform);
    check(!common.model_codec() && !common.profile_definition() &&
              common.profile_codec_owner.empty() && common.render_owner.empty(),
          "platform-only assembly must not inherit compiled Nastran services");
    check(common.render.frozen() && common.render.find(RecordTraits<records::Tri3>::type_id),
          "platform-only assembly must retain the generic render factory");
    MemoryApplication facade({}, {}, {}, {}, common.records);
    ipc::TypedHost host(facade.record_application(), {}, common.operations);
    const auto catalog = ipc::dispatch(facade,
                                       {{"api_version", "1.1"},
                                        {"request_id", "platform-catalog"},
                                        {"operation", "capabilities.list"},
                                        {"parameters", QJsonObject{}}},
                                       {"test"},
                                       common.model_codec(),
                                       common.profile_definition(),
                                       nullptr,
                                       &host);
    check(
        catalog.value("status").toString() == "success" &&
            catalog.value("data").toObject().value("declared_solver_profiles").toArray().isEmpty(),
        "discovery must use the selected assembly's profile binding");
    check(host.supports("geometry.create_line") && !host.supports("nastran.ui"),
          "common operations must remain installed without Nastran UI");
    const auto normal = ipc::assemble_engine(defaults);
    const bool enabled = ipc::nastran_package_enabled();
    check((normal.model_codec() != nullptr) == enabled &&
              (normal.profile_definition() != nullptr) == enabled,
          "default ON/OFF codec selection changed");
    check(normal.render.entries().size() == (enabled ? 3u : 4u) &&
              normal.render_owner == (enabled ? "qcae.nastran" : ""),
          "default ON/OFF render factory or ownership changed");
    const auto coordinator = std::make_shared<ipc::NastranArtifactCoordinator>();
    ipc::EngineContribution codec_only;
    codec_only.id = "test.codec-only";
    codec_only.profile_codec = coordinator->codec_binding();
    const auto codec_selected = ipc::assemble_engine(std::array{codec_only});
    check(codec_selected.model_codec() == &coordinator->codec() &&
              codec_selected.profile_codec_owner == codec_only.id &&
              codec_selected.render_owner.empty(),
          "a callable codec binding is a valid standalone contribution");
    ipc::EngineContribution render_only;
    render_only.id = "test.render-only";
    render_only.render_factory = [] { return ipc::nastran_render_contributions(); };
    const auto render_selected = ipc::assemble_engine(std::array{render_only});
    check(!render_selected.model_codec() && render_selected.render_owner == render_only.id &&
              render_selected.render.entries().size() == 3,
          "a callable render factory is a valid standalone contribution");
}
void keeps_selected_codec_alive() {
    using namespace qcae;
    ipc::EngineAssembly selected;
    const IModelCodec* actual{};
    const IProfileProvider* actual_profile{};
    std::weak_ptr<ipc::NastranArtifactCoordinator> released;
    {
        auto coordinator = std::make_shared<ipc::NastranArtifactCoordinator>();
        released = coordinator;
        actual = &coordinator->codec();
        actual_profile = &coordinator->codec();
        const std::array contribution{ipc::nastran_engine_contribution(coordinator)};
        selected = ipc::assemble_engine(contribution);
    }
    // Remove other coordinator-owning callbacks so this checks the binding's own lifetime.
    selected.operations = {};
    selected.publisher_factory = {};
    check(released.expired() && selected.model_codec() == actual &&
              selected.profile_codec->profile.get() == actual_profile &&
              selected.profile_codec_owner == "qcae.nastran",
          "selected binding must retain the actual codec after coordinator wrapper release");
    const auto profile = selected.profile_definition()->reference;
    ImportRequest input{"cantilever.bdf", {}, profile, "engine-binding-test", "mm-N-MPa"};
    const auto fixtures = std::filesystem::path(__FILE__).parent_path() / "fixtures/nastran";
    for (const auto* path :
         {"cantilever.bdf", "mesh/nodes.bdf", "mesh/beams.bdf", "properties.bdf"}) {
        std::ifstream file(fixtures / path, std::ios::binary);
        check(static_cast<bool>(file), "codec binding fixture is missing");
        input.resources.push_back(
            {path, {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()}});
    }
    const auto decoded = selected.model_codec()->decode(input);
    check(decoded.report.complete && decoded.candidate && !decoded.candidate->analyses.empty(),
          "retained registered codec must perform a real import");
    const auto encoded = selected.model_codec()->encode(
        *decoded.candidate, decoded.candidate->analyses.front().id, profile);
    check(encoded.report.complete && encoded.artifact && encoded.artifact->profile == profile,
          "retained registered codec and definition must perform a real export");
    const auto registry = make_record_registry();
    const auto record = registry->make(records::Node{EntityId("projection"), {1, 2, 3}, {}});
    const auto projection = selected.render.find(RecordTraits<records::Node>::type_id);
    check(projection &&
              std::get<RenderPoint>(projection->project(record)).entity == EntityId("projection"),
          "registered render factory must retain a real callable projection");
}
void rejects_invalid_services() {
    using namespace qcae;
    const auto coordinator = std::make_shared<ipc::NastranArtifactCoordinator>();
    const auto valid = ipc::nastran_engine_contribution(coordinator);
    const auto rejects = [](std::vector<ipc::EngineContribution> contributions) {
        try {
            (void)ipc::assemble_engine(contributions);
        } catch (const RecordError& error) {
            return error.code() == ErrorCode::invalid_input && error.field() == "contribution";
        }
        return false;
    };
    auto invalid = valid;
    invalid.profile_codec->codec.reset();
    check(rejects({invalid}), "null codec binding must be rejected");
    invalid = valid;
    invalid.profile_codec->profile.reset();
    check(rejects({invalid}), "null profile binding must be rejected");
    invalid = valid;
    invalid.profile_codec->profile = std::make_shared<NastranCodec>();
    check(rejects({invalid}), "unrelated profile/codec lifetimes must be rejected");
    invalid = valid;
    invalid.profile_codec = ipc::EngineCodecBinding{
        std::shared_ptr<const IModelCodec>(std::shared_ptr<const IModelCodec>{},
                                           &coordinator->codec()),
        std::shared_ptr<const IProfileProvider>(std::shared_ptr<const IProfileProvider>{},
                                                &coordinator->codec())};
    check(rejects({invalid}), "borrowed aliases without an owner must be rejected");
    invalid = valid;
    const auto incomplete = std::make_shared<IncompleteCodec>();
    invalid.profile_codec = ipc::EngineCodecBinding{incomplete, incomplete};
    check(rejects({invalid}), "incomplete profile definition must be rejected");
    invalid = valid;
    invalid.id = "test.second-profile";
    check(rejects({valid, invalid}), "distinct contribution IDs cannot install two codecs");
    invalid.profile_codec.reset();
    check(rejects({valid, invalid}),
          "distinct contribution IDs cannot install two render factories");
    invalid = valid;
    invalid.render_factory = [] { return RenderContributions{}; };
    check(rejects({invalid}), "unfrozen renderer must be rejected");
    invalid.render_factory = [] {
        RenderContributions empty;
        empty.freeze();
        return empty;
    };
    check(rejects({invalid}), "empty renderer must be rejected");
}
} // namespace
int main() {
    try {
        rejects_duplicate_contribution();
        rejects_duplicate_operation();
        rejects_reserved_operations();
        freezes_registry_and_shares_application();
        selects_only_installed_services();
        keeps_selected_codec_alive();
        rejects_invalid_services();
        std::cout << "PASS: static engine assembly, duplicate rejection and shared authority\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
