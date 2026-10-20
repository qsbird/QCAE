#pragma once
#include "qcae/operation_registry.hpp"
#include "qcae/record_application.hpp"

namespace qcae::features::geometry {
// Registers geometry.create_line, geometry.evaluate_line and geometry.move_endpoint.
[[nodiscard]] Result<bool> register_handlers(operations::OperationRegistry&, RecordApplication&);
} // namespace qcae::features::geometry
