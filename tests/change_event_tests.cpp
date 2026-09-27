#include "runtime_test_support.hpp"
#include <cstdlib>
#include <iostream>
#include <new>

namespace {
long fail_after = -1;
}
void* operator new(std::size_t size) {
    if (fail_after == 0) {
        fail_after = -1;
        throw std::bad_alloc();
    }
    if (fail_after > 0)
        --fail_after;
    if (void* result = std::malloc(size ? size : 1))
        return result;
    throw std::bad_alloc();
}
void operator delete(void* value) noexcept {
    std::free(value);
}
void operator delete(void* value, std::size_t) noexcept {
    std::free(value);
}
void* operator new[](std::size_t size) {
    return ::operator new(size);
}
void operator delete[](void* value) noexcept {
    ::operator delete(value);
}
void operator delete[](void* value, std::size_t) noexcept {
    ::operator delete(value);
}

using namespace runtime_test;
namespace {
void commit(RecordApplication& app, double value, const std::string& key) {
    const auto info = good(app.current_document());
    good(app.execute(
        caller, at(info), "test.material", modulus_signature(value), material_change(value), key));
}
void committed_changes_and_history() {
    auto store = std::make_shared<Store>();
    RecordApplication app(options(store));
    seed(app);
    const auto initial = good(app.current_document());
    const auto before = good(app.snapshot(initial.document));
    commit(app, 180000, "edit");
    const auto current = good(app.current_document());
    const auto after = good(app.snapshot(initial.document));
    auto batch = good(app.changes_since(initial.document, initial.revision));
    check(!batch.resync_required && batch.changes.size() == 1, "one actual commit in journal");
    const auto original = batch.changes.front().changes;
    const auto& change = batch.changes.front();
    check(change.base_revision == initial.revision && change.revision == current.revision &&
              change.direction == RecordDirection::forward && original->records.size() == 1,
          "journal supplies directed versioned change");
    check(original->records.front().before ==
                  before.records.find<records::Material>(EntityId("material")) &&
              original->records.front().after ==
                  after.records.find<records::Material>(EntityId("material")),
          "journal borrows exact immutable model record images");
    good(app.undo(caller, at(current), "undo"));
    const auto undone = good(app.current_document());
    good(app.redo(caller, at(undone), "redo"));
    batch = good(app.changes_since(initial.document, initial.revision));
    check(batch.changes.size() == 3 && batch.changes[1].changes == original &&
              batch.changes[2].changes == original &&
              batch.changes[1].direction == RecordDirection::reverse &&
              batch.changes[2].direction == RecordDirection::forward,
          "undo and redo retain original immutable changes with correct direction");
    const auto count = app.change_journal_stats().entries_published;
    const auto replay = good(app.execute(caller,
                                         at(initial),
                                         "test.material",
                                         modulus_signature(180000),
                                         material_change(180000),
                                         "edit"));
    check(replay.replayed && app.change_journal_stats().entries_published == count,
          "replayed writes publish no duplicate journal entry");
    check(!app.execute(caller,
                       at(initial),
                       "test.material",
                       modulus_signature(170000),
                       material_change(170000),
                       "stale")
               .ok(),
          "stale write rejected");
    check(app.change_journal_stats().entries_published == count, "failed writes publish no entry");
    check(good(app.changes_since(initial.document, good(app.current_document()).revision))
              .changes.empty(),
          "current cursor has no changes");
    const auto model_bytes = app.stats().model_bytes_copied;
    good(app.changes_since(initial.document, initial.revision));
    check(app.change_journal_stats().metadata_bytes_copied > 0 &&
              app.stats().model_bytes_copied == model_bytes,
          "journal metadata measured separately without model copies");
}
void bounded_retention_and_recovery() {
    auto store = std::make_shared<Store>();
    auto config = options(store);
    config.max_change_journal_entries = 2;
    RecordApplication app(config);
    seed(app);
    const auto initial = good(app.current_document());
    for (unsigned i = 0; i < 4; ++i)
        commit(app, 150000 + i, "edit-" + std::to_string(i));
    const auto current = good(app.current_document());
    const auto expired = good(app.changes_since(initial.document, initial.revision));
    check(expired.resync_required && expired.changes.empty() &&
              expired.current_revision == current.revision,
          "expired cursor requires snapshot");
    check(good(app.changes_since(initial.document, current.revision - 2)).changes.size() == 2,
          "oldest retained base is valid");
    const auto future = app.changes_since(initial.document, current.revision + 1);
    check(!future.ok() && future.error->code == ErrorCode::revision_conflict,
          "future change cursor rejected");
    good(app.close_document(caller, at(current), ClosePolicy::keep_recovery, "close"));
    const auto recovered = good(app.recover_document(caller, "recover"));
    check(recovered.document.epoch != initial.document.epoch &&
              !app.changes_since(initial.document, current.revision).ok(),
          "recovery expires original epoch");
    check(good(app.changes_since(recovered.document, 0)).resync_required,
          "transient journal is not fabricated from persisted history");
    commit(app, 120000, "post-recovery");
    check(good(app.changes_since(recovered.document, recovered.revision)).changes.size() == 1,
          "new epoch journals new commits");
    auto disabled_options = options({});
    disabled_options.max_change_journal_entries = 0;
    RecordApplication disabled(disabled_options);
    seed(disabled);
    const auto disabled_info = good(disabled.current_document());
    check(good(disabled.changes_since(disabled_info.document, 0)).resync_required,
          "disabled retention explicitly requests resync");
}
void task_commits_and_uncertain_storage() {
    auto store = std::make_shared<Store>();
    RecordApplication app(options(store));
    seed(app);
    const auto initial = good(app.current_document());
    {
        TaskService service(publisher(app));
        auto request = material_task(app, "journal-task", 125000);
        const auto retry = request;
        const auto task = good(service.start(std::move(request)));
        const auto completed = good(service.wait(caller, task.id));
        const auto batch = good(app.changes_since(initial.document, initial.revision));
        check(completed.state == TaskState::succeeded && batch.changes.size() == 1 &&
                  batch.changes.front().transaction == completed.receipt->transaction,
              "actual worker atomic publication is journaled once");
        check(good(service.start(retry)).id == task.id &&
                  good(app.changes_since(initial.document, initial.revision)).changes.size() == 1,
              "task replay creates no model event");
    }
    const auto before = good(app.current_document());
    const auto count = app.change_journal_stats().entries_published;
    store->fault = Store::Fault::before_model;
    check(!app.execute(caller,
                       at(before),
                       "test.material",
                       modulus_signature(110000),
                       material_change(110000),
                       "fault-before")
               .ok(),
          "definite rollback");
    check(app.change_journal_stats().entries_published == count &&
              good(app.changes_since(before.document, before.revision)).changes.empty(),
          "rolled back durable write cannot publish journal");
    store->fault = Store::Fault::after_model;
    check(!app.execute(caller,
                       at(before),
                       "test.material",
                       modulus_signature(100000),
                       material_change(100000),
                       "fault-after")
               .ok(),
          "unknown commit reported");
    const auto unavailable = app.changes_since(before.document, before.revision);
    check(!unavailable.ok() && unavailable.error->code == ErrorCode::storage_uncertain,
          "unknown durable outcome requires recovery instead of an accepted event cursor");
}
struct TrapStore final : IRecordStore {
    Store implementation;
    bool arm{};
    LoadedRows load_rows() override {
        return implementation.load_rows();
    }
    BatchReceipt commit_rows(const StoreBatch& batch) override {
        const auto receipt = implementation.commit_rows(batch);
        if (arm)
            fail_after = 0;
        return receipt;
    }
};
void no_allocation_after_persist_and_preparation_failure() {
    for (unsigned operation = 0; operation < 3; ++operation) {
        auto store = std::make_shared<TrapStore>();
        RecordApplication app(options(store));
        seed(app);
        if (operation == 2)
            good(app.undo(caller, at(good(app.current_document())), "prepare-redo"));
        const auto info = good(app.current_document());
        std::optional<ChangePreview> preview;
        if (!operation)
            preview = good(app.preview(caller, at(info), material_change(130000)));
        store->arm = true;
        Result<ChangeReceipt> result;
        try {
            if (!operation)
                result = app.commit(caller, at(info), preview->id, "persist-commit");
            else if (operation == 1)
                result = app.undo(caller, at(info), "persist-undo");
            else
                result = app.redo(caller, at(info), "persist-redo");
        } catch (...) {
            fail_after = -1;
            throw;
        }
        const bool no_post_commit_allocation = fail_after == 0;
        fail_after = -1;
        check(result.ok() && no_post_commit_allocation,
              "accepted commit/history publication performs no allocation after durable persist");
        check(good(app.changes_since(info.document, info.revision)).changes.size() == 1,
              "successful no-throw publication includes its journal entry");
    }
    bool reached_success = false;
    for (long allocation = 0; allocation < 768 && !reached_success; ++allocation) {
        auto store = std::make_shared<Store>();
        RecordApplication app(options(store));
        seed(app);
        const auto info = good(app.current_document());
        const auto preview = good(app.preview(caller, at(info), material_change(140000)));
        const auto generation = store->load_rows().generation;
        const auto entries = app.change_journal_stats().entries_published;
        Result<ChangeReceipt> result;
        bool threw = false;
        fail_after = allocation;
        try {
            result = app.commit(caller, at(info), preview.id, "allocation-commit");
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        fail_after = -1;
        if (threw) {
            check(store->load_rows().generation == generation &&
                      good(app.current_document()).revision == info.revision &&
                      app.change_journal_stats().entries_published == entries,
                  "allocation failure rolls back model, durable store and prepared journal");
        } else {
            check(result.ok(), "allocation sweep reached accepted commit");
            reached_success = true;
        }
    }
    check(reached_success, "allocation sweep exhausted all commit preparation allocations");
}
} // namespace
int main() {
    try {
        committed_changes_and_history();
        bounded_retention_and_recovery();
        task_commits_and_uncertain_storage();
        no_allocation_after_persist_and_preparation_failure();
        std::cout << "change journal tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        fail_after = -1;
        std::cerr << error.what() << '\n';
        return 1;
    }
}
