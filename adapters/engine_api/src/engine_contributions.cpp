#include "qcae/engine_contributions.hpp"
#include "qcae/records.hpp"
#include "qcae/geometry_features.hpp"
#include "qcae/line_mesh_task.hpp"
#include "qcae/material_operations.hpp"
#include "qcae/mesh_editing_operations.hpp"
#include "typed_values.hpp"
#include <set>

namespace qcae::ipc {
using namespace operations;
using namespace detail;
TypedHost::OperationContributor TypedHost::default_operations() {
    return [](OperationRegistry& registry,
              RecordApplication& app,
              std::function<TaskService&()> task_service) -> Result<bool> {
        const auto materials = features::materials::register_handlers(registry, app);
        if (!materials.ok())
            return materials;
        const auto mesh_editing = features::mesh_editing::register_handlers(registry, app);
        if (!mesh_editing.ok())
            return mesh_editing;
        const auto geometry = registry.register_typed<GeometryCreateLineInput>(
            InputTraits<GeometryCreateLineInput>::definition(),
            [&app](const OperationContext& ctx, const GeometryCreateLineInput& input) {
                const LineGeometryInput line{input.start_mm, input.end_mm};
                return converted(app.execute(ctx.caller,
                                             {*ctx.document, *ctx.expected_revision},
                                             "geometry.create_line",
                                             line_geometry_signature(line),
                                             create_line_handler(line),
                                             ctx.idempotency_key),
                                 change_receipt_value);
            });
        if (!geometry.ok())
            return geometry;
        return registry.register_typed<MeshGenerateLineInput>(
            InputTraits<MeshGenerateLineInput>::definition(),
            [&app, task_service = std::move(task_service)](
                const OperationContext& ctx, const MeshGenerateLineInput& input) -> Result<Value> {
                const auto snapshot = app.snapshot(*ctx.document);
                if (!snapshot.ok())
                    return {snapshot.status, {}, snapshot.error};
                try {
                    auto request = line_mesh_task(
                        *snapshot.value,
                        ctx.caller,
                        {},
                        {records::GeometryId(input.geometry_id.value), input.segments, {}},
                        ctx.idempotency_key);
                    // Deduplication compares the original submitted context before freshness
                    // checks.
                    request.input.revision = *ctx.expected_revision;
                    return converted(task_service().start(std::move(request)), task_value);
                } catch (const RecordError& error) {
                    return {
                        Status::failed, {}, Diagnostic{error.code(), error.what(), error.field()}};
                }
            });
    };
}

std::vector<EngineContribution> default_engine_contributions() {
    return {{"qcae.model",
             [](RecordRegistry& registry) {
                 for (auto descriptor : generated_record_descriptors())
                     registry.add(std::move(descriptor));
                 registry.add_rule(records::validate_relations);
             },
             TypedHost::default_operations()}};
}
EngineAssembly assemble_engine(std::span<const EngineContribution> contributions) {
    auto registry = std::make_shared<RecordRegistry>();
    std::set<std::string> identities;
    std::vector<TypedHost::OperationContributor> contributors;
    for (const auto& contribution : contributions) {
        if (contribution.id.empty() || !identities.insert(contribution.id).second ||
            (!contribution.records && !contribution.operations))
            throw RecordError(
                ErrorCode::invalid_input,
                "Engine contribution identity must be unique and nonempty, with a registration",
                "contribution");
        if (contribution.records)
            contribution.records(*registry);
        if (contribution.operations)
            contributors.push_back(contribution.operations);
    }
    registry->freeze();
    return {std::move(registry),
            [contributors = std::move(contributors)](operations::OperationRegistry& target,
                                                     RecordApplication& app,
                                                     std::function<TaskService&()> tasks) {
                for (const auto& contribute : contributors) {
                    auto result = contribute(target, app, tasks);
                    if (!result.ok())
                        return result;
                }
                return Result<bool>{Status::success, true, {}};
            }};
}
} // namespace qcae::ipc
