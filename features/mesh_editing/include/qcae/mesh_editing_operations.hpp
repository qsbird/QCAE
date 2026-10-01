#pragma once
#include "qcae/operation_inputs.hpp"
#include "qcae/record_application.hpp"

namespace qcae::features::mesh_editing {
struct OperationPlan {
    std::string signature;
    RecordPrepare prepare;
};
[[nodiscard]] Result<OperationPlan> prepare_move_node(const operations::NodeMoveInput&);
[[nodiscard]] Result<OperationPlan>
prepare_translate_nodes(const operations::NodeTranslateBatchInput&);
[[nodiscard]] Result<OperationPlan>
prepare_assign_section(const operations::BeamAssignSectionInput&);
[[nodiscard]] Result<bool> register_handlers(operations::OperationRegistry&, RecordApplication&);
} // namespace qcae::features::mesh_editing
