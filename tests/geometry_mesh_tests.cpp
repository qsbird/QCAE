#include "runtime_test_support.hpp"
#include "qcae/geometry_features.hpp"
#include "qcae/line_mesh_task.hpp"
#include "qcae/records_model_bridge.hpp"
#include <iostream>
#ifdef QCAE_TEST_SQLITE
#include "qcae/sqlite_store.hpp"
#include <filesystem>
#include <random>
#endif

using namespace runtime_test;
namespace {
#ifdef QCAE_TEST_SQLITE
constexpr const char* backend = "sqlite";
#else
constexpr const char* backend = "memory";
#endif
struct FixtureStore {
#ifdef QCAE_TEST_SQLITE
    std::filesystem::path directory;
    std::shared_ptr<IRecordStore> records;
    FixtureStore() {
        do {
            directory = std::filesystem::temp_directory_path() /
                        ("qcae-mesh-c2-" + std::to_string(std::random_device{}()));
        } while (!std::filesystem::create_directory(directory));
        records = std::make_shared<SqliteWorkspaceStore>((directory / "workspace.sqlite").string());
    }
    ~FixtureStore() {
        records.reset();
        std::filesystem::remove_all(directory);
    }
    void reopen() {
        records.reset();
        records = std::make_shared<SqliteWorkspaceStore>((directory / "workspace.sqlite").string());
    }
#else
    std::shared_ptr<IRecordStore> records = std::make_shared<Store>();
    void reopen() {}
#endif
};
void one_run(unsigned mode, unsigned repetition) {
    FixtureStore store;
    TaskRecord result;
    DocumentInfo final_info;
    Revision task_model_commits{};
    records::GeometryId geometry;
    const LineGeometryInput input{{0, 0, 0}, {1000, 0, 0}};
    {
        RecordApplication app(options(store.records));
        auto info = good(app.create_document(caller, "Line", "create"));
        const auto line_receipt = good(app.execute(caller,
                                                   at(info),
                                                   "geometry.create_line",
                                                   line_geometry_signature(input),
                                                   create_line_handler(input),
                                                   "line"));
        geometry = records::GeometryId(line_receipt.primary_entity.value);
        check(!geometry.value.empty(), "geometry operation returns a stable primary entity");
        info = good(app.current_document());
        const auto snapshot = good(app.snapshot(info.document));
        check(bool(snapshot.records.find<records::GeometryLine>(geometry)),
              "primary entity identifies the created geometry");
        TaskService service(publisher(app));
        auto request = line_mesh_task(snapshot, caller, {}, {geometry, 10, std::nullopt}, "mesh");
        auto gate = std::make_shared<Gate>();
        if (mode) {
            auto work = std::move(request.work);
            request.work = [gate, work](const TaskControl& control) {
                gate->arrive_and_wait();
                return work(control);
            };
        }
        const auto task = good(service.start(std::move(request)));
        if (mode)
            gate->wait_for();
        if (mode == 1)
            check(good(service.cancel(caller, task.id)).accepted,
                  "mesh cancellation accepted before publication");
        if (mode == 2) {
            const std::array<double, 3> endpoint{1200, 0, 0};
            good(app.execute(caller,
                             at(info),
                             "geometry.move_endpoint",
                             line_endpoint_signature(geometry, endpoint),
                             move_line_endpoint_handler(geometry, endpoint),
                             "move-before-task"));
        }
        if (mode)
            gate->release();
        result = good(service.wait(caller, task.id));
        info = good(app.current_document());
        const auto current = good(app.snapshot(info.document));
        if (!mode) {
            check(result.state == TaskState::succeeded && result.receipt,
                  "real line task committed");
            check(current.records.count(RecordTraits<records::Node>::type_id) == 11 &&
                      current.records.count(RecordTraits<records::Beam>::type_id) == 10,
                  "uniform mesher generated eleven nodes and ten line elements");
            const auto mesh = line_mesh_identity(task.id);
            check(result.receipt->primary_entity.value == mesh.value,
                  "task receipt retains primary MeshId");
            const auto generation = store.records->load_rows().generation;
            check(!good(service.cancel(caller, task.id)).accepted,
                  "post-commit cancellation returns committed fact");
            check(store.records->load_rows().generation == generation,
                  "post-commit cancellation adds no transaction");
            const auto binding = current.records.find<records::Mesh>(mesh);
            check(binding && binding->get<records::Mesh>().geometry == geometry &&
                      !binding->get<records::Mesh>().stale,
                  "mesh provenance and distinct geometry identity retained");
            for (unsigned index = 0; index <= 10; ++index) {
                const auto node = current.records.find<records::Node>(
                    EntityId("node-" + task.id + "-" + std::to_string(index)));
                check(node && node->get<records::Node>().position ==
                                  std::array<double, 3>{100.0 * index, 0, 0},
                      "node coordinate golden");
                check(node->get<records::Node>().mesh == mesh, "node mesh ownership");
                if (index < 10) {
                    const auto line = current.records.find<records::Beam>(
                        EntityId("line-" + task.id + "-" + std::to_string(index)));
                    check(line && !line->get<records::Beam>().section &&
                              line->get<records::Beam>().mesh == mesh,
                          "line topology exists before optional physics assignment");
                    check(line->get<records::Beam>().nodes[0] == node->get<records::Node>().id &&
                              line->get<records::Beam>().nodes[1] ==
                                  EntityId("node-" + task.id + "-" + std::to_string(index + 1)),
                          "line connection golden");
                }
            }
            check(info.revision == snapshot.info.revision + 1,
                  "meshing is exactly one model transaction");
            check(!validate_model(model_from_records(current.records)).empty(),
                  "legacy strict export validation rejects unassigned physics properties");
        } else {
            check(result.state == (mode == 1 ? TaskState::cancelled : TaskState::conflicted),
                  "cancelled/stale task terminal state");
            check(current.records.count(RecordTraits<records::Node>::type_id) == 0 &&
                      current.records.count(RecordTraits<records::Mesh>::type_id) == 0,
                  "cancelled/stale candidate does not publish any mesh records");
            check(info.revision == snapshot.info.revision + (mode == 2 ? 1 : 0),
                  "only the explicitly committed geometry edit advances revision");
        }
        final_info = info;
        task_model_commits = info.revision - snapshot.info.revision - (mode == 2 ? 1 : 0);
    }
    store.reopen();
    {
        RecordApplication app(options(store.records));
        const auto recovered = good(app.recover_document(caller, "recover"));
        check(recovered.document.id == final_info.document.id &&
                  recovered.document.epoch != final_info.document.epoch &&
                  recovered.revision == final_info.revision,
              "recovery retains document/revision and renews epoch");
        TaskService restored(publisher(app));
        const auto persisted = good(restored.query(caller, result.id));
        check(persisted.state == result.state && persisted.events.size() == result.events.size(),
              "terminal task facts survive a store reopen without rerun");
        const auto created =
            good(app.action_outcome(caller, recovered.document, "geometry.create_line", "line"));
        check(created.primary_entity.value == geometry.value,
              "geometry primary identity survives persisted operation replay");
        if (!mode) {
            const auto committed = good(app.action_outcome(
                caller, recovered.document, "mesh.generate_line", "task-publication:" + result.id));
            check(persisted.receipt &&
                      persisted.receipt->transaction == result.receipt->transaction &&
                      persisted.receipt->primary_entity == result.receipt->primary_entity &&
                      committed.replayed && committed.transaction == result.receipt->transaction &&
                      committed.primary_entity == result.receipt->primary_entity,
                  "task and operation primary identities survive persisted idempotency replay");
        }
    }
    std::cout << "{\"case\":\""
              << (mode == 0   ? "success"
                  : mode == 1 ? "cancel_before_commit"
                              : "stale_candidate")
              << "\",\"run_id\":\"" << backend << "-mesh-" << mode << '-' << repetition
              << "\",\"backend\":\"" << backend << "\",\"final_state\":\""
              << task_state_name(result.state) << "\",\"task_model_commits\":" << task_model_commits
              << "}\n";
}
void replay_after_source_undo() {
    FixtureStore store;
    RecordApplication app(options(store.records));
    auto info = good(app.create_document(caller, "Replay", "create-replay"));
    const records::GeometryId geometry("replay-geometry");
    const EntityId section("replay-section");
    good(app.execute(
        caller,
        at(info),
        "test.geometry_with_section",
        "replay-fixture",
        [geometry, section](const DocumentView& view,
                            const RecordIdentityAllocator&) -> Result<RecordPreparedOperation> {
            EditSession edit(view);
            edit.put(records::GeometryLine{geometry, {0, 0, 0}, {1000, 0, 0}, 1});
            edit.put(records::Material{EntityId("replay-material"), "Steel", 210000, .3});
            edit.put(records::BeamSection{
                section, "Section", EntityId("replay-material"), 100, 833.333, 833.333, 1400});
            return {Status::success,
                    RecordPreparedOperation{edit.prepare(),
                                            "Replay fixture",
                                            EntityId(geometry.value),
                                            "replay-fixture",
                                            0,
                                            true},
                    {}};
        },
        "replay-fixture"));
    info = good(app.current_document());
    TaskService service(publisher(app));
    const auto first = good(service.start(line_mesh_task(
        good(app.snapshot(info.document)), caller, {}, {geometry, 10, section}, "mesh-replay")));
    const auto complete = good(service.wait(caller, first.id));
    check(complete.state == TaskState::succeeded && complete.receipt,
          "replay fixture task committed before undo");
    good(app.undo(caller, at(good(app.current_document())), "undo-replay-mesh"));
    good(app.undo(caller, at(good(app.current_document())), "undo-replay-sources"));
    const auto undone = good(app.current_document());
    const auto snapshot = good(app.snapshot(undone.document));
    check(snapshot.records.size() == 0, "undo removed mesh, geometry and section");
    auto retry = line_mesh_task(snapshot, caller, {}, {geometry, 10, section}, "mesh-replay");
    retry.input.revision = first.input.revision;
    const auto generation = store.records->load_rows().generation;
    const auto replayed = good(service.start(std::move(retry)));
    check(replayed.id == first.id && replayed.receipt &&
              replayed.receipt->transaction == complete.receipt->transaction &&
              replayed.receipt->primary_entity == complete.receipt->primary_entity &&
              good(app.current_document()).revision == undone.revision &&
              store.records->load_rows().generation == generation,
          "task replay survives deleted source references without executing or committing");
    auto changed = line_mesh_task(snapshot, caller, {}, {geometry, 9, section}, "mesh-replay");
    changed.input.revision = first.input.revision;
    const auto conflict = service.start(std::move(changed));
    check(!conflict.ok() && conflict.error->code == ErrorCode::idempotency_key_conflict,
          "changed retry is rejected before source-reference lookup");
    const auto missing = good(service.start(
        line_mesh_task(snapshot, caller, {}, {geometry, 10, section}, "missing-sources")));
    check(good(service.wait(caller, missing.id)).state == TaskState::failed &&
              good(app.current_document()).revision == undone.revision &&
              good(app.snapshot(undone.document)).records.size() == 0,
          "new work with missing sources fails without a model transaction");
    std::cout << "PASS: task replay after undo removes geometry and section; new missing-source "
                 "work fails\n";
}
void history_roundtrip() {
    FixtureStore store;
    RecordApplication app(options(store.records));
    auto info = good(app.create_document(caller, "History", "create-history"));
    const LineGeometryInput input{{0, 0, 0}, {1000, 0, 0}};
    const auto line = good(app.execute(caller,
                                       at(info),
                                       "geometry.create_line",
                                       line_geometry_signature(input),
                                       create_line_handler(input),
                                       "history-line"));
    info = good(app.current_document());
    TaskService service(publisher(app));
    const auto started = good(service.start(
        line_mesh_task(good(app.snapshot(info.document)),
                       caller,
                       {},
                       {records::GeometryId(line.primary_entity.value), 10, std::nullopt},
                       "history-mesh")));
    const auto mesh_task = good(service.wait(caller, started.id));
    check(mesh_task.state == TaskState::succeeded, "history fixture mesh succeeded");
    info = good(app.current_document());
    const auto before = good(app.snapshot(info.document)).records;
    const auto start_revision = info.revision;
    const EntityId node("node-" + started.id + "-5");
    const auto moved = good(app.execute(
        caller,
        at(info),
        "test.move_node",
        "node5-y1",
        [node](const DocumentView& view,
               const RecordIdentityAllocator&) -> Result<RecordPreparedOperation> {
            EditSession edit(view);
            edit.update<records::Node>(node, [](auto& value) { value.position[1] = 1; });
            return {
                Status::success,
                RecordPreparedOperation{edit.prepare(), "Move node 5", node, "node5-y1", 0, false},
                {}};
        },
        "history-move"));
    const auto after = good(app.snapshot(info.document)).records;
    check(diff_record_views(before, after).records.size() == 1,
          "node movement changes exactly one record");
    for (unsigned run = 0; run < 100; ++run) {
        good(app.undo(
            caller, at(good(app.current_document())), "history-undo-" + std::to_string(run)));
        check(diff_record_views(before, good(app.snapshot(info.document)).records).empty(),
              "undo equals complete pre-move golden");
        good(app.redo(
            caller, at(good(app.current_document())), "history-redo-" + std::to_string(run)));
        check(diff_record_views(after, good(app.snapshot(info.document)).records).empty(),
              "redo equals complete post-move golden");
    }
    const auto final_info = good(app.current_document());
    check(final_info.revision == start_revision + 201,
          "one edit and 100 undo/redo rounds advance revision exactly 201");
    const auto generation = store.records->load_rows().generation;
    const auto replay =
        good(app.action_outcome(caller, final_info.document, "test.move_node", "history-move"));
    check(replay.primary_entity == node && replay.transaction == moved.transaction &&
              replay.replayed && generation == store.records->load_rows().generation,
          "history preserves the original primary entity and transaction fact");
    std::cout << "PASS: 100 undo/redo rounds; semantic differences=0; revision_delta=201\n";
}
} // namespace
int main() {
    try {
        for (unsigned mode = 0; mode < 3; ++mode)
            for (unsigned run = 0; run < 10; ++run)
                one_run(mode, run);
        replay_after_source_undo();
        history_roundtrip();
        std::cout << "PASS: 10 success + 10 cancellation + 10 stale real mesh tasks\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
