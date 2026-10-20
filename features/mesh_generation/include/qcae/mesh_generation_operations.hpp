#pragma once
#include "qcae/operation_registry.hpp"
#include "qcae/record_application.hpp"
#include "qcae/task_application.hpp"
#include <functional>

namespace qcae::features::mesh_generation {
// Starts a mesh task and returns its wire status. The host supplies it because it owns the task
// service and the task status format it shares with its other background operations.
using StartTask = std::function<Result<operations::Value>(TaskRequest)>;

// Registers mesh.regenerate_line and mesh.generate_line.
[[nodiscard]] Result<bool>
register_handlers(operations::OperationRegistry&, RecordApplication&, StartTask);
} // namespace qcae::features::mesh_generation
