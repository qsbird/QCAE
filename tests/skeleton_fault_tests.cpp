#include "runtime_test_support.hpp"
#include "qcae/geometry_features.hpp"
#include "qcae/line_mesh_task.hpp"
#include "qcae/quantities.hpp"
#include "qcae/sqlite_store.hpp"
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <spawn.h>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
using namespace runtime_test;
namespace {
namespace fs = std::filesystem;
std::string executable;
std::string json_string(std::string_view value) {
    std::string out = "\"";
    for (const unsigned char c : value) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c < 32 || c >= 127) {
            constexpr char hex[] = "0123456789abcdef";
            out += "\\u00";
            out += hex[c / 16];
            out += hex[c % 16];
        } else
            out += static_cast<char>(c);
    }
    return out + '"';
}
struct Sandbox {
    fs::path path;
    Sandbox() {
        auto pattern = (fs::temp_directory_path() / "qcae-sk-fault-XXXXXX").string();
        auto* result = ::mkdtemp(pattern.data());
        check(result, "mkdtemp failed");
        path = result;
    }
    ~Sandbox() {
        fs::remove_all(path);
    }
    std::string database() const {
        return (path / "workspace.sqlite").string();
    }
    std::string project() const {
        return (path / "M.qcae").string();
    }
};
struct ReleaseGate {
    std::shared_ptr<Gate> gate;
    ~ReleaseGate() {
        gate->release();
    }
};
struct Audit {
    std::string input;
    std::vector<std::pair<std::string, bool>> assertions;
    std::map<std::string, std::string> actual;
    void require(bool passed, std::string label) {
        assertions.emplace_back(label, passed);
        if (!passed)
            throw std::runtime_error(label);
    }
    std::string json(std::string_view id,
                     std::string_view run,
                     std::string_view source,
                     std::string_view error) const {
        std::ostringstream out;
        out << "{\"case_id\":" << json_string(id) << ",\"run_id\":" << json_string(run)
            << ",\"source_tree_sha256\":" << json_string(source)
            << ",\"input\":" << json_string(input)
            << ",\"passed\":" << (error.empty() ? "true" : "false")
            << ",\"error\":" << json_string(error) << ",\"assertions\":[";
        bool comma{};
        for (const auto& [label, passed] : assertions) {
            if (comma)
                out << ',';
            out << "{\"expected\":" << json_string(label)
                << ",\"actual\":" << (passed ? "true" : "false")
                << ",\"passed\":" << (passed ? "true" : "false") << '}';
            comma = true;
        }
        out << "],\"actual\":{";
        comma = false;
        for (const auto& [key, value] : actual) {
            if (comma)
                out << ',';
            out << json_string(key) << ':' << json_string(value);
            comma = true;
        }
        return out.str() + "}}";
    }
};
std::string row_bytes(const LoadedRows& rows) {
    return encode_record_rows(rows.rows);
}
std::string model_bytes(const DocumentView& view) {
    std::string result;
    view.visit([&](const Record& record) {
        result += std::to_string(record->encoded().size()) + ':' + record->encoded();
    });
    return result;
}
std::string history_bytes(const HistorySnapshot& history) {
    std::string out = std::to_string(history.cursor) + ':' + std::to_string(history.revision);
    for (const auto& item : history.items)
        out += ':' + item.transaction.value + ':' + item.label + ':' + (item.applied ? "1" : "0");
    return out;
}
struct State {
    DocumentInfo info;
    std::string model, history, rows;
    std::uint64_t generation{};
};
State capture(RecordApplication& app, IRecordStore& store) {
    const auto info = good(app.current_document());
    const auto rows = store.load_rows();
    return {info,
            model_bytes(good(app.snapshot(info.document)).records),
            history_bytes(good(app.history(info.document))),
            row_bytes(rows),
            rows.generation};
}
void unchanged(Audit& audit, const State& before, const State& after, bool durable = true) {
    audit.actual["before_model"] = before.model;
    audit.actual["after_model"] = after.model;
    audit.actual["before_history"] = before.history;
    audit.actual["after_history"] = after.history;
    audit.actual["before_revision"] = std::to_string(before.info.revision);
    audit.actual["after_revision"] = std::to_string(after.info.revision);
    audit.require(before.model == after.model, "Committed model equals the pre-fault model");
    audit.require(before.history == after.history,
                  "History including cursor and transaction IDs is unchanged");
    audit.require(before.info.revision == after.info.revision, "Model revision is unchanged");
    if (durable) {
        audit.actual["before_persistent_rows"] = before.rows;
        audit.actual["after_persistent_rows"] = after.rows;
        audit.require(
            before.rows == after.rows && before.generation == after.generation,
            "Persistent model/history/idempotency rows and store generation are unchanged");
    }
}
template <class T>
void rejected(Audit& audit,
              const Result<T>& result,
              ErrorCode code,
              Status status = Status::failed) {
    constexpr std::array error_names{"MISSING_INPUT",
                                     "INVALID_INPUT",
                                     "INVALID_UNIT",
                                     "ENTITY_NOT_FOUND",
                                     "DOCUMENT_NOT_FOUND",
                                     "DOCUMENT_ALREADY_OPEN",
                                     "DOCUMENT_EPOCH_EXPIRED",
                                     "REVISION_CONFLICT",
                                     "PREVIEW_EXPIRED",
                                     "IDEMPOTENCY_KEY_CONFLICT",
                                     "NOTHING_TO_UNDO",
                                     "NOTHING_TO_REDO",
                                     "RESOURCE_LIMIT",
                                     "UNSUPPORTED_CAPABILITY",
                                     "STORAGE_FAILURE",
                                     "STORAGE_UNCERTAIN",
                                     "SCHEMA_UNSUPPORTED"};
    audit.actual["error_code"] =
        result.error ? error_names.at(static_cast<std::size_t>(result.error->code)) : "none";
    audit.actual["error_message"] = result.error ? result.error->message : "none";
    audit.require(!result.ok() && result.status == status && result.error &&
                      result.error->code == code,
                  "Fault returns the specified status/error and no success value");
}
RecordApplicationOptions sqlite_options(const std::shared_ptr<SqliteWorkspaceStore>& store) {
    auto result = options(store);
    result.projects = store;
    return result;
}
ChangeReceipt change(RecordApplication& app, std::string key, double value = 70000) {
    return good(app.execute(caller,
                            at(good(app.current_document())),
                            "test.material",
                            modulus_signature(value),
                            material_change(value),
                            key));
}
void setup(RecordApplication& app) {
    seed(app);
}
int spawn(std::vector<std::string> words) {
    words.insert(words.begin(), executable);
    std::vector<char*> arguments;
    for (auto& word : words)
        arguments.push_back(word.data());
    arguments.push_back(nullptr);
    pid_t child{};
    const auto error =
        ::posix_spawn(&child, executable.c_str(), nullptr, nullptr, arguments.data(), environ);
    if (error)
        throw std::runtime_error(std::strerror(error));
    int status{};
    pid_t waited{};
    do {
        waited = ::waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    check(waited == child && WIFEXITED(status),
          "Child did not exit normally at deterministic hook");
    return WEXITSTATUS(status);
}
int child(int argc, char** argv) {
    if (argc == 4 && std::string(argv[1]) == "--lock") {
        try {
            if (std::string(argv[2]) == "workspace") {
                SqliteWorkspaceStore second(argv[3]);
            } else {
                Sandbox local;
                SqliteWorkspaceStore second(local.database());
                second.acquire_project(argv[3]);
            }
        } catch (const StorageError&) {
            return 42;
        }
        return 0;
    }
    check(argc == 5 && std::string(argv[1]) == "--crash", "Invalid child arguments");
    bool armed{};
    const std::string milestone = argv[3];
    StoreOptions faults;
    faults.fault = [&](const std::string& point) {
        if (armed && point == milestone)
            ::_Exit(86);
    };
    auto store = std::make_shared<SqliteWorkspaceStore>(argv[2], faults);
    RecordApplication app(sqlite_options(store));
    const auto info = good(app.recover_document(caller, "child-recover"));
    {
        std::ofstream context(std::string(argv[2]) + ".child-context");
        context << info.document.id.value << '\n'
                << info.document.epoch.value << '\n'
                << info.revision << '\n';
        context.flush();
        check(bool(context), "Cannot record child request context");
    }
    armed = true;
    if (milestone == "after_db_commit")
        change(app, "crash-operation");
    else
        good(app.save_document(caller, at(info), argv[4], false, "crash-save"));
    return 2;
}
void ordinary_fault(unsigned id, Audit& audit) {
    Sandbox sandbox;
    bool armed{};
    StoreOptions hook;
    hook.fault = [&](const std::string& point) {
        if (armed && point == "before_db_commit") {
            armed = false;
            throw std::runtime_error("F08 deterministic before_db_commit");
        }
    };
    auto store = std::make_shared<SqliteWorkspaceStore>(sandbox.database(), hook);
    RecordApplication app(sqlite_options(store));
    setup(app);
    auto before = capture(app, *store);
    audit.actual["document_id"] = before.info.document.id.value;
    if (id == 1) {
        audit.input = "material.set_young_modulus; value=70; unit omitted";
        auto result = app.execute(
            caller,
            at(before.info),
            "test.unit",
            "70:missing-unit",
            [](const DocumentView&,
               const RecordIdentityAllocator&) -> Result<RecordPreparedOperation> {
                const auto quantity =
                    parameters::canonical_quantity({70, ""}, parameters::Dimension::pressure);
                return {quantity.status, {}, quantity.error};
            },
            "missing-unit");
        rejected(audit, result, ErrorCode::missing_input, Status::needs_input);
    } else if (id == 2) {
        audit.input =
            "BeamSection.material references first absent identity then existing Node identity";
        good(app.execute(
            caller,
            at(before.info),
            "seed.node",
            "node",
            [](const DocumentView& view, const RecordIdentityAllocator&) {
                EditSession edit(view);
                edit.put(records::Node{EntityId("node"), {0, 0, 0}, {}});
                return Result<RecordPreparedOperation>{
                    Status::success,
                    RecordPreparedOperation{
                        edit.prepare(), "Node", EntityId("node"), "node", 0, true},
                    {}};
            },
            "node"));
        before = capture(app, *store);
        for (const auto& reference : {"absent", "node"}) {
            const auto result = app.execute(
                caller,
                at(before.info),
                "section.create",
                reference,
                [reference](const DocumentView& view, const RecordIdentityAllocator&) {
                    EditSession edit(view);
                    edit.put(records::BeamSection{EntityId("section"),
                                                  "Section",
                                                  EntityId(reference),
                                                  100,
                                                  833.333,
                                                  833.333,
                                                  1400});
                    return Result<RecordPreparedOperation>{
                        Status::success,
                        RecordPreparedOperation{edit.prepare(),
                                                "Bad reference",
                                                EntityId("section"),
                                                reference,
                                                0,
                                                true},
                        {}};
                },
                reference);
            audit.require(!result.ok() && result.error,
                          "Missing or wrong-type material reference is rejected");
        }
    } else if (id == 3) {
        audit.input = "Prepare modulus=70000 at revision 1; commit independent modulus=100000; "
                      "submit first preview at revision 1";
        const auto preview = good(app.preview(caller, at(before.info), material_change(70000)));
        change(app, "prior", 100000);
        const auto context = before.info;
        before = capture(app, *store);
        rejected(audit,
                 app.commit(caller, at(context), preview.id, "stale"),
                 ErrorCode::revision_conflict,
                 Status::conflict);
    } else if (id == 5 || id == 6 || id == 7) {
        const auto context = before.info;
        const auto first = change(app, "original");
        if (id == 7)
            good(app.undo(caller, at(good(app.current_document())), "undo-original"));
        before = capture(app, *store);
        audit.input = id == 5 ? "Discard success response; retry original key and parameters"
                      : id == 6
                          ? "Retry original key with modulus changed to 90000"
                          : "Undo successful modulus change; resend original key and parameters";
        const auto retry = app.execute(caller,
                                       at(context),
                                       "test.material",
                                       modulus_signature(id == 6 ? 90000 : 70000),
                                       material_change(id == 6 ? 90000 : 70000),
                                       "original");
        if (id == 6)
            rejected(audit, retry, ErrorCode::idempotency_key_conflict, Status::conflict);
        else {
            const auto receipt = good(retry);
            audit.actual["original_transaction"] = first.transaction.value;
            audit.actual["retry_transaction"] = receipt.transaction.value;
            audit.require(receipt.replayed && receipt.transaction == first.transaction &&
                              receipt.committed_revision == first.committed_revision,
                          "Retry returns the original successful transaction fact");
        }
    } else if (id == 8) {
        audit.input = "StoreOptions::fault throws at before_db_commit for the real application "
                      "modulus transaction";
        const auto preview = good(app.preview(caller, at(before.info), material_change(70000)));
        // Preview is transient; compare the persistent image immediately before the fault.
        before = capture(app, *store);
        armed = true;
        rejected(audit,
                 app.commit(caller, at(before.info), preview.id, "db-fault"),
                 ErrorCode::storage_failure);
        audit.require(!app.operation(caller, before.info.document, "commit", "db-fault").ok(),
                      "Failed commit has no durable successful idempotency fact");
    }
    unchanged(audit, before, capture(app, *store));
}
void epoch_fault(Audit& audit) {
    Sandbox sandbox;
    auto store = std::make_shared<SqliteWorkspaceStore>(sandbox.database());
    DocumentInfo old;
    {
        RecordApplication app(sqlite_options(store));
        setup(app);
        old = good(app.current_document());
    }
    RecordApplication recovered(sqlite_options(store));
    const auto info = good(recovered.recover_document(caller, "recover"));
    const auto before = capture(recovered, *store);
    audit.input = "Recover real application; submit operation with the previous DocumentEpoch";
    audit.require(info.document.id == old.document.id && info.document.epoch != old.document.epoch,
                  "Recovery retains DocumentId and renews Epoch");
    rejected(
        audit,
        recovered.execute(
            caller, at(old), "test.material", "old-epoch", material_change(70000), "old-epoch"),
        ErrorCode::document_epoch_expired,
        Status::conflict);
    unchanged(audit, before, capture(recovered, *store));
}
void crash_fault(unsigned id, Audit& audit) {
    Sandbox sandbox;
    State before;
    {
        auto store = std::make_shared<SqliteWorkspaceStore>(sandbox.database());
        RecordApplication app(sqlite_options(store));
        setup(app);
        before = capture(app, *store);
    }
    const auto point = id == 9 ? "after_db_commit" : "after_project_publish";
    audit.input = std::string("posix_spawn/exec child; _Exit(86) at ") + point;
    const auto exit = spawn({"--crash", sandbox.database(), point, sandbox.project()});
    audit.actual["child_exit_code"] = std::to_string(exit);
    audit.require(exit == 86, "Real child terminated at the deterministic SQLite/publication hook");
    DocumentInfo child_info;
    {
        std::ifstream context(sandbox.database() + ".child-context");
        context >> child_info.document.id.value >> child_info.document.epoch.value >>
            child_info.revision;
        check(bool(context), "Missing original child request context");
    }
    audit.actual["child_document_id"] = child_info.document.id.value;
    audit.actual["child_document_epoch"] = child_info.document.epoch.value;
    audit.actual["child_expected_revision"] = std::to_string(child_info.revision);
    audit.actual["original_idempotency_key"] = id == 9 ? "crash-operation" : "crash-save";
    auto store = std::make_shared<SqliteWorkspaceStore>(sandbox.database());
    const auto stored = store->load_rows();
    auto image = decode_record_state_image(
        stored.rows, make_record_registry(), {}, std::array{task_row_handler()});
    audit.actual["before_model"] = before.model;
    audit.actual["before_history"] = before.history;
    if (id == 10) {
        audit.require(bool(image.save_intent),
                      "Uncompleted save intent is durable after process termination");
        const auto project = store->read_project(store->acquire_project(sandbox.project()));
        audit.actual["intent_token"] = image.save_intent->token;
        audit.actual["project_token"] = project.save_token;
        audit.require(
            project.save_token == image.save_intent->token &&
                project.payload == image.save_intent->snapshot,
            "Published file token and full snapshot exactly match the durable save intent");
    }
    RecordApplication app(sqlite_options(store));
    const auto recovered = good(app.recover_document(caller, "parent-recover"));
    audit.require(recovered.document.id == before.info.document.id &&
                      recovered.document.epoch != before.info.document.epoch,
                  "Recovery preserves the document and starts a new epoch");
    const auto after = capture(app, *store);
    audit.actual["after_model"] = after.model;
    audit.actual["after_history"] = after.history;
    audit.actual["after_revision"] = std::to_string(after.info.revision);
    if (id == 9) {
        const auto snapshot = good(app.snapshot(recovered.document));
        const auto original = good(
            app.action_outcome(caller, recovered.document, "test.material", "crash-operation"));
        audit.actual["recovered_transaction"] = original.transaction.value;
        audit.require(recovered.revision == before.info.revision + 1 &&
                          snapshot.records.find<records::Material>(EntityId("material"))
                                  ->get<records::Material>()
                                  .young_modulus_mpa == 70000,
                      "Recovered application contains exactly the new material transaction");
        const auto history = good(app.history(recovered.document));
        audit.require(history.items.size() == 2 && history.cursor == 2 &&
                          history.items.back().transaction == original.transaction,
                      "Recovered history contains the original transaction exactly once");
        const auto generation = store->load_rows().generation;
        const auto retry = good(
            app.action_outcome(caller, recovered.document, "test.material", "crash-operation"));
        audit.require(
            retry.replayed && retry.transaction == original.transaction &&
                store->load_rows().generation == generation,
            "Original key lookup returns the recovered fact without another store commit");
    } else {
        audit.require(recovered.revision == before.info.revision && after.model == before.model &&
                          after.history == before.history,
                      "Interrupted save preserves model and complete history");
        audit.require(!recovered.dirty &&
                          recovered.saved_path == fs::canonical(sandbox.project()).string() &&
                          recovered.saved_content_state == recovered.content_state,
                      "Recovery reconciles the matching file into an exact saved marker");
        const auto fact = good(app.host_operation(caller, "save_document", "crash-save"));
        const auto generation = store->load_rows().generation;
        const auto retry =
            good(app.save_document(caller, at(child_info), sandbox.project(), false, "crash-save"));
        audit.require(
            fact.project_id == recovered.project_id && retry.project_id == fact.project_id &&
                retry.saved_content_state == recovered.content_state &&
                store->load_rows().generation == generation,
            "Original save key remains queryable and replays without republishing or committing");
    }
}
void changed_save_target(Audit& audit) {
    Sandbox sandbox;
    bool armed{};
    StoreOptions hook;
    hook.fault = [&](const std::string& point) {
        if (armed && point == "before_project_publish") {
            armed = false;
            throw std::runtime_error("F11 publication held");
        }
    };
    auto store = std::make_shared<SqliteWorkspaceStore>(sandbox.database(), hook);
    State initial;
    {
        RecordApplication app(sqlite_options(store));
        setup(app);
        initial = capture(app, *store);
        armed = true;
        rejected(
            audit,
            app.save_document(caller, at(initial.info), sandbox.project(), false, "pending-save"),
            ErrorCode::storage_failure);
        const auto staged = capture(app, *store);
        // A second application creates a valid unrelated snapshot. Installing it is the fault
        // injection; normal application writes remain on their commit coordinator.
        Sandbox foreign;
        auto foreign_store = std::make_shared<SqliteWorkspaceStore>(foreign.database());
        RecordApplication other(sqlite_options(foreign_store));
        setup(other);
        auto foreign_info = good(other.current_document());
        good(other.save_document(
            caller, at(foreign_info), foreign.project(), false, "foreign-save"));
        const auto target =
            foreign_store->read_project(foreign_store->acquire_project(foreign.project()));
        store->publish_project(store->acquire_project(sandbox.project()), target);
        const auto result =
            app.save_document(caller, at(initial.info), sandbox.project(), false, "pending-save");
        audit.input = "Fail before project publish; replace target with a valid unrelated project "
                      "before same-key retry";
        audit.require(!result.ok(), "Changed target is rejected by the pending save retry");
        const auto changed_target = store->read_project(store->acquire_project(sandbox.project()));
        audit.require(changed_target.payload == target.payload &&
                          changed_target.save_token == target.save_token,
                      "Retry preserves the unrelated target token and full file bytes");
        unchanged(audit, staged, capture(app, *store));
    }
    RecordApplication restored(sqlite_options(store));
    const auto info = good(restored.recover_document(caller, "recover-target-change"));
    audit.require(
        model_bytes(good(restored.snapshot(info.document)).records) == initial.model &&
            info.saved_path.empty(),
        "Recovery retains the valid working model without claiming the changed target is saved");
    good(restored.save_document(
        caller, at(info), (sandbox.path / "safe.qcae").string(), false, "safe-save"));
    audit.require(!good(restored.current_document()).dirty,
                  "Recovered model can be explicitly saved elsewhere");
}
void second_writer(Audit& audit) {
    Sandbox sandbox;
    auto store = std::make_shared<SqliteWorkspaceStore>(sandbox.database());
    RecordApplication app(sqlite_options(store));
    setup(app);
    good(app.save_document(
        caller, at(good(app.current_document())), sandbox.project(), false, "save"));
    audit.input = "Second process attempts workspace and project paths, symlinks and hardlinks; "
                  "owner then commits";
    for (const auto& pair :
         {std::pair{"workspace", sandbox.database()}, std::pair{"project", sandbox.project()}}) {
        const auto symlink = sandbox.path / (std::string(pair.first) + "-symlink");
        const auto hardlink = sandbox.path / (std::string(pair.first) + "-hardlink");
        fs::create_symlink(pair.second, symlink);
        fs::create_hard_link(pair.second, hardlink);
        for (const auto& path : {pair.second, symlink.string(), hardlink.string()}) {
            const auto exit = spawn({"--lock", pair.first, path});
            audit.actual[std::string(pair.first) + ':' + fs::path(path).filename().string()] =
                std::to_string(exit);
            audit.require(exit == 42,
                          "Second writer is rejected for " + std::string(pair.first) + ':' +
                              fs::path(path).filename().string());
        }
    }
    const auto revision = good(app.current_document()).revision;
    change(app, "owner-still-commits");
    audit.require(good(app.current_document()).revision == revision + 1,
                  "Original holder can still commit exactly once");
    const auto current = good(app.current_document());
    good(app.save_document(caller, at(current), current.saved_path, false, "owner-saves"));
}
void task_fault(unsigned id, Audit& audit) {
    Sandbox sandbox;
    auto store = std::make_shared<SqliteWorkspaceStore>(sandbox.database());
    RecordApplication app(sqlite_options(store));
    auto info = good(app.create_document(caller, "M task", "create"));
    const LineGeometryInput line{{0, 0, 0}, {1000, 0, 0}};
    const auto receipt = good(app.execute(caller,
                                          at(info),
                                          "geometry.create_line",
                                          line_geometry_signature(line),
                                          create_line_handler(line),
                                          "line"));
    info = good(app.current_document());
    const auto snapshot = good(app.snapshot(info.document));
    const records::GeometryId geometry(receipt.primary_entity.value);
    TaskService service(publisher(app));
    auto request = line_mesh_task(snapshot, caller, {}, {geometry, 10, {}}, "fault-mesh");
    auto gate = std::make_shared<Gate>();
    ReleaseGate release_on_failure{gate};
    if (id != 14) {
        auto work = std::move(request.work);
        request.work = [gate, work](const TaskControl& control) {
            gate->arrive_and_wait();
            return work(control);
        };
    }
    const auto before = capture(app, *store);
    const auto task = good(service.start(std::move(request)));
    if (id != 14)
        gate->wait_for();
    if (id == 13) {
        audit.input = "Real line mesher held at deterministic worker gate; cancellation accepted "
                      "before commit";
        audit.require(good(service.cancel(caller, task.id)).accepted,
                      "Cancellation is accepted before publication");
    } else if (id == 15) {
        audit.input = "Hold mesher candidate at old revision; commit geometry endpoint=1200; "
                      "release candidate";
        const std::array<double, 3> end{1200, 0, 0};
        good(app.execute(caller,
                         at(info),
                         "geometry.move_endpoint",
                         line_endpoint_signature(geometry, end),
                         move_line_endpoint_handler(geometry, end),
                         "newer-input"));
    }
    if (id != 14)
        gate->release();
    const auto final = good(service.wait(caller, task.id));
    const auto after = capture(app, *store);
    audit.actual["task_id"] = task.id;
    audit.actual["task_state"] = task_state_name(final.state);
    audit.actual["before_model"] = before.model;
    audit.actual["after_model"] = after.model;
    audit.actual["after_history"] = after.history;
    if (id == 14) {
        audit.input = "Real mesher succeeds; cancel the already committed task";
        audit.require(final.state == TaskState::succeeded && final.receipt,
                      "Real mesher committed its 11-node/10-beam result");
        const auto cancel = good(service.cancel(caller, task.id));
        audit.require(!cancel.accepted && cancel.task.receipt &&
                          cancel.task.receipt->transaction == final.receipt->transaction,
                      "Late cancel returns the already committed fact");
        unchanged(audit, after, capture(app, *store));
        audit.require(after.info.revision == before.info.revision + 1,
                      "Committed mesh advances the model once");
    } else {
        audit.require(final.state == (id == 13 ? TaskState::cancelled : TaskState::conflicted),
                      "Task has the required terminal cancelled/conflicted state");
        const auto current = good(app.snapshot(after.info.document)).records;
        audit.require(current.count(RecordTraits<records::Node>::type_id) == 0 &&
                          current.count(RecordTraits<records::Beam>::type_id) == 0 &&
                          after.info.revision == before.info.revision + (id == 15 ? 1 : 0),
                      "Cancelled/stale candidate produces zero model transactions and cannot "
                      "overwrite current input");
        if (id == 13)
            unchanged(audit, before, after, false);
        else
            audit.require(
                current.find<records::GeometryLine>(geometry)->get<records::GeometryLine>().end ==
                    std::array<double, 3>{1200, 0, 0},
                "Newer endpoint remains committed after stale candidate rejection");
    }
    std::uint64_t sequence{};
    std::ostringstream event_observations;
    for (const auto& event : final.events) {
        audit.require(event.sequence > sequence,
                      "Task transition sequence increases monotonically");
        sequence = event.sequence;
        event_observations << event.sequence << ':' << task_state_name(event.state) << ':'
                           << event.progress << '\n';
    }
    audit.actual["task_transition_events"] = event_observations.str();
}
void damaged_project(Audit& audit) {
    Sandbox sandbox;
    auto store = std::make_shared<SqliteWorkspaceStore>(sandbox.database());
    const auto path = store->acquire_project(sandbox.project());
    audit.input = "Publish a corrupt project, then a future project-schema version; attempt normal "
                  "open from each";
    for (const auto& payload :
         {std::string("broken-project-bytes"), std::string("19:QCAE-RECORD-PROJECT999;invalid")}) {
        store->publish_project(store->acquire_project(path), {"bad-schema-token", payload});
        const auto rows = store->load_rows();
        RecordApplication app(sqlite_options(store));
        rejected(audit, app.open_document(caller, path, "open-bad"), ErrorCode::schema_unsupported);
        audit.require(!app.current_document().ok() && !app.recovery_available(),
                      "Rejected schema never activates an editable empty document");
        audit.require(row_bytes(rows) == row_bytes(store->load_rows()) &&
                          rows.generation == store->load_rows().generation,
                      "Rejected project open does not persist a replacement empty model");
    }
}
void incompatible_profile(Audit& audit) {
    Sandbox sandbox;
    auto store = std::make_shared<SqliteWorkspaceStore>(sandbox.database());
    DocumentInfo saved;
    {
        RecordApplication app(sqlite_options(store));
        setup(app);
        auto info = good(app.current_document());
        good(app.execute(
            caller,
            at(info),
            "analysis.create",
            "old-profile-semantics",
            [](const DocumentView& view, const RecordIdentityAllocator&) {
                EditSession edit(view);
                edit.put(records::AnalysisDefinition{
                    EntityId("analysis"), "Old semantics", {profile, "linear_static"}, {}, {}, {}});
                return Result<RecordPreparedOperation>{
                    Status::success,
                    RecordPreparedOperation{edit.prepare(),
                                            "Analysis",
                                            EntityId("analysis"),
                                            "old-profile-semantics",
                                            0,
                                            true},
                    {}};
            },
            "analysis"));
        saved = good(app.save_document(
            caller, at(good(app.current_document())), sandbox.project(), false, "save"));
    }
    auto settings = sqlite_options(store);
    settings.profiles_supported = [](const DocumentView& view) {
        bool supported = true;
        view.visit(RecordTraits<records::AnalysisDefinition>::type_id, [&](const Record& record) {
            supported =
                supported &&
                record->get<records::AnalysisDefinition>().target.profile.definition_digest ==
                    "new-incompatible-semantic-v2";
        });
        return supported;
    };
    RecordApplication app(settings);
    const auto rows = store->load_rows();
    audit.input = "Persist AnalysisDefinition with semantic-v1; activate adapter accepting only "
                  "incompatible semantic-v2";
    auto open_settings = settings;
    open_settings.records = std::make_shared<Store>();
    RecordApplication open_app(open_settings);
    rejected(audit,
             open_app.open_document(caller, sandbox.project(), "open-new-semantics"),
             ErrorCode::schema_unsupported);
    rejected(audit,
             app.recover_document(caller, "recover-new-semantics"),
             ErrorCode::schema_unsupported);
    audit.require(!app.current_document().ok(),
                  "Incompatible profile cannot activate an editable document");
    audit.require(
        row_bytes(rows) == row_bytes(store->load_rows()),
        "Profile rejection preserves all durable rows without rewriting profile semantics");
    audit.actual["persisted_profile_digest"] = profile.definition_digest;
    audit.actual["accepted_profile_digest"] = "new-incompatible-semantic-v2";
    (void)saved;
}
void quota_fault(Audit& audit) {
    audit.input =
        "Exercise max_entities=1, max_history_entries=1, and 2 running + 8 queued worker limits";
    for (const bool history_limit : {false, true}) {
        Sandbox sandbox;
        auto store = std::make_shared<SqliteWorkspaceStore>(sandbox.database());
        auto settings = sqlite_options(store);
        settings.limits.max_materials = 1;
        if (!history_limit)
            settings.limits.max_entities = 1;
        settings.material_count = [](const DocumentView& view) {
            return view.count(RecordTraits<records::Material>::type_id);
        };
        if (history_limit)
            settings.limits.max_history_entries = 1;
        RecordApplication app(settings);
        setup(app);
        const auto before = capture(app, *store);
        const auto result = app.execute(
            caller,
            at(before.info),
            "quota-operation",
            history_limit ? modulus_signature(70000) : "create",
            history_limit
                ? material_change(70000)
                : RecordPrepare{[](const DocumentView& view, const RecordIdentityAllocator&) {
                      EditSession edit(view);
                      edit.put(records::Material{EntityId("material-second"), "Other", 70000, .3});
                      return Result<RecordPreparedOperation>{
                          Status::success,
                          RecordPreparedOperation{edit.prepare(),
                                                  "Other",
                                                  EntityId("material-second"),
                                                  "create",
                                                  70000,
                                                  true},
                          {}};
                  }},
            "quota-operation");
        rejected(audit, result, ErrorCode::resource_limit);
        unchanged(audit, before, capture(app, *store));
    }
    Sandbox sandbox;
    auto store = std::make_shared<SqliteWorkspaceStore>(sandbox.database());
    RecordApplication app(sqlite_options(store));
    setup(app);
    TaskService service(publisher(app));
    auto gate = std::make_shared<Gate>();
    ReleaseGate release_on_failure{gate};
    std::vector<TaskRecord> tasks;
    for (unsigned i = 0; i < 2; ++i)
        tasks.push_back(
            good(service.start(material_task(app, "active-" + std::to_string(i), 70000, gate))));
    gate->wait_for(2);
    for (unsigned i = 0; i < 8; ++i)
        tasks.push_back(
            good(service.start(material_task(app, "queued-" + std::to_string(i), 70000, gate))));
    const auto before = capture(app, *store);
    const auto exceeded = service.start(material_task(app, "ninth-queued", 70000, gate));
    const bool bounds = service.active_workers() == 2 && service.queued_tasks() == 8;
    // Release the workers before an assertion can throw, so failing probes cannot strand a test.
    for (const auto& task : tasks)
        good(service.cancel(caller, task.id));
    gate->release();
    for (const auto& task : tasks)
        good(service.wait(caller, task.id));
    rejected(audit, exceeded, ErrorCode::resource_limit);
    audit.require(bounds, "Queue saturation contains exactly 2 active workers and 8 pending tasks");
    unchanged(audit, before, capture(app, *store), false);
}
void replacement_fault(Audit& audit) {
    Sandbox sandbox;
    auto store = std::make_shared<SqliteWorkspaceStore>(sandbox.database());
    RecordApplication app(sqlite_options(store));
    auto info = good(app.create_document(caller, "Unmapped replacement", "create"));
    const LineGeometryInput line{{0, 0, 0}, {1000, 0, 0}};
    const auto created = good(app.execute(caller,
                                          at(info),
                                          "geometry.create_line",
                                          line_geometry_signature(line),
                                          create_line_handler(line),
                                          "line"));
    const records::GeometryId geometry(created.primary_entity.value);
    TaskService service(publisher(app));
    info = good(app.current_document());
    const auto task = good(service.start(
        line_mesh_task(good(app.snapshot(info.document)), caller, {}, {geometry, 10, {}}, "mesh")));
    const auto completed = good(service.wait(caller, task.id));
    audit.require(completed.state == TaskState::succeeded,
                  "Original 11-node/10-beam mesh is generated by the real worker");
    const auto mesh = line_mesh_identity(task.id);
    const EntityId old_node("node-" + task.id + "-0");
    info = good(app.current_document());
    good(app.execute(
        caller,
        at(info),
        "seed.references",
        "unmapped-external-references",
        [old_node](const DocumentView& view, const RecordIdentityAllocator&) {
            EditSession edit(view);
            edit.put(records::EntitySet{EntityId("fixed-set"), "Fixed", {old_node}});
            edit.put(records::Constraint{EntityId("fixed"), {old_node}, "123456"});
            edit.put(
                records::IncludeDocument{EntityId("root-include"), "model.bdf", {}, {old_node}});
            edit.put(records::SourceIdentifier{EntityId("source"),
                                               old_node,
                                               "import-provenance",
                                               EntityId("root-include"),
                                               profile,
                                               "GRID",
                                               7});
            return Result<RecordPreparedOperation>{
                Status::success,
                RecordPreparedOperation{edit.prepare(),
                                        "Seed references",
                                        old_node,
                                        "unmapped-external-references",
                                        0,
                                        true},
                {}};
        },
        "references"));
    info = good(app.current_document());
    const std::array<double, 3> end{1200, 0, 0};
    good(app.execute(caller,
                     at(info),
                     "geometry.move_endpoint",
                     line_endpoint_signature(geometry, end),
                     move_line_endpoint_handler(geometry, end),
                     "changed-line"));
    const auto before = capture(app, *store);
    audit.input = "Public regenerate_line_mesh_task: stale real 10-segment mesh →9 segments with "
                  "unmapped set/constraint/INCLUDE/source references; policy=reject_unmapped";
    const auto view = good(app.snapshot(before.info.document)).records;
    audit.require(view.find<records::Mesh>(mesh)->get<records::Mesh>().stale,
                  "Geometry endpoint edit marks the old mesh binding stale");
    // Supplementary coordinator probe rejects a deliberately incomplete generic candidate.
    const auto generic = app.execute(
        caller,
        at(before.info),
        "test.unmapped-candidate",
        "reject_unmapped",
        [old_node, mesh](const DocumentView& base, const RecordIdentityAllocator&) {
            EditSession edit(base);
            edit.erase({RecordTraits<records::Node>::type_id, old_node.value});
            edit.put(records::Node{EntityId("new-node"), {0, 0, 0}, mesh});
            return Result<RecordPreparedOperation>{
                Status::success,
                RecordPreparedOperation{edit.prepare(),
                                        "Unmapped generic candidate",
                                        EntityId("new-node"),
                                        "reject_unmapped",
                                        0,
                                        true},
                {}};
        },
        "generic-unmapped");
    audit.require(!generic.ok(), "Supplementary generic unmapped candidate is rejected atomically");
    unchanged(audit, before, capture(app, *store));
    const auto task_rows_before = store->load_rows();
    const auto replacement =
        good(service.start(regenerate_line_mesh_task(good(app.snapshot(before.info.document)),
                                                     caller,
                                                     mesh,
                                                     9,
                                                     "reject_unmapped",
                                                     "public-unmapped-replace")));
    const auto failed = good(service.wait(caller, replacement.id));
    audit.actual["replacement_task_id"] = replacement.id;
    audit.actual["replacement_final_state"] = task_state_name(failed.state);
    audit.actual["replacement_diagnostic"] =
        failed.diagnostic ? failed.diagnostic->message : "none";
    audit.require(
        failed.state == TaskState::failed && !failed.receipt && failed.diagnostic,
        "Real public regeneration rejects the complete candidate with unmapped references");
    unchanged(audit, before, capture(app, *store), false);
    auto model_fact_rows = [](LoadedRows rows) {
        rows.rows.erase(std::remove_if(rows.rows.begin(),
                                       rows.rows.end(),
                                       [](const StoredRow& row) {
                                           return row.key.space == StoreSpace::task_record ||
                                                  row.key.space == StoreSpace::document_metadata;
                                       }),
                        rows.rows.end());
        return encode_record_rows(rows.rows);
    };
    audit.require(model_fact_rows(task_rows_before) == model_fact_rows(store->load_rows()),
                  "Public rejected task persists zero model/history/operation-fact changes");
    const auto after = good(app.snapshot(before.info.document)).records;
    audit.require(
        after.find<records::Constraint>(EntityId("fixed"))->get<records::Constraint>().nodes ==
                std::vector<EntityId>{old_node} &&
            after.find<records::EntitySet>(EntityId("fixed-set"))
                    ->get<records::EntitySet>()
                    .members == std::vector<EntityId>{old_node} &&
            after.find<records::SourceIdentifier>(EntityId("source"))
                    ->get<records::SourceIdentifier>()
                    .entity == old_node &&
            after.find<records::Node>(old_node) && !after.find<records::Node>(EntityId("new-node")),
        "Rejected public replacement retains original identities and all external references "
        "without silent remapping");
}
} // namespace
int main(int argc, char** argv) {
    executable = fs::absolute(argv[0]).string();
    try {
        if (argc > 1 && std::string_view(argv[1]).starts_with("--"))
            return child(argc, argv);
        const std::string evidence = argc > 1 ? argv[1] : "";
        const std::string source = argc > 2 ? argv[2] : "mutable-diagnostic";
        const auto nonce =
            std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + '-' +
            std::to_string(::getpid());
        std::ofstream file;
        if (!evidence.empty())
            file.open(evidence);
        unsigned failures{};
        for (unsigned id = 1; id <= 21; ++id) {
            if (id == 5 || id == 16 || id == 17)
                continue;
            for (unsigned repeat = 1; repeat <= 10; ++repeat) {
                Audit audit;
                std::string error;
                try {
                    if (id == 4)
                        epoch_fault(audit);
                    else if (id == 9 || id == 10)
                        crash_fault(id, audit);
                    else if (id == 11)
                        changed_save_target(audit);
                    else if (id == 12)
                        second_writer(audit);
                    else if (id >= 13 && id <= 15)
                        task_fault(id, audit);
                    else if (id == 18)
                        damaged_project(audit);
                    else if (id == 19)
                        incompatible_profile(audit);
                    else if (id == 20)
                        quota_fault(audit);
                    else if (id == 21)
                        replacement_fault(audit);
                    else
                        ordinary_fault(id, audit);
                } catch (const std::exception& problem) {
                    error = problem.what();
                    ++failures;
                }
                const auto case_id = std::string("F") + (id < 10 ? "0" : "") + std::to_string(id);
                const auto run = "sqlite-" + nonce + '-' + case_id + '-' + std::to_string(repeat);
                const auto line = audit.json(case_id, run, source, error);
                if (file) {
                    file << line << '\n';
                    file.flush();
                }
                std::cout << line << '\n';
            }
        }
        return failures ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
}
