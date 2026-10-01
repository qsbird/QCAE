#include "qcae/geometry_features.hpp"
#include <cmath>
#include <numeric>

namespace qcae {
std::array<double, 3>
evaluate_line(const DocumentView& view, const records::GeometryId& id, double u) {
    if (!std::isfinite(u) || u < 0 || u > 1)
        throw RecordError(ErrorCode::invalid_input, "Line parameter must be between 0 and 1", "u");
    const auto record = view.find<records::GeometryLine>(id);
    if (!record)
        throw RecordError(ErrorCode::entity_not_found, "Line geometry does not exist", id.value);
    const auto& line = record->get<records::GeometryLine>();
    std::array<double, 3> position;
    for (std::size_t axis = 0; axis < position.size(); ++axis)
        position[axis] = std::lerp(line.start[axis], line.end[axis], u);
    return position;
}
std::string line_geometry_signature(const LineGeometryInput& input) {
    const std::array fields{std::string("geometry.line.v1:mm"),
                            record_wire::vector3(input.start_mm),
                            record_wire::vector3(input.end_mm)};
    return record_wire::strings(fields);
}
RecordPrepare create_line_handler(LineGeometryInput input) {
    return [input](const DocumentView& view,
                   const RecordIdentityAllocator& allocate) -> Result<RecordPreparedOperation> {
        const auto id = allocate();
        EditSession edit(view);
        edit.put(
            records::GeometryLine{records::GeometryId(id.value), input.start_mm, input.end_mm, 1});
        return {Status::success,
                RecordPreparedOperation{
                    edit.prepare(), "Create line", id, line_geometry_signature(input), 0, true},
                {}};
    };
}
std::string line_endpoint_signature(const records::GeometryId& id,
                                    const std::array<double, 3>& endpoint) {
    const std::array signature{
        std::string("geometry.line.endpoint.v1:mm"), id.value, record_wire::vector3(endpoint)};
    return record_wire::strings(signature);
}
RecordPrepare move_line_endpoint_handler(records::GeometryId id, std::array<double, 3> endpoint) {
    return [id = std::move(id),
            endpoint](const DocumentView& view,
                      const RecordIdentityAllocator&) -> Result<RecordPreparedOperation> {
        EditSession edit(view);
        edit.update<records::GeometryLine>(id, [&](auto& line) {
            if (line.geometry_revision == UINT64_MAX)
                throw RecordError(ErrorCode::resource_limit, "Geometry revision exhausted");
            line.end = endpoint;
            ++line.geometry_revision;
        });
        view.visit(RecordTraits<records::Mesh>::type_id, [&](const Record& record) {
            const auto& mesh = record->get<records::Mesh>();
            if (mesh.geometry == id)
                edit.update<records::Mesh>(mesh.id, [](auto& value) { value.stale = true; });
        });
        return {Status::success,
                RecordPreparedOperation{edit.prepare(),
                                        "Move line endpoint",
                                        EntityId(id.value),
                                        line_endpoint_signature(id, endpoint),
                                        0,
                                        false},
                {}};
    };
}
} // namespace qcae
