#include "qcae/line_mesh_task.hpp"
#include <cmath>

namespace qcae {
records::MeshId line_mesh_identity(std::string_view task_id) {
    return records::MeshId("mesh-" + std::string(task_id));
}
TaskRequest line_mesh_task(const RecordSnapshot& snapshot,
                           Caller caller,
                           ProfileRef profile,
                           LineMeshInput input,
                           std::string key) {
    if (!input.segments || input.segments > 100000)
        throw RecordError(ErrorCode::resource_limit, "Line segments must be between 1 and 100000");
    const std::array fields{std::string("mesh.line.uniform.v1"),
                            input.geometry.value,
                            record_wire::number(input.segments),
                            input.section ? input.section->value : std::string()};
    const auto signature = record_wire::strings(fields);
    TaskRequest request;
    request.caller = std::move(caller);
    request.idempotency_key = std::move(key);
    request.operation = "mesh.generate_line";
    request.signature = signature;
    request.input = {snapshot.info.document, snapshot.info.revision, std::move(profile)};
    request.work = [base = snapshot.records, input = std::move(input), signature](
                       const TaskControl& control) -> std::shared_ptr<const TaskPayload> {
        // Resolve references only for new work. A completed task must replay its
        // persisted fact even after undo removes its original geometry or section.
        control.checkpoint();
        const auto geometry = base.find<records::GeometryLine>(input.geometry);
        if (!geometry)
            throw RecordError(
                ErrorCode::entity_not_found, "Line geometry does not exist", input.geometry.value);
        if (input.section && !base.find<records::BeamSection>(*input.section))
            throw RecordError(
                ErrorCode::invalid_input, "Assigned section does not exist", input.section->value);
        const auto& line = geometry->get<records::GeometryLine>();
        const auto mesh = line_mesh_identity(control.task_id());
        auto node_id = [&](std::uint32_t index) {
            return EntityId("node-" + control.task_id() + "-" + std::to_string(index));
        };
        auto beam_id = [&](std::uint32_t index) {
            return EntityId("line-" + control.task_id() + "-" + std::to_string(index));
        };
        if (base.find_identity(mesh.value))
            throw RecordError(ErrorCode::invalid_input, "Generated mesh ID already exists");
        std::array<double, 3> axis{};
        for (std::size_t dimension = 0; dimension < 3; ++dimension)
            axis[dimension] = line.end[dimension] - line.start[dimension];
        const double length = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
        const std::array<double, 3> orientation = std::abs(axis[2] / length) < .9
                                                      ? std::array<double, 3>{0, 0, 1}
                                                      : std::array<double, 3>{0, 1, 0};
        EditSession edit(base);
        edit.put(records::Mesh{
            mesh, "Uniform line mesh", "geometry", input.geometry, line.geometry_revision, false});
        const auto interval = std::max<std::uint32_t>(1, input.segments / 10);
        for (std::uint32_t index = 0; index <= input.segments; ++index) {
            control.checkpoint();
            if (base.find_identity(node_id(index).value))
                throw RecordError(ErrorCode::invalid_input, "Generated node ID already exists");
            std::array<double, 3> position{};
            for (std::size_t dimension = 0; dimension < 3; ++dimension)
                position[dimension] = index == input.segments
                                          ? line.end[dimension]
                                          : line.start[dimension] +
                                                axis[dimension] * (double(index) / input.segments);
            edit.put(records::Node{node_id(index), position, mesh});
            if (index % interval == 0)
                control.progress(.8 * double(index) / input.segments);
        }
        for (std::uint32_t index = 0; index < input.segments; ++index) {
            control.checkpoint();
            if (base.find_identity(beam_id(index).value))
                throw RecordError(ErrorCode::invalid_input, "Generated element ID already exists");
            edit.put(records::Beam{beam_id(index),
                                   input.section,
                                   {node_id(index), node_id(index + 1)},
                                   orientation,
                                   mesh});
        }
        control.progress(.95);
        auto change = edit.prepare();
        if (change.changes.records.size() != std::size_t(input.segments) * 2 + 2)
            throw RecordError(ErrorCode::invalid_input, "Mesher produced an incomplete candidate");
        control.checkpoint();
        return std::make_shared<const RecordTaskPayload>(
            RecordPreparedOperation{std::move(change),
                                    "Generate uniform line mesh",
                                    EntityId(mesh.value),
                                    signature,
                                    0,
                                    true});
    };
    return request;
}
} // namespace qcae
