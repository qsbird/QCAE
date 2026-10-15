#pragma once
#include "qcae/core.hpp"
#include "qcae/operation_registry.hpp"
#include "qcae/task_application.hpp"

namespace qcae::ipc::detail {
using operations::Value;
// The solver run an analysis.start task owns; analysis.get_run, get_result and validate_result take
// this identity, not the task id or the result artifact id.
inline std::string analysis_run_id(const std::string& task_id) {
    return "run-" + task_id;
}
inline Value task_value(const TaskRecord& task) {
    Value::Array events;
    for (const auto& event : task.events)
        events.emplace_back(Value::Object{{"sequence", Value(std::to_string(event.sequence))},
                                          {"state", Value(task_state_name(event.state))},
                                          {"progress", Value(event.progress)}});
    Value::Object object{{"task_id", Value(task.id)},
                         {"state", Value(task_state_name(task.state))},
                         {"progress", Value(task.progress)},
                         {"input_revision", Value(std::to_string(task.input.revision))},
                         {"events", Value(std::move(events))}};
    if (task.operation == "analysis.start")
        object.emplace("run_id", Value(analysis_run_id(task.id)));
    if (task.receipt)
        object.emplace("receipt", operations::change_receipt_value(*task.receipt));
    if (task.artifact_receipt)
        object.emplace(
            "artifact_receipt",
            Value(Value::Object{
                {"artifact_id", Value(task.artifact_receipt->artifact_id)},
                {"input_revision", Value(std::to_string(task.artifact_receipt->input_revision))},
                {"manifest_sha256", Value(task.artifact_receipt->manifest_sha256)}}));
    if (task.diagnostic)
        object.emplace("diagnostic",
                       Value(Value::Object{{"code", Value(error_name(task.diagnostic->code))},
                                           {"message", Value(task.diagnostic->message)},
                                           {"field", Value(task.diagnostic->field)}}));
    return Value(std::move(object));
}
template <class T, class Convert>
Result<Value> converted(const Result<T>& result, Convert convert) {
    if (!result.ok())
        return {result.status, {}, result.error};
    return {Status::success, convert(*result.value), {}};
}
} // namespace qcae::ipc::detail
