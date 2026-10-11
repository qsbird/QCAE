#include "qcae/mesh_editing_operations.hpp"
#include <algorithm>

namespace qcae::features::mesh_editing {
using namespace operations;

Result<OperationPlan> prepare_move_node(const NodeMoveInput& input) {
    auto canonical = canonical_copy(input);
    for (std::size_t axis = 0; axis < 3; ++axis) {
        auto coordinate = parameters::canonical_quantity({input.position_mm[axis], "mm"},
                                                         parameters::Dimension::length);
        if (!coordinate.ok()) {
            coordinate.error->field = "position_mm[" + std::to_string(axis) + "]";
            return forward_failure<OperationPlan>(coordinate);
        }
        canonical.position_mm[axis] = coordinate.value->value;
    }
    return record_plan(std::move(canonical), [](const NodeMoveInput& canonical, PlanEdit& edit) {
        edit.update<records::Node>(canonical.entity_id,
                                   [&](auto& node) { node.position = canonical.position_mm; });
        return ChangeSummary{"Move node", canonical.entity_id, 0, false};
    });
}

Result<OperationPlan> prepare_assign_section(const BeamAssignSectionInput& input) {
    const auto identities = wire::entity_id_array(wire::to_value(input.beam_ids), "beam_ids");
    if (!identities.ok())
        return forward_failure<OperationPlan>(identities);
    const auto section = wire::entity_id(Value(input.section_id.value), "section_id");
    if (!section.ok())
        return forward_failure<OperationPlan>(section);
    auto canonical = canonical_copy(input);
    std::sort(canonical.beam_ids.begin(), canonical.beam_ids.end());
    return record_plan(
        std::move(canonical), [](const BeamAssignSectionInput& canonical, PlanEdit& edit) {
            for (const auto& beam : canonical.beam_ids)
                edit.update<records::Beam>(
                    beam, [&](auto& record) { record.section = canonical.section_id; });
            return ChangeSummary{"Assign beam section", canonical.beam_ids.front(), 0, false};
        });
}

Result<OperationPlan> prepare_translate_nodes(const NodeTranslateBatchInput& input) {
    const auto identities = wire::entity_id_array(wire::to_value(input.node_ids), "node_ids");
    if (!identities.ok())
        return forward_failure<OperationPlan>(identities);
    auto canonical = canonical_copy(input);
    const std::array components{&canonical.x, &canonical.y, &canonical.z};
    constexpr std::array names{"x", "y", "z"};
    for (std::size_t axis = 0; axis < components.size(); ++axis) {
        auto component =
            parameters::canonical_quantity(*components[axis], parameters::Dimension::length);
        if (!component.ok()) {
            component.error->field = names[axis];
            return forward_failure<OperationPlan>(component);
        }
        *components[axis] = std::move(*component.value);
    }
    std::sort(canonical.node_ids.begin(), canonical.node_ids.end());
    return record_plan(
        std::move(canonical), [](const NodeTranslateBatchInput& canonical, PlanEdit& edit) {
            const std::array offset{canonical.x.value, canonical.y.value, canonical.z.value};
            for (const auto& identity : canonical.node_ids) {
                edit.update<records::Node>(identity, [&](auto& node) {
                    for (std::size_t axis = 0; axis < offset.size(); ++axis) {
                        const auto coordinate = parameters::canonical_quantity(
                            {node.position[axis] + offset[axis], "mm"},
                            parameters::Dimension::length);
                        if (!coordinate.ok())
                            throw RecordError(coordinate.error->code,
                                              coordinate.error->message,
                                              "position[" + std::to_string(axis) + "]");
                        node.position[axis] = coordinate.value->value;
                    }
                });
            }
            return ChangeSummary{"Translate nodes", canonical.node_ids.front(), 0, false};
        });
}

Result<OperationPlan> prepare_create_tri3(const MeshCreateTri3Input& input) {
    const auto nodes = wire::entity_id_array(wire::to_value(input.node_ids), "node_ids");
    if (!nodes.ok())
        return forward_failure<OperationPlan>(nodes);
    const auto mesh = wire::entity_id(Value(input.mesh_id.value), "mesh_id");
    if (!mesh.ok())
        return forward_failure<OperationPlan>(mesh);
    if (input.node_ids.size() != 3)
        return {
            Status::failed,
            std::nullopt,
            Diagnostic{ErrorCode::invalid_input, "Tri3 requires exactly three nodes", "node_ids"}};
    // Node order is topology: preserve its winding in both storage and retry signatures.
    return record_plan(
        canonical_copy(input), [](const MeshCreateTri3Input& canonical, PlanEdit& edit) {
            const auto id = edit.allocate();
            const auto& nodes = canonical.node_ids;
            edit.put(records::Tri3{
                id, {nodes[0], nodes[1], nodes[2]}, records::MeshId(canonical.mesh_id.value)});
            return ChangeSummary{"Create Tri3 topology", id, 0, true};
        });
}

Result<bool> register_handlers(OperationRegistry& registry, RecordApplication& app) {
    auto registered = register_plan(registry, app, prepare_move_node);
    if (registered.ok())
        registered = register_plan(registry, app, prepare_assign_section);
    if (registered.ok())
        registered = register_plan(registry, app, prepare_translate_nodes);
    if (registered.ok())
        registered = registry.register_typed<NodeTranslateBatchPreviewInput>(
            InputTraits<NodeTranslateBatchPreviewInput>::definition(),
            [&app](const OperationContext& context,
                   const NodeTranslateBatchPreviewInput& input) -> Result<Value> {
                const auto plan =
                    prepare_translate_nodes({input.node_ids, input.x, input.y, input.z});
                const auto preview = preview_plan(app, context, plan);
                if (!preview.ok())
                    return forward_failure<Value>(preview);
                const auto revision = std::to_string(preview.value->context.expected_revision);
                return success(Value(Value::Object{
                    {"preview_id", Value(preview.value->id.value)},
                    {"affected_entity_id", Value(preview.value->affected_entity.value)},
                    {"revision", Value(revision)}}));
            });
    if (registered.ok())
        registered = register_plan(registry, app, prepare_create_tri3);
    return registered;
}
} // namespace qcae::features::mesh_editing
