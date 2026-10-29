#pragma once
#include "qcae/feature_operation.hpp"
#include "qcae/operation_inputs.hpp"

namespace qcae::features::mesh_editing {
[[nodiscard]] Result<OperationPlan> prepare_move_node(const operations::NodeMoveInput&);
[[nodiscard]] Result<OperationPlan>
prepare_translate_nodes(const operations::NodeTranslateBatchInput&);
[[nodiscard]] Result<OperationPlan>
prepare_assign_section(const operations::BeamAssignSectionInput&);
[[nodiscard]] Result<OperationPlan> prepare_create_tri3(const operations::MeshCreateTri3Input&);
// Assigns a shell section to every Tri3 of a mesh.
[[nodiscard]] Result<OperationPlan>
prepare_assign_shell_section(const operations::MeshAssignShellSectionInput&);
[[nodiscard]] Result<bool> register_handlers(operations::OperationRegistry&, RecordApplication&);
} // namespace qcae::features::mesh_editing
