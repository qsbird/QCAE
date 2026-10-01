#include "qcae/task_service.hpp"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cmath>
#include <deque>
#include <iomanip>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace qcae {
namespace {
template <class T> Result<T> success(T value) {
    return {Status::success, std::move(value), {}};
}
template <class T> Result<T> failure(ErrorCode code, const std::string& message) {
    const auto status = code == ErrorCode::revision_conflict ||
                                code == ErrorCode::idempotency_key_conflict ||
                                code == ErrorCode::document_epoch_expired
                            ? Status::conflict
                            : Status::failed;
    return {status, {}, Diagnostic{code, message, {}}};
}
bool same_context(const TaskInputContext& a, const TaskInputContext& b) {
    return a.document.id == b.document.id && a.document.epoch == b.document.epoch &&
           a.revision == b.revision && a.profile == b.profile;
}
std::string nonce() {
    std::random_device source;
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(8) << source() << std::setw(8) << source();
    return out.str();
}
class TaskCancelled : public std::exception {};
class TaskBudgetExceeded : public std::runtime_error {
  public:
    TaskBudgetExceeded() : std::runtime_error("Task progress event quota exceeded") {}
};
} // namespace
bool TaskControl::cancellation_requested() const {
    return cancelled_();
}
void TaskControl::checkpoint() const {
    if (cancellation_requested())
        throw TaskCancelled();
}
void TaskControl::progress(double value) const {
    checkpoint();
    progress_(value);
}
const std::string& TaskControl::task_id() const noexcept {
    return id_;
}

struct TaskService::State {
    struct Entry {
        TaskRecord record;
        TaskWork work;
        std::atomic<bool> cancelled{};
    };
    TaskPublisher publisher;
    TaskServiceLimits limits;
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::map<std::string, std::shared_ptr<Entry>, std::less<>> entries;
    std::deque<std::shared_ptr<Entry>> queue;
    std::vector<std::thread> workers;
    std::string application_nonce{nonce()};
    std::uint64_t next_id{1}, next_event{1};
    std::size_t active{};
    bool stopping{}, blocked{}, recovery_required{};

    State(TaskPublisher value, TaskServiceLimits settings)
        : publisher(std::move(value)), limits(settings) {
        if (!publisher.validate_session || !publisher.validate_input || !publisher.load ||
            !publisher.persist || !publisher.publish || !limits.workers || limits.workers > 2 ||
            !limits.queue_capacity || limits.queue_capacity > 8 || !limits.max_records)
            throw std::invalid_argument(
                "Task service requires complete callbacks and bounded worker/queue limits");
        auto result = reload();
        if (!result.ok())
            throw std::runtime_error(result.error->message);
    }
    Result<bool> reload(bool interrupt_unfinished = false) {
        if (active || !queue.empty())
            return failure<bool>(ErrorCode::invalid_input,
                                 "Active tasks must stop before reconciliation");
        const auto loaded = publisher.load();
        if (!loaded.ok())
            return {loaded.status, {}, loaded.error};
        if (loaded.value->size() > limits.max_records)
            return failure<bool>(ErrorCode::resource_limit, "Task record quota exceeded");
        std::map<std::string, std::shared_ptr<Entry>, std::less<>> restored;
        std::uint64_t sequence = 1;
        for (const auto& record : *loaded.value) {
            encode_task_record(record);
            if ((!task_terminal(record.state) && !interrupt_unfinished) ||
                record.state == TaskState::outcome_unknown)
                return failure<bool>(
                    ErrorCode::storage_uncertain,
                    "Explicit application recovery must interrupt unfinished tasks");
            auto entry = std::make_shared<Entry>();
            entry->record = record;
            if (!restored.emplace(record.id, std::move(entry)).second)
                return failure<bool>(ErrorCode::schema_unsupported, "Duplicate task identity");
            if (record.events.back().sequence == UINT64_MAX)
                return failure<bool>(ErrorCode::resource_limit, "Task event sequence exhausted");
            sequence = std::max(sequence, record.events.back().sequence + 1);
        }
        // A definite side-row rollback has no unknown model outcome. Once all workers
        // have stopped, explicit reconciliation can retire its durable unfinished rows.
        // Unknown commits still require application recovery before load() succeeds.
        for (auto& [unused, entry] : restored) {
            (void)unused;
            if (task_terminal(entry->record.state))
                continue;
            if (sequence == UINT64_MAX || entry->record.events.size() >= 128)
                return failure<bool>(ErrorCode::resource_limit,
                                     "Task recovery event quota exceeded");
            auto interrupted = entry->record;
            interrupted.state = TaskState::interrupted;
            interrupted.events.push_back({sequence++, interrupted.state, interrupted.progress});
            interrupted.diagnostic = Diagnostic{
                ErrorCode::storage_failure,
                "Task interrupted after a definite state-write failure; it was not rerun",
                interrupted.id};
            Result<bool> stored;
            try {
                stored = publisher.persist(interrupted, entry->record);
            } catch (const std::exception& error) {
                stored = failure<bool>(ErrorCode::storage_uncertain, error.what());
            } catch (...) {
                stored = failure<bool>(ErrorCode::storage_uncertain,
                                       "Task reconciliation outcome is unknown");
            }
            if ((stored.ok() && !*stored.value) || (!stored.ok() && !stored.error))
                stored = failure<bool>(ErrorCode::storage_uncertain,
                                       "Task reconciliation returned no durable acknowledgement");
            if (!stored.ok()) {
                recovery_required =
                    recovery_required || stored.error->code == ErrorCode::storage_uncertain;
                return stored;
            }
            entry->record = std::move(interrupted);
        }
        entries.swap(restored);
        next_event = sequence;
        blocked = false;
        recovery_required = false;
        changed.notify_all();
        return success(true);
    }
    void event(TaskRecord& record, TaskState state, double progress) {
        if (next_event == UINT64_MAX || record.events.size() >= 128)
            throw std::runtime_error("Task event quota exceeded");
        record.state = state;
        record.progress = progress;
        record.events.push_back({next_event++, state, progress});
    }
    void storage_stop(const std::shared_ptr<Entry>& entry, const Diagnostic& diagnostic) {
        blocked = true;
        recovery_required = recovery_required || diagnostic.code == ErrorCode::storage_uncertain;
        for (auto& [unused, value] : entries) {
            (void)unused;
            value->cancelled.store(true);
            if (!task_terminal(value->record.state)) {
                const auto terminal =
                    value == entry && diagnostic.code == ErrorCode::storage_uncertain
                        ? TaskState::outcome_unknown
                        : TaskState::interrupted;
                event(value->record, terminal, value->record.progress);
                value->record.diagnostic = diagnostic;
            }
        }
        for (auto& value : queue)
            value->work = {};
        queue.clear();
        changed.notify_all();
    }
    bool persist(const std::shared_ptr<Entry>& entry, TaskRecord next) {
        Result<bool> result;
        try {
            result = publisher.persist(next, entry->record);
        } catch (const std::exception& error) {
            result = failure<bool>(ErrorCode::storage_uncertain, error.what());
        } catch (...) {
            result =
                failure<bool>(ErrorCode::storage_uncertain, "Task persistence outcome is unknown");
        }
        if (result.ok() && !*result.value)
            result = failure<bool>(ErrorCode::storage_uncertain,
                                   "Task persistence did not acknowledge its write");
        if (!result.ok() && !result.error)
            result =
                failure<bool>(ErrorCode::storage_uncertain, "Task persistence returned no outcome");
        if (!result.ok()) {
            storage_stop(entry, *result.error);
            return false;
        }
        entry->record = std::move(next);
        changed.notify_all();
        return true;
    }
    void finish(const std::shared_ptr<Entry>& entry,
                TaskState state,
                std::optional<Diagnostic> diagnostic = {}) {
        if (blocked)
            return;
        auto next = entry->record;
        if (state == TaskState::cancelled && next.state == TaskState::running)
            event(next, TaskState::cancel_requested, next.progress);
        event(next, state, next.progress);
        next.diagnostic = std::move(diagnostic);
        persist(entry, std::move(next));
    }
    void run() {
        for (;;) {
            std::shared_ptr<Entry> entry;
            std::string identity;
            {
                std::unique_lock lock(mutex);
                changed.wait(lock, [&] { return stopping || (!blocked && !queue.empty()); });
                if (queue.empty()) {
                    if (stopping)
                        return;
                    continue;
                }
                entry = queue.front();
                queue.pop_front();
                identity = entry->record.id;
                if (stopping || entry->cancelled.load()) {
                    finish(entry, TaskState::cancelled);
                    entry->work = {};
                    continue;
                }
                auto running = entry->record;
                event(running, TaskState::running, running.progress);
                if (!persist(entry, std::move(running))) {
                    entry->work = {};
                    continue;
                }
                ++active;
                changed.notify_all();
            }
            TaskControl control;
            control.id_ = identity;
            control.cancelled_ = [entry] { return entry->cancelled.load(); };
            control.progress_ = [this, entry](double progress) {
                std::lock_guard lock(mutex);
                if (entry->cancelled.load() || blocked)
                    throw TaskCancelled();
                if (!std::isfinite(progress) || progress < entry->record.progress || progress > 1)
                    throw std::invalid_argument(
                        "Task progress must be finite, monotonic, and normalized");
                if (progress == entry->record.progress)
                    return;
                if (entry->record.events.size() >= 120)
                    throw TaskBudgetExceeded();
                auto next = entry->record;
                event(next, TaskState::running, progress);
                if (!persist(entry, std::move(next)))
                    throw TaskCancelled();
            };
            std::shared_ptr<const TaskPayload> payload;
            std::optional<Diagnostic> failure_reason;
            std::optional<TaskCompletion> completion;
            try {
                payload = entry->work(control);
                if (payload)
                    completion = payload->completion();
                if (completion && completion->state != TaskState::failed &&
                    completion->state != TaskState::cancelled &&
                    completion->state != TaskState::interrupted &&
                    completion->state != TaskState::outcome_unknown) {
                    completion.reset();
                    throw std::invalid_argument("Worker terminal override cannot certify success");
                }
            } catch (const TaskCancelled&) {
            } catch (const TaskBudgetExceeded& error) {
                failure_reason = Diagnostic{ErrorCode::resource_limit, error.what(), identity};
            } catch (const std::exception& error) {
                failure_reason = Diagnostic{ErrorCode::invalid_input, error.what(), identity};
            } catch (...) {
                failure_reason =
                    Diagnostic{ErrorCode::invalid_input, "Task worker failed", identity};
            }
            {
                std::lock_guard lock(mutex);
                if (!blocked) {
                    if (completion)
                        finish(entry, completion->state, std::move(completion->diagnostic));
                    else if (entry->cancelled.load() || stopping)
                        finish(entry, TaskState::cancelled);
                    else if (failure_reason || !payload)
                        finish(entry,
                               TaskState::failed,
                               failure_reason.value_or(
                                   Diagnostic{ErrorCode::invalid_input,
                                              "Task produced no complete candidate",
                                              entry->record.id}));
                    else {
                        auto committing = entry->record;
                        event(committing, TaskState::committing, committing.progress);
                        if (persist(entry, std::move(committing))) {
                            auto complete = entry->record;
                            event(complete, TaskState::succeeded, 1);
                            Result<TaskRecord> result;
                            try {
                                result =
                                    publisher.publish(payload, entry->record, std::move(complete));
                            } catch (const std::exception& error) {
                                result =
                                    failure<TaskRecord>(ErrorCode::storage_uncertain, error.what());
                            } catch (...) {
                                result = failure<TaskRecord>(ErrorCode::storage_uncertain,
                                                             "Task publication outcome is unknown");
                            }
                            if (!result.ok() && !result.error)
                                result = failure<TaskRecord>(ErrorCode::storage_uncertain,
                                                             "Task publisher returned no outcome");
                            if (result.ok()) {
                                try {
                                    encode_task_record(*result.value);
                                    if (result.value->id != entry->record.id ||
                                        result.value->state != TaskState::succeeded ||
                                        !same_context(result.value->input, entry->record.input))
                                        throw std::runtime_error(
                                            "Task publisher returned a mismatched completion fact");
                                } catch (const std::exception& error) {
                                    result = failure<TaskRecord>(ErrorCode::storage_uncertain,
                                                                 error.what());
                                }
                            }
                            if (result.ok()) {
                                entry->record = std::move(*result.value);
                            } else if (result.error->code == ErrorCode::storage_uncertain)
                                storage_stop(entry, *result.error);
                            else {
                                const bool conflict =
                                    result.status == Status::conflict ||
                                    result.error->code == ErrorCode::revision_conflict ||
                                    result.error->code == ErrorCode::document_epoch_expired ||
                                    result.error->code == ErrorCode::preview_expired;
                                finish(entry,
                                       conflict ? TaskState::conflicted : TaskState::failed,
                                       result.error);
                            }
                        }
                    }
                }
                entry->work = {};
                --active;
                changed.notify_all();
            }
        }
    }
};

TaskService::TaskService(TaskPublisher publisher, TaskServiceLimits limits)
    : state_(std::make_unique<State>(std::move(publisher), limits)) {
    try {
        for (std::size_t index = 0; index < limits.workers; ++index)
            state_->workers.emplace_back([state = state_.get()] { state->run(); });
    } catch (...) {
        {
            std::lock_guard lock(state_->mutex);
            state_->stopping = true;
        }
        state_->changed.notify_all();
        for (auto& worker : state_->workers)
            worker.join();
        throw;
    }
}
TaskService::~TaskService() {
    {
        std::lock_guard lock(state_->mutex);
        state_->stopping = true;
        for (auto& [unused, entry] : state_->entries) {
            (void)unused;
            entry->cancelled.store(true);
        }
    }
    state_->changed.notify_all();
    for (auto& worker : state_->workers)
        worker.join();
}
Result<TaskRecord> TaskService::start(TaskRequest request) {
    std::lock_guard lock(state_->mutex);
    if (state_->stopping || state_->blocked)
        return failure<TaskRecord>(ErrorCode::storage_uncertain,
                                   "Task service needs reconciliation");
    if (request.caller.principal.empty() || request.idempotency_key.empty() ||
        request.operation.empty() || request.signature.empty() || !request.work)
        return failure<TaskRecord>(ErrorCode::missing_input,
                                   "Task caller, key, operation, signature and work are required");
    if (request.caller.principal.size() > 1024 || request.idempotency_key.size() > 1024 ||
        request.operation.size() > 256 || request.signature.size() > 32768)
        return failure<TaskRecord>(ErrorCode::resource_limit,
                                   "Task request metadata exceeds quota");
    const auto session = state_->publisher.validate_session(request.input.document);
    if (!session.ok())
        return {session.status, {}, session.error};
    if (!state_->active && state_->queue.empty() &&
        (state_->entries.empty() ||
         std::none_of(state_->entries.begin(), state_->entries.end(), [&](const auto& item) {
             return item.second->record.input.document.id == request.input.document.id;
         }))) {
        const auto loaded = state_->reload();
        if (!loaded.ok())
            return {loaded.status, {}, loaded.error};
    }
    for (const auto& [unused, entry] : state_->entries) {
        (void)unused;
        const auto& record = entry->record;
        if (record.caller.principal == request.caller.principal &&
            record.idempotency_key == request.idempotency_key &&
            record.input.document.id == request.input.document.id) {
            if (record.operation != request.operation || record.signature != request.signature ||
                !same_context(record.input, request.input))
                return failure<TaskRecord>(ErrorCode::idempotency_key_conflict,
                                           "Task start key was used with different inputs");
            return success(record);
        }
    }
    if (state_->queue.size() >= state_->limits.queue_capacity ||
        state_->entries.size() >= state_->limits.max_records)
        return failure<TaskRecord>(ErrorCode::resource_limit,
                                   "Task queue or retained-record quota reached");
    auto accepted = state_->publisher.validate_input(request.input);
    if (!accepted.ok())
        return {accepted.status, {}, accepted.error};
    auto entry = std::make_shared<State::Entry>();
    auto& record = entry->record;
    record.id = "task-" + state_->application_nonce + "-" + std::to_string(state_->next_id++);
    record.caller = std::move(request.caller);
    record.idempotency_key = std::move(request.idempotency_key);
    record.operation = std::move(request.operation);
    record.signature = std::move(request.signature);
    record.input = std::move(request.input);
    state_->event(record, TaskState::queued, 0);
    entry->work = std::move(request.work);
    try {
        encode_task_record(record);
    } catch (const std::exception& error) {
        return failure<TaskRecord>(ErrorCode::invalid_input, error.what());
    }
    state_->entries.emplace(record.id, entry);
    try {
        state_->queue.push_back(entry);
    } catch (...) {
        state_->entries.erase(record.id);
        throw;
    }
    Result<bool> stored;
    try {
        stored = state_->publisher.persist(record, std::nullopt);
    } catch (const std::exception& error) {
        stored = failure<bool>(ErrorCode::storage_uncertain, error.what());
    } catch (...) {
        stored = failure<bool>(ErrorCode::storage_uncertain, "Task admission outcome is unknown");
    }
    if ((stored.ok() && !*stored.value) || (!stored.ok() && !stored.error))
        stored = failure<bool>(ErrorCode::storage_uncertain,
                               "Task admission returned no durable acknowledgement");
    if (!stored.ok()) {
        if (stored.error->code == ErrorCode::storage_uncertain)
            state_->storage_stop(entry, *stored.error);
        else {
            state_->queue.pop_back();
            state_->entries.erase(record.id);
        }
        return {stored.status, {}, stored.error};
    }
    state_->changed.notify_all();
    return success(record);
}
Result<TaskRecord> TaskService::query(const Caller& caller, std::string_view id) const {
    std::lock_guard lock(state_->mutex);
    if (state_->entries.empty() && !state_->active && !state_->blocked) {
        const auto loaded = state_->reload();
        if (!loaded.ok())
            return {loaded.status, {}, loaded.error};
    }
    const auto found = state_->entries.find(id);
    if (found == state_->entries.end() ||
        found->second->record.caller.principal != caller.principal)
        return failure<TaskRecord>(ErrorCode::entity_not_found, "Task does not exist for caller");
    return success(found->second->record);
}
Result<TaskCancelResult> TaskService::cancel(const Caller& caller, std::string_view id) {
    std::lock_guard lock(state_->mutex);
    const auto found = state_->entries.find(id);
    if (found == state_->entries.end() ||
        found->second->record.caller.principal != caller.principal)
        return failure<TaskCancelResult>(ErrorCode::entity_not_found,
                                         "Task does not exist for caller");
    auto entry = found->second;
    if (task_terminal(entry->record.state))
        return success(TaskCancelResult{entry->record, false});
    if (entry->record.state == TaskState::cancel_requested)
        return success(TaskCancelResult{entry->record, true});
    auto next = entry->record;
    state_->event(next, TaskState::cancel_requested, next.progress);
    const bool queued = entry->record.state == TaskState::queued;
    if (queued)
        state_->event(next, TaskState::cancelled, next.progress);
    if (!state_->persist(entry, std::move(next)))
        return failure<TaskCancelResult>(ErrorCode::storage_uncertain,
                                         "Task cancellation outcome needs reconciliation");
    entry->cancelled.store(true);
    if (queued) {
        std::erase(state_->queue, entry);
        entry->work = {};
    }
    state_->changed.notify_all();
    return success(TaskCancelResult{entry->record, true});
}
Result<TaskRecord> TaskService::wait(const Caller& caller, std::string_view id) {
    std::unique_lock lock(state_->mutex);
    const auto found = state_->entries.find(id);
    if (found == state_->entries.end() ||
        found->second->record.caller.principal != caller.principal)
        return failure<TaskRecord>(ErrorCode::entity_not_found, "Task does not exist for caller");
    auto entry = found->second;
    state_->changed.wait(lock, [&] { return task_terminal(entry->record.state); });
    return success(entry->record);
}
Result<bool> TaskService::reconcile() {
    std::lock_guard lock(state_->mutex);
    return state_->reload(state_->blocked && !state_->recovery_required);
}
std::size_t TaskService::active_workers() const {
    std::lock_guard lock(state_->mutex);
    return state_->active;
}
std::size_t TaskService::queued_tasks() const {
    std::lock_guard lock(state_->mutex);
    return state_->queue.size();
}
} // namespace qcae
