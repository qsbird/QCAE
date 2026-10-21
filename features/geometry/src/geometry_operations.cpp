#include "qcae/geometry_operations.hpp"
#include "qcae/feature_operation.hpp"
#include "qcae/geometry_features.hpp"
#include "qcae/operation_inputs.hpp"
#include "qcae/planar_geometry.hpp"

namespace qcae::features::geometry {
using operations::GeometryCreateLineInput;
using operations::GeometryCreateRegionInput;
using operations::GeometryEvaluateLineInput;
using operations::GeometryLoopInput;
using operations::GeometryMoveEndpointInput;
using operations::InputTraits;
using operations::OperationContext;
using operations::Value;

namespace {
Result<Value> receipt_value(const Result<ChangeReceipt>& receipt) {
    if (!receipt.ok())
        return forward_failure<Value>(receipt);
    return success(operations::change_receipt_value(*receipt.value));
}

// The handlers keep their own signature functions, so retries of commits made before typed
// registration moved here still return the original receipt.
Result<Value> create_line(RecordApplication& app,
                          const OperationContext& ctx,
                          const GeometryCreateLineInput& input) {
    const LineGeometryInput line{input.start_mm, input.end_mm};
    return receipt_value(app.execute(ctx.caller,
                                     {*ctx.document, *ctx.expected_revision},
                                     "geometry.create_line",
                                     line_geometry_signature(line),
                                     create_line_handler(line),
                                     ctx.idempotency_key));
}

Result<Value> evaluate(RecordApplication& app,
                       const OperationContext& ctx,
                       const GeometryEvaluateLineInput& input) {
    const auto snapshot = app.snapshot(*ctx.document);
    if (!snapshot.ok())
        return forward_failure<Value>(snapshot);
    try {
        const auto position = evaluate_line(
            snapshot.value->records, records::GeometryId(input.geometry_id.value), input.u);
        return success(Value(Value::Object{
            {"geometry_id", Value(input.geometry_id.value)},
            {"position_mm",
             Value(Value::Array{Value(position[0]), Value(position[1]), Value(position[2])})},
            {"revision", Value(std::to_string(snapshot.value->info.revision))}}));
    } catch (const RecordError& error) {
        return failed<Value>(error);
    }
}

Result<Value> move_endpoint(RecordApplication& app,
                            const OperationContext& ctx,
                            const GeometryMoveEndpointInput& input) {
    const records::GeometryId geometry(input.geometry_id.value);
    return receipt_value(app.execute(ctx.caller,
                                     {*ctx.document, *ctx.expected_revision},
                                     "geometry.move_endpoint",
                                     line_endpoint_signature(geometry, input.end_mm),
                                     move_line_endpoint_handler(geometry, input.end_mm),
                                     ctx.idempotency_key));
}

// The region an input describes, as plain geometry.
PlanarRegion planar_region(const GeometryCreateRegionInput& input) {
    auto loop = [](const GeometryLoopInput& outline) {
        PlanarLoop result;
        for (const auto& corner : outline.corners)
            result.corners_mm.push_back(corner.position_mm);
        return result;
    };
    PlanarRegion region{loop(input.outer), {}};
    if (input.holes)
        for (const auto& hole : *input.holes)
            region.holes.push_back(loop(hole));
    return region;
}

// Adds one point per corner and the loop that joins them; returns the loop's identity.
EntityId add_loop(const GeometryLoopInput& outline, PlanEdit& edit) {
    records::GeometryLoop loop{edit.allocate(), {}};
    for (const auto& corner : outline.corners) {
        const EntityId point = edit.allocate();
        edit.put(records::GeometryPoint{point, corner.position_mm});
        loop.corners.push_back(point);
    }
    const EntityId id = loop.id;
    edit.put(std::move(loop));
    return id;
}
} // namespace

Result<OperationPlan> prepare_create_region(const GeometryCreateRegionInput& input) {
    auto canonical = canonical_copy(input);
    // The record rule checks the region again; checking the input first names the input field.
    const auto shape = measure_planar_region(planar_region(canonical));
    if (!shape.ok())
        return forward_failure<OperationPlan>(shape);
    return record_plan(
        std::move(canonical), [](const GeometryCreateRegionInput& region, PlanEdit& edit) {
            records::GeometryRegion record{
                records::GeometryId(edit.allocate().value), add_loop(region.outer, edit), {}, 1};
            if (region.holes)
                for (const auto& hole : *region.holes)
                    record.holes.push_back(add_loop(hole, edit));
            const EntityId id(record.id.value);
            edit.put(std::move(record));
            return ChangeSummary{"Create region", id, 0, true};
        });
}

Result<bool> register_handlers(operations::OperationRegistry& registry, RecordApplication& app) {
    auto registered = registry.register_typed<GeometryCreateLineInput>(
        InputTraits<GeometryCreateLineInput>::definition(),
        [&app](const OperationContext& ctx, const GeometryCreateLineInput& input) {
            return create_line(app, ctx, input);
        });
    if (registered.ok())
        registered = registry.register_typed<GeometryEvaluateLineInput>(
            InputTraits<GeometryEvaluateLineInput>::definition(),
            [&app](const OperationContext& ctx, const GeometryEvaluateLineInput& input) {
                return evaluate(app, ctx, input);
            });
    if (registered.ok())
        registered = registry.register_typed<GeometryMoveEndpointInput>(
            InputTraits<GeometryMoveEndpointInput>::definition(),
            [&app](const OperationContext& ctx, const GeometryMoveEndpointInput& input) {
                return move_endpoint(app, ctx, input);
            });
    if (registered.ok())
        registered = register_plan(registry, app, prepare_create_region);
    return registered;
}
} // namespace qcae::features::geometry
