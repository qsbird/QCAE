#include "qcae/mesh_editing_operations.hpp"
#include "qcae/quantities.hpp"
#include "qcae/records.hpp"
#include <algorithm>

namespace qcae::features::mesh_editing {
namespace {
using namespace operations;
template <class T> Result<T> success(T value) {
    return {Status::success, std::move(value), std::nullopt};
}
template <class T> Result<T> failed(const RecordError& error) {
    return {Status::failed, std::nullopt, Diagnostic{error.code(), error.what(), error.field()}};
}

template <class Input, class Factory>
Result<Value> execute(RecordApplication& app,
                      const OperationContext& context,
                      const Input& input,
                      Factory factory) {
    const auto plan = factory(input);
    if (!plan.ok()) {
        return {plan.status, std::nullopt, plan.error};
    }
    const auto receipt = app.execute(context.caller,
                                     WriteContext{*context.document, *context.expected_revision},
                                     std::string(InputTraits<Input>::operation_id),
                                     plan.value->signature,
                                     plan.value->prepare,
                                     context.idempotency_key);
    if (!receipt.ok()) {
        return {receipt.status, std::nullopt, receipt.error};
    }
    return success(change_receipt_value(*receipt.value));
}
} // namespace

Result<OperationPlan> prepare_move_node(const operations::NodeMoveInput& input) {
    auto canonical = input;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        auto coordinate = parameters::canonical_quantity({input.position_mm[axis], "mm"},
                                                         parameters::Dimension::length);
        if (!coordinate.ok()) {
            coordinate.error->field = "position_mm[" + std::to_string(axis) + "]";
            return {coordinate.status, std::nullopt, coordinate.error};
        }
        canonical.position_mm[axis] = coordinate.value->value;
    }
    const auto signature = canonical_value(InputTraits<NodeMoveInput>::to_value(canonical));
    if (!signature.ok()) {
        return {signature.status, std::nullopt, signature.error};
    }
    RecordPrepare prepare = [canonical, signature = *signature.value](
                                const DocumentView& view, const RecordIdentityAllocator&) {
        try {
            EditSession edit(view);
            edit.update<records::Node>(canonical.entity_id,
                                       [&](auto& node) { node.position = canonical.position_mm; });
            return success(RecordPreparedOperation{
                edit.prepare(), "Move node", canonical.entity_id, signature, 0, false});
        } catch (const RecordError& error) {
            return failed<RecordPreparedOperation>(error);
        }
    };
    return success(OperationPlan{*signature.value, std::move(prepare)});
}

Result<OperationPlan> prepare_assign_section(const operations::BeamAssignSectionInput& input) {
    const auto identities = wire::entity_id_array(wire::to_value(input.beam_ids), "beam_ids");
    const auto section = wire::entity_id(Value(input.section_id.value), "section_id");
    if (!identities.ok()) {
        return {identities.status, std::nullopt, identities.error};
    }
    if (!section.ok()) {
        return {section.status, std::nullopt, section.error};
    }
    auto canonical = input;
    std::sort(canonical.beam_ids.begin(), canonical.beam_ids.end());
    const auto signature =
        canonical_value(InputTraits<BeamAssignSectionInput>::to_value(canonical));
    if (!signature.ok()) {
        return {signature.status, std::nullopt, signature.error};
    }
    RecordPrepare prepare = [canonical, signature = *signature.value](
                                const DocumentView& view, const RecordIdentityAllocator&) {
        try {
            EditSession edit(view);
            for (const auto& beam : canonical.beam_ids) {
                edit.update<records::Beam>(
                    beam, [&](auto& record) { record.section = canonical.section_id; });
            }
            return success(RecordPreparedOperation{edit.prepare(),
                                                   "Assign beam section",
                                                   canonical.beam_ids.front(),
                                                   signature,
                                                   0,
                                                   false});
        } catch (const RecordError& error) {
            return failed<RecordPreparedOperation>(error);
        }
    };
    return success(OperationPlan{*signature.value, std::move(prepare)});
}

Result<bool> register_handlers(operations::OperationRegistry& registry, RecordApplication& app) {
    const auto registered = registry.register_typed<NodeMoveInput>(
        InputTraits<NodeMoveInput>::definition(),
        [&app](const OperationContext& context, const NodeMoveInput& input) {
            return execute(app, context, input, prepare_move_node);
        });
    if (!registered.ok())
        return registered;
    return registry.register_typed<BeamAssignSectionInput>(
        InputTraits<BeamAssignSectionInput>::definition(),
        [&app](const OperationContext& context, const BeamAssignSectionInput& input) {
            return execute(app, context, input, prepare_assign_section);
        });
}
} // namespace qcae::features::mesh_editing
