#pragma once
#include "qcae/record_application.hpp"
#include "qcae/task_service.hpp"

namespace qcae {
struct RecordTaskPayload final : TaskPayload {
    RecordPreparedOperation operation;
    explicit RecordTaskPayload(RecordPreparedOperation value) : operation(std::move(value)) {}
};
OwnedRowHandler task_row_handler();
TaskPublisher record_task_publisher(RecordApplication&,
                                    std::function<bool(const ProfileRef&)> profile_supported);
} // namespace qcae
