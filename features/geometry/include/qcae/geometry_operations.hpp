#pragma once
#include "qcae/feature_operation.hpp"
#include "qcae/operation_inputs.hpp"

namespace qcae::features::geometry {
// Creates a flat region in one change: one GeometryPoint per corner, one GeometryLoop per outline
// and the GeometryRegion that joins the outer loop and its holes. An invalid region is refused
// before anything is created, with the input field at fault ("outer" or "holes[i]").
[[nodiscard]] Result<OperationPlan>
prepare_create_region(const operations::GeometryCreateRegionInput&);

// Registers geometry.create_line, geometry.evaluate_line, geometry.move_endpoint and
// geometry.create_region.
[[nodiscard]] Result<bool> register_handlers(operations::OperationRegistry&, RecordApplication&);
} // namespace qcae::features::geometry
