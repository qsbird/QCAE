#include "qcae/mesh_generation_operations.hpp"
#include "qcae/feature_operation.hpp"
#include "qcae/line_mesh_task.hpp"
#include "qcae/operation_inputs.hpp"

namespace qcae::features::mesh_generation {
using operations::InputTraits;
using operations::MeshGenerateLineInput;
using operations::MeshRegenerateLineInput;
using operations::OperationContext;
using operations::Value;

namespace {
// Builds a task request from the current snapshot and starts it. Deduplication compares the
// originally submitted context before freshness checks, so the request carries the caller's
// expected revision, not the snapshot's.
template <class MakeRequest>
Result<Value> start_from_snapshot(RecordApplication& app,
                                  const OperationContext& ctx,
                                  const StartTask& start,
                                  MakeRequest make_request) {
    const auto snapshot = app.snapshot(*ctx.document);
    if (!snapshot.ok())
        return forward_failure<Value>(snapshot);
    try {
        TaskRequest request = make_request(*snapshot.value);
        request.input.revision = *ctx.expected_revision;
        return start(std::move(request));
    } catch (const RecordError& error) {
        return failed<Value>(error);
    }
}
} // namespace

Result<bool> register_handlers(operations::OperationRegistry& registry,
                               RecordApplication& app,
                               StartTask start) {
    auto registered = registry.register_typed<MeshRegenerateLineInput>(
        InputTraits<MeshRegenerateLineInput>::definition(),
        [&app, start](const OperationContext& ctx, const MeshRegenerateLineInput& input) {
            return start_from_snapshot(app, ctx, start, [&](const RecordSnapshot& snapshot) {
                return regenerate_line_mesh_task(snapshot,
                                                 ctx.caller,
                                                 records::MeshId(input.mesh_id.value),
                                                 input.segments,
                                                 input.replacement_policy,
                                                 ctx.idempotency_key);
            });
        });
    if (registered.ok())
        registered = registry.register_typed<MeshGenerateLineInput>(
            InputTraits<MeshGenerateLineInput>::definition(),
            [&app, start = std::move(start)](const OperationContext& ctx,
                                             const MeshGenerateLineInput& input) {
                return start_from_snapshot(app, ctx, start, [&](const RecordSnapshot& snapshot) {
                    return line_mesh_task(
                        snapshot,
                        ctx.caller,
                        {},
                        {records::GeometryId(input.geometry_id.value), input.segments, {}},
                        ctx.idempotency_key);
                });
            });
    return registered;
}
} // namespace qcae::features::mesh_generation
