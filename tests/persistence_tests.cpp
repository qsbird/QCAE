#include "qcae/core.hpp"
#include "qcae/state_codec.hpp"
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
using namespace qcae;
namespace {
void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
template <class T> T good(Result<T> result) {
    if (!result.ok())
        throw std::runtime_error(result.error ? result.error->message : "missing result");
    return *result.value;
}
struct FakeStore : IWorkspaceStore {
    std::optional<StoredWorkspace> workspace;
    std::map<std::string, StoredProject> projects;
    std::map<std::string, std::string> aliases;
    enum class Fault { none, commit_before, commit_after, publish_before, publish_after } fault{};
    std::optional<StoredWorkspace> load() override {
        return workspace;
    }
    std::uint64_t commit(std::uint64_t expected, const std::string& payload) override {
        if (fault == Fault::commit_before) {
            fault = Fault::none;
            throw StorageError("before commit");
        }
        if (workspace && workspace->generation != expected)
            throw StorageError("generation conflict");
        workspace = StoredWorkspace{expected + 1, payload};
        if (fault == Fault::commit_after) {
            fault = Fault::none;
            throw StorageError("after commit", true);
        }
        return expected + 1;
    }
    std::string acquire_project(const std::string& path) override {
        const auto found = aliases.find(path);
        return found == aliases.end() ? path : found->second;
    }
    void release_projects_except(const std::string&) noexcept override {}
    StoredProject read_project(const std::string& path) override {
        const auto found = projects.find(path);
        if (found == projects.end())
            throw StorageError("project_not_found");
        return found->second;
    }
    void publish_project(const std::string& path, const StoredProject& project) override {
        if (fault == Fault::publish_before) {
            fault = Fault::none;
            throw StorageError("before publish");
        }
        projects[path] = project;
        if (fault == Fault::publish_after) {
            fault = Fault::none;
            throw StorageError("after publish", true);
        }
    }
};
WriteContext at(const DocumentInfo& info) {
    return {info.document, info.revision};
}
void main_flow() {
    auto store = std::make_shared<FakeStore>();
    Caller user{"tester"};
    DocumentInfo original, saved;
    {
        MemoryApplication app({}, store);
        check(app.durable() && !app.recovery_available(), "durable discovery");
        original = good(app.create_document(user, "Beam", "create"));
        check(original.durable && !original.dirty, "create facts");
        const auto preview =
            good(app.preview(user, at(original), CreateMaterial{"Steel", {210, "GPa"}}));
        store->fault = FakeStore::Fault::commit_before;
        auto failed = app.commit(user, at(original), preview.id, "steel");
        check(!failed.ok() && failed.error->code == ErrorCode::storage_failure,
              "definite commit failure");
        check(good(app.current_document()).revision == 0, "RAM unchanged on failure");
        good(app.commit(user, at(original), preview.id, "steel"));
        check(good(app.current_document()).revision == 1, "committed model");
        saved = good(app.current_document());
    }
    {
        MemoryApplication app({}, store);
        check(app.recovery_available() && !app.current_document().ok(), "explicit recovery");
        auto recovered = good(app.recover_document(user, "recover"));
        check(recovered.document.id == original.document.id &&
                  recovered.document.epoch != original.document.epoch && recovered.revision == 1,
              "recovery identity");
        check(good(app.snapshot(recovered.document)).materials.size() == 1, "recovered model");
        check(!app.preview(user, at(saved), CreateMaterial{"X", {1, "MPa"}}).ok(),
              "old epoch rejected");
        check(good(app.operation(user, recovered.document, "commit", "steel")).replayed,
              "commit fact retained");
        good(app.undo(user, at(recovered), "undo"));
        auto undone = good(app.current_document());
        check(good(app.snapshot(undone.document)).materials.empty(), "undo after restart");
        good(app.redo(user, at(undone), "redo"));
        auto redone = good(app.current_document());
        check(good(app.snapshot(redone.document)).materials.size() == 1, "redo after restart");
        auto other_store = std::make_shared<FakeStore>();
        {
            MemoryApplication other({}, other_store);
            auto other_info = good(other.create_document(user, "Other", "other-create"));
            good(other.save_document(user, at(other_info), "/foreign.qcae", false, "other-save"));
        }
        store->projects["/foreign.qcae"] = other_store->projects.at("/foreign.qcae");
        auto foreign = app.save_document(user, at(redone), "/foreign.qcae", false, "foreign");
        check(!foreign.ok() && foreign.error->code == ErrorCode::invalid_input,
              "unrelated target rejected before intent");
        good(app.undo(user, at(redone), "undo-foreign"));
        auto after_foreign_undo = good(app.current_document());
        good(app.redo(user, at(after_foreign_undo), "redo-foreign"));
        redone = good(app.current_document());
        store->fault = FakeStore::Fault::publish_before;
        auto incomplete = app.save_document(user, at(redone), "/beam.qcae", false, "save");
        check(!incomplete.ok() && incomplete.error->code == ErrorCode::storage_failure,
              "publish failure");
        check(!app.current_document().value->saved_path.size(), "save marker unchanged");
        auto blocked = app.undo(user, at(redone), "undo2");
        check(!blocked.ok() && blocked.error->code == ErrorCode::storage_uncertain,
              "pending intent blocks write");
        auto finished = good(app.save_document(user, at(redone), "/beam.qcae", false, "save"));
        check(!finished.dirty && finished.saved_path == "/beam.qcae", "save completion");
        check(good(app.host_operation(user, "save_document", "save")).project_id ==
                  finished.project_id,
              "host save lookup");
        auto original_project = finished.project_id;
        auto as = good(app.save_document(user, at(finished), "/copy.qcae", true, "save-as"));
        check(as.project_id != original_project && as.document.id == finished.document.id &&
                  as.document.epoch == finished.document.epoch && as.revision == finished.revision,
              "save-as identity and revision");
        good(app.close_document(user, at(as), ClosePolicy::discard, "discard"));
        check(!app.current_document().ok() && !app.recovery_available(),
              "discard removes recovery");
        check(good(app.create_document(user, "Beam", "create")).document.id == original.document.id,
              "closed create key replays original fact");
        check(good(app.host_operation(user, "close_document", "discard")).document.id ==
                  as.document.id,
              "closed host fact retained");
        auto opened = good(app.open_document(user, "/copy.qcae", "open"));
        check(opened.document.id != as.document.id && opened.document.epoch != as.document.epoch &&
                  opened.project_id == as.project_id && opened.revision == 0,
              "normal open identity");
    }
}
void crash_windows() {
    auto store = std::make_shared<FakeStore>();
    Caller user{"tester"};
    DocumentInfo created;
    {
        MemoryApplication app({}, store);
        created = good(app.create_document(user, "A", "create"));
        auto preview = good(app.preview(user, at(created), CreateMaterial{"A", {1, "MPa"}}));
        store->fault = FakeStore::Fault::commit_after;
        auto result = app.commit(user, at(created), preview.id, "commit");
        check(!result.ok() && result.error->code == ErrorCode::storage_uncertain,
              "uncertain commit");
        check(app.recovery_available(), "poisoned state discovery");
    }
    {
        MemoryApplication app({}, store);
        auto recovered = good(app.recover_document(user, "recover"));
        check(recovered.revision == 1 &&
                  good(app.snapshot(recovered.document)).materials.size() == 1,
              "durable commit survived RAM publication gap");
        check(good(app.operation(user, recovered.document, "commit", "commit")).replayed,
              "commit outcome survived response gap");
        store->fault = FakeStore::Fault::publish_after;
        auto failed = app.save_document(user, at(recovered), "/a.qcae", false, "save");
        check(!failed.ok() && failed.error->code == ErrorCode::storage_uncertain,
              "uncertain publish");
    }
    {
        MemoryApplication app({}, store);
        auto recovered = good(app.recover_document(user, "recover2"));
        check(recovered.saved_path == "/a.qcae" && !recovered.dirty,
              "published snapshot reconciled after restart");
        check(good(app.host_operation(user, "save_document", "save")).saved_path == "/a.qcae",
              "reconciled save fact");
    }
}
void uncertain_same_process_recovery() {
    auto store = std::make_shared<FakeStore>();
    MemoryApplication app({}, store);
    Caller user{"tester"};
    auto created = good(app.create_document(user, "A", "create"));
    auto preview = good(app.preview(user, at(created), CreateMaterial{"Steel", {1, "MPa"}}));
    store->fault = FakeStore::Fault::commit_after;
    auto uncertain = app.commit(user, at(created), preview.id, "commit");
    check(!uncertain.ok() && uncertain.error->code == ErrorCode::storage_uncertain,
          "uncertain state reported");
    auto recovered = good(app.recover_document(user, "recover"));
    check(recovered.revision == 1 && recovered.document.epoch != created.document.epoch,
          "same-process recovery reloads durable fact");
}
void unavailable_profile_rejected() {
    auto store = std::make_shared<FakeStore>();
    Model model;
    model.analyses.push_back(
        {EntityId("analysis"), "case", {{"unknown", "1", "digest"}, "linear_static"}, {}, {}});
    state_codec::Writer writer;
    writer.text("QCAE-PROJECT");
    writer.number(1);
    writer.text("project");
    writer.text("case");
    writer.text("state");
    state_codec::write_model(writer, model);
    store->projects["/profile.qcae"] = StoredProject{"token", writer.take()};
    MemoryApplication app(
        {}, store, [](const ProfileRef& profile) { return profile.profile_id == "nastran"; });
    auto rejected = app.open_document(Caller{"tester"}, "/profile.qcae", "open");
    check(!rejected.ok() && rejected.error->code == ErrorCode::schema_unsupported,
          "unavailable profile rejected");
    check(!app.current_document().ok() && !store->workspace,
          "rejected project does not activate or persist");
}
void pending_target_changes() {
    auto store = std::make_shared<FakeStore>();
    Caller user{"tester"};
    store->aliases["/alias"] = "/original";
    {
        MemoryApplication app({}, store);
        auto created = good(app.create_document(user, "A", "create"));
        store->fault = FakeStore::Fault::publish_before;
        auto failed = app.save_document(user, at(created), "/alias", false, "save");
        check(!failed.ok(), "save intent staged");
        store->aliases["/alias"] = "/other";
        auto retargeted = app.save_document(user, at(created), "/alias", false, "save");
        check(!retargeted.ok() && retargeted.error->code == ErrorCode::idempotency_key_conflict &&
                  !store->projects.contains("/other"),
              "retargeted alias cannot redirect pending save");
        store->aliases["/alias"] = "/original";
        auto saved = good(app.save_document(user, at(created), "/alias", false, "save"));
        check(saved.saved_path == "/original", "frozen save target reused");
    }
    auto other = std::make_shared<FakeStore>();
    {
        MemoryApplication app({}, other);
        auto created = good(app.create_document(user, "B", "create"));
        other->fault = FakeStore::Fault::publish_before;
        auto failed = app.save_document(user, at(created), "/target", false, "save");
        check(!failed.ok(), "unpublished save intent");
    }
    other->projects["/target"] = store->projects.at("/original");
    {
        MemoryApplication app({}, other);
        auto recovered = good(app.recover_document(user, "recover"));
        check(recovered.saved_path.empty(), "different target does not mark work saved");
        auto new_save =
            good(app.save_document(user, at(recovered), "/elsewhere", false, "new-save"));
        check(new_save.saved_path == "/elsewhere", "mismatched intent does not strand work");
    }
}
void corrupted_workspace_rejected() {
    auto store = std::make_shared<FakeStore>();
    state_codec::Writer writer;
    writer.text("QCAE-WORKSPACE");
    writer.number(2);
    writer.text("nonce");
    writer.number(1);
    writer.boolean(false);
    Model orphan;
    orphan.nodes.push_back({EntityId("node"), {0, 0, 0}});
    state_codec::write_model(writer, orphan);
    writer.text("");
    writer.number(0);
    writer.number(0);
    writer.number(0);
    writer.number(0);
    writer.boolean(false);
    writer.boolean(false);
    store->workspace = StoredWorkspace{1, writer.take()};
    bool rejected = false;
    try {
        MemoryApplication app({}, store);
    } catch (const state_codec::CodecError&) {
        rejected = true;
    }
    check(rejected, "orphan node rejected on load");
    store = std::make_shared<FakeStore>();
    {
        MemoryApplication app({}, store);
        Caller user{"tester"};
        auto created = good(app.create_document(user, "A", "create"));
        auto preview = good(app.preview(user, at(created), CreateMaterial{"Steel", {1, "MPa"}}));
        good(app.commit(user, at(created), preview.id, "commit"));
        auto id = good(app.snapshot(created.document)).materials[0].id.value;
        auto& payload = store->workspace->payload;
        const auto model_position = payload.find(id);
        const auto position = payload.find(id, model_position + id.size());
        check(position != std::string::npos, "delta fixture located");
        payload[position] = payload[position] == 'x' ? 'y' : 'x';
    }
    rejected = false;
    try {
        MemoryApplication app({}, store);
    } catch (const state_codec::CodecError&) {
        rejected = true;
    }
    check(rejected, "corrupt delta rejected on load");
}
} // namespace
int main() {
    main_flow();
    crash_windows();
    uncertain_same_process_recovery();
    unavailable_profile_rejected();
    pending_target_changes();
    corrupted_workspace_rejected();
}
