#include "runtime_test_support.hpp"
#include <iostream>

using namespace runtime_test;
namespace {
void queue_and_cancel() {
    auto store = std::make_shared<Store>();
    RecordApplication app(options(store));
    seed(app);
    TaskService service(publisher(app));
    const auto initial = good(app.current_document());
    auto gate = std::make_shared<Gate>();
    std::vector<TaskRecord> tasks;
    tasks.push_back(good(service.start(material_task(app, "running-1", 190000, gate))));
    tasks.push_back(good(service.start(material_task(app, "running-2", 180000, gate))));
    gate->wait_for(2);
    check(service.active_workers() == 2, "two real workers reach the barrier");
    for (unsigned index = 0; index < 8; ++index)
        tasks.push_back(
            good(service.start(material_task(app, "queued-" + std::to_string(index), 170000))));
    check(service.queued_tasks() == 8, "eight pending tasks admitted");
    const auto generation_before_reconcile = store->load_rows().generation;
    const auto active_reconcile = service.reconcile();
    check(!active_reconcile.ok() && active_reconcile.error->code == ErrorCode::invalid_input &&
              store->load_rows().generation == generation_before_reconcile,
          "reconciliation cannot rewrite facts while workers or queued tasks are active");
    auto excess = service.start(material_task(app, "ninth-pending", 160000));
    check(!excess.ok() && excess.error->code == ErrorCode::resource_limit,
          "ninth pending task rejected");
    check(!app.close_document(caller, at(initial), ClosePolicy::keep_recovery, "close-active").ok(),
          "application refuses close while persisted task facts are active");
    check(good(app.current_document()).revision == initial.revision,
          "queued/running metadata does not advance model revision");
    for (const auto& task : tasks)
        check(good(service.cancel(caller, task.id)).accepted, "pre-commit cancellation accepted");
    gate->release();
    for (const auto& task : tasks)
        check(good(service.wait(caller, task.id)).state == TaskState::cancelled,
              "cancelled work never publishes");
    check(service.active_workers() == 0 && service.queued_tasks() == 0,
          "all task resources released");
    check(good(app.current_document()).revision == initial.revision,
          "cancelled tasks produced zero model transactions");
}
void incomplete_candidates_do_not_commit() {
    auto store = std::make_shared<Store>();
    RecordApplication app(options(store));
    seed(app);
    TaskService service(publisher(app));
    const auto initial = good(app.current_document());
    for (unsigned sample = 0; sample < 2; ++sample) {
        auto request = material_task(app, "incomplete-" + std::to_string(sample), 180000);
        const auto base = good(app.snapshot(initial.document)).records;
        request.work = [base, sample](const TaskControl&) -> std::shared_ptr<const TaskPayload> {
            if (!sample)
                return {};
            EditSession edit(base);
            return std::make_shared<const RecordTaskPayload>(
                RecordPreparedOperation{edit.prepare(),
                                        "Empty candidate",
                                        EntityId{},
                                        modulus_signature(180000),
                                        0,
                                        false});
        };
        const auto started = good(service.start(std::move(request)));
        check(good(service.wait(caller, started.id)).state == TaskState::failed,
              "null or empty worker candidates cannot claim success");
        check(good(app.current_document()).revision == initial.revision,
              "incomplete worker candidates cannot create model transactions");
    }
}
void competing_worker_publications() {
    auto store = std::make_shared<Store>();
    RecordApplication app(options(store));
    seed(app);
    TaskService service(publisher(app));
    const auto initial = good(app.current_document());
    auto gate = std::make_shared<Gate>();
    const auto first = good(service.start(material_task(app, "competing-first", 180000, gate)));
    const auto second = good(service.start(material_task(app, "competing-second", 190000, gate)));
    gate->wait_for(2);
    gate->release();
    const auto left = good(service.wait(caller, first.id));
    const auto right = good(service.wait(caller, second.id));
    check((left.state == TaskState::succeeded && right.state == TaskState::conflicted) ||
              (right.state == TaskState::succeeded && left.state == TaskState::conflicted),
          "two candidates at one revision yield exactly one committed winner");
    const auto info = good(app.current_document());
    check(info.revision == initial.revision + 1, "parallel workers share one application writer");
    const auto material =
        good(app.snapshot(info.document)).records.find<records::Material>(EntityId("material"));
    check(material->get<records::Material>().young_modulus_mpa ==
              (left.state == TaskState::succeeded ? 180000 : 190000),
          "losing candidate never overwrites winner");
}
void state_write_failure_reconciliation() {
    auto store = std::make_shared<Store>();
    RecordApplication app(options(store));
    seed(app);
    TaskService service(publisher(app));
    const auto initial = good(app.current_document());
    store->task_write_failure.store(2); // Admission is durable; queued -> running rolls back.
    const auto task = good(service.start(material_task(app, "state-write-failure", 120000)));
    check(good(service.wait(caller, task.id)).state == TaskState::interrupted,
          "definite task-state rollback stops the task");
    check(!app.recovery_available(), "definite side-row rollback does not poison model authority");
    store->task_write_failure.store(1);
    check(!service.reconcile().ok(), "failed reconciliation remains retryable");
    check(!service.start(material_task(app, "blocked-start", 110000)).ok(),
          "failed repair keeps admission blocked");
    good(service.reconcile());
    check(good(service.query(caller, task.id)).state == TaskState::interrupted,
          "explicit reconciliation durably interrupts unfinished work without rerun");
    check(good(app.current_document()).revision == initial.revision,
          "repair changes no model revision");
    const auto next = good(service.start(material_task(app, "after-repair", 100000)));
    check(good(service.wait(caller, next.id)).state == TaskState::succeeded,
          "service resumes after a definite task-state write failure");
}
void publication_faults_and_replay() {
    for (auto fault : {Store::Fault::none, Store::Fault::before_model, Store::Fault::after_model}) {
        auto store = std::make_shared<Store>();
        RecordApplication app(options(store));
        seed(app);
        TaskService service(publisher(app));
        const auto initial = good(app.current_document());
        auto request = material_task(app, "publication", 123000);
        const auto retry = request;
        store->fault.store(fault);
        const auto started = good(service.start(std::move(request)));
        auto completed = good(service.wait(caller, started.id));
        if (fault == Store::Fault::none) {
            check(completed.state == TaskState::succeeded, "successful publication fact");
            const auto generation = store->load_rows().generation;
            check(good(service.start(retry)).id == started.id &&
                      store->load_rows().generation == generation,
                  "task start retry after revision advanced returns original fact without rerun");
        } else if (fault == Store::Fault::before_model) {
            check(completed.state == TaskState::failed, "definite publication failure recorded");
            check(good(app.current_document()).revision == initial.revision,
                  "definite failure preserved model");
        } else {
            check(completed.state == TaskState::outcome_unknown && app.recovery_available(),
                  "uncertain commit stops the service");
            const auto persisted = decode_record_state_image(store->load_rows().rows,
                                                             make_record_registry(),
                                                             {},
                                                             std::array{task_row_handler()});
            check(persisted.document->revision == initial.revision + 1,
                  "durable model committed exactly once");
            bool durable_success = false;
            for (const auto& [key, row] : *persisted.owned_rows)
                if (key.identity == started.id)
                    durable_success =
                        decode_task_record(*row->payload).state == TaskState::succeeded;
            check(durable_success, "atomic durable task success was not overwritten with failure");
            const auto before_reconcile = store->load_rows().generation;
            const auto premature = service.reconcile();
            check(!premature.ok() && premature.error->code == ErrorCode::storage_uncertain &&
                      store->load_rows().generation == before_reconcile &&
                      good(service.query(caller, started.id)).state == TaskState::outcome_unknown,
                  "task reconciliation cannot bypass explicit application recovery for unknown "
                  "commits");
            const auto recovered = good(app.recover_document(caller, "recover-publication"));
            good(service.reconcile());
            completed = good(service.query(caller, started.id));
            check(completed.state == TaskState::succeeded && completed.receipt,
                  "reconciliation returns committed task fact");
            check(recovered.revision == initial.revision + 1,
                  "recovery did not duplicate model commit");
            const auto generation = store->load_rows().generation;
            check(!good(service.cancel(caller, started.id)).accepted,
                  "cancellation after commit returns existing fact");
            check(store->load_rows().generation == generation,
                  "late cancellation does not write or roll back");
            const auto expired = service.start(retry);
            check(!expired.ok() && expired.error->code == ErrorCode::document_epoch_expired,
                  "old-epoch task start is rejected after recovery");
            auto different = retry;
            different.input.document = recovered.document;
            different.signature += "changed";
            auto conflict = service.start(std::move(different));
            check(!conflict.ok() && conflict.error->code == ErrorCode::idempotency_key_conflict,
                  "same task key with different inputs conflicts");
        }
    }
}
void restart_and_owner_validation() {
    auto store = std::make_shared<Store>();
    DocumentInfo info;
    TaskRecord interrupted;
    {
        RecordApplication app(options(store));
        seed(app);
        info = good(app.current_document());
        interrupted.id = "task-restored-running";
        interrupted.caller = caller;
        interrupted.idempotency_key = "restart";
        interrupted.operation = "test.material.task";
        interrupted.signature = modulus_signature(100000);
        interrupted.input = {info.document, info.revision, profile};
        interrupted.state = TaskState::running;
        interrupted.events = {{1, TaskState::queued, 0}, {2, TaskState::running, 0}};
        good(publisher(app).persist(interrupted, std::nullopt));
    }
    {
        auto unknown = options(store);
        unknown.owned_row_handlers.clear();
        bool rejected = false;
        try {
            RecordApplication unsupported(std::move(unknown));
        } catch (const RecordError& error) {
            rejected = error.code() == ErrorCode::schema_unsupported;
        }
        check(rejected, "unknown owner must reject workspace activation");
    }
    {
        RecordApplication app(options(store));
        auto recovered = good(app.recover_document(caller, "restart-recover"));
        check(recovered.revision == info.revision,
              "interrupting restored work does not alter the model");
        TaskService service(publisher(app));
        check(good(service.query(caller, interrupted.id)).state == TaskState::interrupted,
              "restored running task marked interrupted without rerun");
        const auto rows =
            good(app.owned_rows(recovered.document, StoreSpace::task_record, "qcae.runtime.task"));
        auto future = std::make_shared<OwnedRowImage>(*rows.front());
        future->schema_version = 99;
        const OwnedRowUpdate update{future->key, rows.front()->payload, future};
        const auto generation = store->load_rows().generation;
        auto rejected = app.update_owned_rows(caller, recovered.document, std::span(&update, 1));
        check(!rejected.ok() && rejected.error->code == ErrorCode::schema_unsupported &&
                  store->load_rows().generation == generation,
              "unknown owned schema rejected without persistent mutation");
    }
}
void direct_failure_releases_private_preview() {
    auto store = std::make_shared<Store>();
    auto settings = options(store);
    settings.limits.max_previews = 1;
    RecordApplication app(std::move(settings));
    seed(app);
    const auto context = at(good(app.current_document()));
    store->fault.store(Store::Fault::before_model);
    const auto failed = app.execute(
        caller, context, "test.retry", modulus_signature(120000), material_change(120000), "retry");
    check(!failed.ok() && failed.error->code == ErrorCode::storage_failure,
          "direct commit definitely rolls back");
    const auto retried = good(app.execute(caller,
                                          context,
                                          "test.retry",
                                          modulus_signature(120000),
                                          material_change(120000),
                                          "retry"));
    check(retried.committed_revision == context.expected_revision + 1,
          "failed direct command releases its private preview for a real retry");
}
void execute_and_history_integrity() {
    auto store = std::make_shared<Store>();
    RecordApplication app(options(store));
    seed(app);
    const auto context = at(good(app.current_document()));
    unsigned calls = 0;
    auto handler = [&](const DocumentView& view, const RecordIdentityAllocator& allocate) {
        ++calls;
        return material_change(90000)(view, allocate);
    };
    auto first = good(
        app.execute(caller, context, "test.direct", modulus_signature(90000), handler, "direct"));
    const auto generation = store->load_rows().generation;
    auto retry = good(
        app.execute(caller, context, "test.direct", modulus_signature(90000), handler, "direct"));
    check(calls == 1 && retry.replayed && retry.transaction == first.transaction &&
              generation == store->load_rows().generation,
          "direct execute replays before revision and handler checks");
    good(app.undo(caller, at(good(app.current_document())), "undo-direct"));
    check(good(app.execute(
                   caller, context, "test.direct", modulus_signature(90000), handler, "direct"))
                  .replayed &&
              calls == 1,
          "direct retry after undo does not reapply the command");
    auto conflict = app.execute(caller, context, "test.direct", "other", handler, "direct");
    check(!conflict.ok() && conflict.error->code == ErrorCode::idempotency_key_conflict,
          "direct key/input conflict precedes revision check");

    auto registry = make_record_registry();
    RecordStateImage image(registry);
    image.application_nonce = "nonce";
    image.document = good(app.current_document());
    image.document->content_state = "after";
    image.initial_content_state = "before";
    image.records = DocumentView(registry);
    auto history = std::make_shared<RecordHistoryImage>();
    history->transaction = TransactionId("duplicate");
    history->content_state = "after";
    image.history = {history, history};
    image.cursor = 2;
    std::map<StoreKey, SharedStoreBytes> unique;
    for (const auto& row : encode_record_state_image(image))
        unique[row.key] = row.value;
    std::vector<StoredRow> rows;
    for (const auto& [key, value] : unique)
        rows.push_back({key, value});
    bool rejected = false;
    try {
        decode_record_state_image(rows, registry);
    } catch (const RecordError& error) {
        rejected = error.code() == ErrorCode::schema_unsupported;
    }
    check(rejected, "duplicate transaction references in metadata are rejected");
}
} // namespace
int main() {
    try {
        queue_and_cancel();
        incomplete_candidates_do_not_commit();
        competing_worker_publications();
        state_write_failure_reconciliation();
        publication_faults_and_replay();
        restart_and_owner_validation();
        direct_failure_releases_private_preview();
        execute_and_history_integrity();
        std::cout << "PASS: real workers, queue bounds, cancellation, atomic publication, restart "
                     "and direct-operation replay\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
