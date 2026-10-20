#include "qcae/geometry_operations.hpp"
#include "qcae/feature_operation.hpp"
#include "qcae/geometry_features.hpp"
#include "qcae/operation_inputs.hpp"

namespace qcae::features::geometry {
using operations::GeometryCreateLineInput;
using operations::GeometryEvaluateLineInput;
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
} // namespace

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
    return registered;
}
} // namespace qcae::features::geometry
