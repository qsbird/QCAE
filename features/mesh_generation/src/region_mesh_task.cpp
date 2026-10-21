#include "qcae/region_mesh_task.hpp"
#include "qcae/region_geometry.hpp"
#include <cmath>

namespace qcae {
records::MeshId region_mesh_identity(std::string_view task_id) {
    return records::MeshId("mesh-" + std::string(task_id));
}

TaskRequest region_mesh_task(const RecordSnapshot& snapshot,
                             Caller caller,
                             RegionMeshInput input,
                             std::string key,
                             std::shared_ptr<const IPlanarMesher> mesher) {
    if (!std::isfinite(input.element_size_mm) || input.element_size_mm <= 0)
        throw RecordError(ErrorCode::invalid_input,
                          "The element size must be a positive number of millimetres",
                          "element_size_mm");
    const std::array fields{std::string("mesh.region.v1"),
                            input.region.value,
                            record_wire::real(input.element_size_mm),
                            mesher->identity()};
    const auto signature = record_wire::strings(fields);
    TaskRequest request;
    request.caller = std::move(caller);
    request.idempotency_key = std::move(key);
    request.operation = "mesh.generate_region";
    request.signature = signature;
    request.input = {snapshot.info.document, snapshot.info.revision, {}};
    request.work = [base = snapshot.records, input = std::move(input), signature, mesher](
                       const TaskControl& control) -> std::shared_ptr<const TaskPayload> {
        // Resolve the region only for new work: a completed task replays its persisted fact.
        control.checkpoint();
        const auto record = base.find<records::GeometryRegion>(input.region);
        if (!record)
            throw RecordError(
                ErrorCode::entity_not_found, "Region geometry does not exist", input.region.value);
        const auto& region = record->get<records::GeometryRegion>();
        const auto mesh_id = region_mesh_identity(control.task_id());
        if (base.find_identity(mesh_id.value))
            throw RecordError(ErrorCode::invalid_input, "Generated mesh ID already exists");
        control.progress(.1);

        const auto meshed = mesher->mesh(records::planar_region(base, region),
                                         {input.element_size_mm, region_mesh_max_triangles});
        if (!meshed.ok())
            throw RecordError(meshed.error->code, meshed.error->message, meshed.error->field);
        // The mesher cannot be interrupted; a cancel that arrived meanwhile takes effect here,
        // before anything is published.
        control.checkpoint();
        control.progress(.7);

        const auto& mesh = *meshed.value;
        EditSession edit(base);
        edit.put(records::Mesh{
            mesh_id, "Region mesh", "geometry", input.region, region.geometry_revision, false});
        std::vector<EntityId> nodes;
        nodes.reserve(mesh.nodes_mm.size());
        for (std::size_t index = 0; index < mesh.nodes_mm.size(); ++index) {
            EntityId node("node-" + control.task_id() + "-" + std::to_string(index));
            if (base.find_identity(node.value))
                throw RecordError(ErrorCode::invalid_input, "Generated node ID already exists");
            edit.put(records::Node{node, mesh.nodes_mm[index], mesh_id});
            nodes.push_back(std::move(node));
        }
        control.checkpoint();
        for (std::size_t index = 0; index < mesh.triangles.size(); ++index) {
            EntityId triangle("tri-" + control.task_id() + "-" + std::to_string(index));
            if (base.find_identity(triangle.value))
                throw RecordError(ErrorCode::invalid_input, "Generated triangle ID already exists");
            const auto& corners = mesh.triangles[index];
            edit.put(records::Tri3{
                triangle, {nodes[corners[0]], nodes[corners[1]], nodes[corners[2]]}, mesh_id});
        }
        control.progress(.95);
        auto change = edit.prepare();
        if (change.changes.records.size() != 1 + mesh.nodes_mm.size() + mesh.triangles.size())
            throw RecordError(ErrorCode::invalid_input, "Mesher produced an incomplete candidate");
        control.checkpoint();
        return std::make_shared<const RecordTaskPayload>(
            RecordPreparedOperation{std::move(change),
                                    "Generate region mesh",
                                    EntityId(mesh_id.value),
                                    signature,
                                    0,
                                    true});
    };
    return request;
}
} // namespace qcae
