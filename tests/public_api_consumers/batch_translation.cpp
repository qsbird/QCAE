#include "qcae/mesh_editing_operations.hpp"

int main() {
    const qcae::operations::NodeTranslateBatchInput input{
        {qcae::EntityId("node4"), qcae::EntityId("node5")}, {0, "mm"}, {1, "mm"}, {0, "mm"}};
    const auto plan = qcae::features::mesh_editing::prepare_translate_nodes(input);
    const auto decoded =
        qcae::operations::InputTraits<qcae::operations::NodeTranslateBatchInput>::from_value(
            qcae::operations::InputTraits<qcae::operations::NodeTranslateBatchInput>::to_value(
                input));
    return plan.ok() && decoded.ok() ? 0 : 1;
}
