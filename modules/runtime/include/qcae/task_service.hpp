#pragma once
#include "qcae/application_types.hpp"
#include <functional>
#include <memory>
#include <string_view>

namespace qcae {
enum class TaskState {
    queued,
    running,
    cancel_requested,
    committing,
    succeeded,
    cancelled,
    conflicted,
    failed,
    interrupted,
    outcome_unknown
};
bool task_terminal(TaskState) noexcept;
const char* task_state_name(TaskState) noexcept;
struct TaskInputContext {
    DocumentRef document{DocumentId{}, DocumentEpoch{}};
    Revision revision{};
    ProfileRef
        profile; // All three strings empty means solver-independent; partial triples are invalid.
};
struct TaskEvent {
    std::uint64_t sequence{};
    TaskState state{TaskState::queued};
    double progress{};
};
struct TaskRecord {
    std::string id;
    Caller caller;
    std::string idempotency_key, operation, signature;
    TaskInputContext input;
    TaskState state{TaskState::queued};
    double progress{};
    std::vector<TaskEvent> events;
    std::optional<ChangeReceipt> receipt;
    std::optional<Diagnostic> diagnostic;
};
std::string encode_task_record(const TaskRecord&);
TaskRecord decode_task_record(std::string_view);
struct TaskPayload {
    virtual ~TaskPayload() = default;
};
class TaskControl {
  public:
    bool cancellation_requested() const;
    void checkpoint() const;
    void progress(double) const;
    const std::string& task_id() const noexcept;

  private:
    friend class TaskService;
    std::function<bool()> cancelled_;
    std::function<void(double)> progress_;
    std::string id_;
};
using TaskWork = std::function<std::shared_ptr<const TaskPayload>(const TaskControl&)>;
struct TaskRequest {
    Caller caller;
    std::string idempotency_key, operation, signature;
    TaskInputContext input;
    TaskWork work;
};
// Callbacks serialize through one document application. They must not re-enter TaskService.
struct TaskPublisher {
    std::function<Result<bool>(const DocumentRef&)> validate_session;
    std::function<Result<bool>(const TaskInputContext&)> validate_input;
    std::function<Result<std::vector<TaskRecord>>()> load;
    std::function<Result<bool>(const TaskRecord&, const std::optional<TaskRecord>&)> persist;
    std::function<Result<TaskRecord>(
        const std::shared_ptr<const TaskPayload>&, const TaskRecord& expected, TaskRecord success)>
        publish;
};
struct TaskServiceLimits {
    std::size_t workers{2};
    std::size_t queue_capacity{8};
    std::size_t max_records{1024};
};
struct TaskCancelResult {
    TaskRecord task;
    bool accepted{};
};
class TaskService {
  public:
    explicit TaskService(TaskPublisher, TaskServiceLimits = {});
    ~TaskService();
    TaskService(const TaskService&) = delete;
    TaskService& operator=(const TaskService&) = delete;
    Result<TaskRecord> start(TaskRequest);
    Result<TaskRecord> query(const Caller&, std::string_view task_id) const;
    Result<TaskCancelResult> cancel(const Caller&, std::string_view task_id);
    Result<TaskRecord> wait(const Caller&, std::string_view task_id);
    Result<bool>
    reconcile(); // Quiescent reload; unknown commits require application recovery first.
    std::size_t active_workers() const;
    std::size_t queued_tasks() const;

  private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace qcae
