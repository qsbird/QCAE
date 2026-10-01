#include "qcae/analysis_features.hpp"
#include "qcae/analysis_input.hpp"
#include "qcae/mesh_editing_operations.hpp"
#include "qcae/nastran_package.hpp"
#include "qcae/records.hpp"
#include "qcae/render_projector.hpp"
#include <filesystem>
#include <iostream>
#include <map>
#include <random>
#include <stdexcept>
#ifdef QCAE_TEST_SQLITE
#include "qcae/sqlite_store.hpp"
#endif

using namespace qcae;
using namespace qcae::operations;
namespace {
void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
template <class T> T good(Result<T> value) {
    if (!value.ok())
        throw std::runtime_error(value.error ? value.error->message : "Missing result");
    return std::move(*value.value);
}
template <class T> void bad(const Result<T>& value, ErrorCode code) {
    check(!value.ok() && value.error && value.error->code == code, "Expected atomic rejection");
}
struct MemoryRows final : IRecordStore {
    std::map<StoreKey, SharedStoreBytes> rows;
    std::uint64_t generation{};
    LoadedRows load_rows() override {
        LoadedRows result;
        result.generation = generation;
        for (const auto& [key, value] : rows)
            result.rows.push_back({key, value});
        return result;
    }
    BatchReceipt commit_rows(const StoreBatch& batch) override {
        check(batch.expected_generation == generation, "Generation must match");
        auto next = rows;
        for (const auto& mutation : batch.mutations)
            if (mutation.after)
                next[mutation.key] = mutation.after;
            else
                next.erase(mutation.key);
        rows.swap(next);
        return {++generation, batch.mutations.size(), 0};
    }
};
WriteContext at(const DocumentInfo& info) {
    return {info.document, info.revision};
}
OperationContext context(RecordApplication& app, std::string key) {
    const auto info = good(app.current_document());
    return {Caller{"tri3-tests"}, info.document, info.revision, std::move(key), {}, "tri3"};
}
RecordPrepare seed() {
    return [](const DocumentView& view, const RecordIdentityAllocator&) {
        EditSession edit(view);
        edit.put(records::Mesh{records::MeshId("mesh"), "Surface", "manual", {}, 0, false});
        edit.put(records::Mesh{records::MeshId("other"), "Other", "manual", {}, 0, false});
        const records::MeshId mesh("mesh");
        edit.put(records::Node{EntityId("a"), {0, 0, 0}, mesh});
        edit.put(records::Node{EntityId("b"), {2, 0, 0}, mesh});
        edit.put(records::Node{EntityId("c"), {0, 2, 0}, mesh});
        edit.put(records::Node{EntityId("d"), {4, 0, 0}, mesh});
        edit.put(records::Node{EntityId("foreign"), {0, 0, 2}, records::MeshId("other")});
        edit.put(records::Material{EntityId("material"), "Steel", 210000, .3});
        return Result<RecordPreparedOperation>{
            Status::success,
            RecordPreparedOperation{edit.prepare(), "Seed", EntityId{}, "seed", 0, true},
            {}};
    };
}
void run() {
    const auto directory = std::filesystem::temp_directory_path() /
                           ("qcae-tri3-" + std::to_string(std::random_device{}()));
    std::filesystem::create_directory(directory);
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::filesystem::remove_all(path);
        }
    } cleanup{directory};
    RecordApplicationOptions options;
    options.registry = make_record_registry();
#ifdef QCAE_TEST_SQLITE
    auto sqlite = std::make_shared<SqliteWorkspaceStore>((directory / "work.sqlite").string());
    options.records = sqlite;
    options.projects = sqlite;
#else
    options.records = std::make_shared<MemoryRows>();
#endif
    DocumentInfo final;
    EntityId triangle;
    DocumentView expected(options.registry);
    const Caller caller{"tri3-tests"};
    OperationContext original_context;
    {
        RecordApplication app(options);
        auto info = good(app.create_document(caller, "Tri3", "create"));
        good(app.execute(caller, at(info), "test.seed", "seed", seed(), "seed"));
        OperationRegistry registry;
        good(features::mesh_editing::register_handlers(registry, app));
        const auto snapshot = [&] {
            return good(app.snapshot(good(app.current_document()).document));
        };
        const auto input =
            MeshCreateTri3Input{{EntityId("a"), EntityId("b"), EntityId("c")}, EntityId("mesh")};
        const auto before = snapshot();
        const auto reject = [&](const MeshCreateTri3Input& invalid, const char* key) {
            const auto generation = options.records->load_rows().generation;
            check(!registry
                       .invoke("mesh.create_tri3",
                               context(app, key),
                               InputTraits<MeshCreateTri3Input>::to_value(invalid))
                       .ok(),
                  "Invalid connectivity must be rejected");
            check(options.records->load_rows().generation == generation &&
                      diff_record_views(before.records, snapshot().records).empty() &&
                      snapshot().info.revision == before.info.revision,
                  "Invalid topology must not change records, revision or durable rows");
        };
        reject({{EntityId("a"), EntityId("b")}, EntityId("mesh")}, "short");
        reject({{EntityId("a"), EntityId("b"), EntityId("b")}, EntityId("mesh")}, "duplicate");
        reject({{EntityId("a"), EntityId("b"), EntityId("unknown")}, EntityId("mesh")}, "missing");
        reject({{EntityId("a"), EntityId("b"), EntityId("material")}, EntityId("mesh")}, "type");
        reject({{EntityId("a"), EntityId("b"), EntityId("d")}, EntityId("mesh")}, "zero-area");
        reject({{EntityId("a"), EntityId("b"), EntityId("foreign")}, EntityId("mesh")}, "owner");
        reject({input.node_ids, EntityId("other")}, "mesh-mismatch");
        reject({input.node_ids, EntityId("unknown")}, "mesh-missing");
        reject({input.node_ids, EntityId("material")}, "mesh-type");
        original_context = context(app, "create-tri3");
        const auto created =
            good(registry.invoke("mesh.create_tri3",
                                 original_context,
                                 InputTraits<MeshCreateTri3Input>::to_value(input)));
        triangle = EntityId(
            std::get<std::string>(std::get<Value::Object>(created.data).at("entity_id").data));
        const auto after = snapshot();
        check(after.info.revision == before.info.revision + 1 &&
                  diff_record_views(before.records, after.records).records.size() == 1,
              "Tri3 creation must publish exactly one record and one transaction");
        const auto record = after.records.find<records::Tri3>(triangle);
        check(record && record->get<records::Tri3>().nodes ==
                            std::array<EntityId, 3>{EntityId("a"), EntityId("b"), EntityId("c")},
              "Tri3 must persist ordered connectivity");
        auto encoded = RecordTraits<records::Tri3>::descriptor().encode(record->object());
        encoded.version = 2;
        try {
            options.registry->from_input(encoded);
            check(false, "Future Tri3 schema must be rejected");
        } catch (const RecordError& error) {
            check(error.code() == ErrorCode::schema_unsupported, "Future schema error");
        }
        const auto generation = options.records->load_rows().generation;
        const auto replay =
            good(registry.invoke("mesh.create_tri3",
                                 original_context,
                                 InputTraits<MeshCreateTri3Input>::to_value(input)));
        check(std::get<Value::Object>(replay.data).at("transaction_id") ==
                      std::get<Value::Object>(created.data).at("transaction_id") &&
                  options.records->load_rows().generation == generation,
              "Retry must return the original transaction without publication");
        auto changed = input;
        std::swap(changed.node_ids[0], changed.node_ids[1]);
        bad(registry.invoke("mesh.create_tri3",
                            original_context,
                            InputTraits<MeshCreateTri3Input>::to_value(changed)),
            ErrorCode::idempotency_key_conflict);
        auto stale = context(app, "stale");
        stale.expected_revision = before.info.revision;
        bad(registry.invoke(
                "mesh.create_tri3", stale, InputTraits<MeshCreateTri3Input>::to_value(input)),
            ErrorCode::revision_conflict);
        stale = context(app, "epoch");
        stale.document->epoch = DocumentEpoch("old");
        bad(registry.invoke(
                "mesh.create_tri3", stale, InputTraits<MeshCreateTri3Input>::to_value(input)),
            ErrorCode::document_epoch_expired);
        for (const RecordKey& key : {RecordKey{RecordTraits<records::Node>::type_id, "a"},
                                     RecordKey{RecordTraits<records::Mesh>::type_id, "mesh"}}) {
            const auto removed = app.execute(
                caller,
                at(after.info),
                "test.erase",
                key.identity,
                [key](const DocumentView& view, const RecordIdentityAllocator&) {
                    EditSession edit(view);
                    edit.erase(key);
                    return Result<RecordPreparedOperation>{
                        Status::success,
                        RecordPreparedOperation{
                            edit.prepare(), "Erase", EntityId{}, key.identity, 0, false},
                        {}};
                },
                "erase-" + key.identity);
            check(!removed.ok() && diff_record_views(after.records, snapshot().records).empty(),
                  "Referenced node/mesh deletion must leave no dangling topology");
        }
        bad(registry.invoke("node.move",
                            context(app, "collapse"),
                            InputTraits<NodeMoveInput>::to_value({EntityId("c"), {1, 0, 0}})),
            ErrorCode::invalid_input);
        check(snapshot().info.revision == after.info.revision &&
                  options.records->load_rows().generation == generation,
              "Dependent area validation must atomically reject node collapse");
        ViewSession view;
        view.id = "tri3-view";
        view.document = after.info.document;
        view.model_revision = after.info.revision;
        view.view_revision = 1;
        RenderProjector projector;
        const auto render = good(projector.update(after.records, view));
        check(render.full && render.full->cells.size() == 1 &&
                  render.full->cells[0].kind == RenderCellKind::polygon &&
                  render.full->cells[0].points.size() == 3 &&
                  render.full->cells[0].entity == triangle && !render.legacy_line2_compatible,
              "Production render contribution must project Tri3 to a three-point polygon");
        bad(validate_nastran_export(after.records, EntityId("analysis"), NastranCodec{}),
            ErrorCode::unsupported_capability);
        bad(features::analysis::freeze_analysis_input(
                after.records, EntityId("analysis"), ProfileRef{}, {}),
            ErrorCode::unsupported_capability);
        bad(features::analysis::RuleCatalog{}.check(after.records, EntityId("analysis")),
            ErrorCode::unsupported_capability);
        good(app.undo(caller, at(after.info), "undo"));
        check(diff_record_views(before.records, snapshot().records).empty(),
              "Undo removes only Tri3");
        good(registry.invoke("mesh.create_tri3",
                             original_context,
                             InputTraits<MeshCreateTri3Input>::to_value(input)));
        check(diff_record_views(before.records, snapshot().records).empty(),
              "Retry after undo must not reapply Tri3");
        good(app.redo(caller, at(snapshot().info), "redo"));
        check(diff_record_views(after.records, snapshot().records).empty(),
              "Redo restores all records");
#ifdef QCAE_TEST_SQLITE
        info = good(app.save_document(
            caller, at(snapshot().info), (directory / "tri3.qcae").string(), false, "save"));
        good(app.close_document(caller, at(info), ClosePolicy::discard, "close"));
        const auto opened =
            good(app.open_document(caller, (directory / "tri3.qcae").string(), "open"));
        check(opened.document.id != info.document.id && opened.revision == 0 &&
                  diff_record_views(after.records, snapshot().records).empty(),
              "Normal open must renew document and preserve every Tri3 field/reference");
#endif
        final = snapshot().info;
        expected = snapshot().records;
    }
#ifdef QCAE_TEST_SQLITE
    options.projects.reset();
    options.records.reset();
    sqlite.reset();
    sqlite = std::make_shared<SqliteWorkspaceStore>((directory / "work.sqlite").string());
    options.records = sqlite;
    options.projects = sqlite;
#endif
    RecordApplication recovered(options);
    const auto info = good(recovered.recover_document(caller, "recover"));
    check(info.document.id == final.document.id && info.document.epoch != final.document.epoch &&
              info.revision == final.revision &&
              diff_record_views(expected, good(recovered.snapshot(info.document)).records).empty(),
          "Recovery must renew epoch while retaining all Tri3 records and revision");
    std::cout << "PASS: Tri3 typed registry, atomic references/area/mesh, history, render, "
                 "unsupported analysis/export, recovery";
#ifdef QCAE_TEST_SQLITE
    std::cout << ", actual SQLite save/normal open/reopen";
#endif
    std::cout << '\n';
}
} // namespace
int main() {
    try {
        run();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
