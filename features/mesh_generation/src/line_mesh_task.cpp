#include "qcae/line_mesh_task.hpp"
#include <cmath>
#include <algorithm>
#include <map>
#include <set>

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
namespace {
struct UniformChain {
    std::vector<records::Node> nodes;
    std::vector<records::Beam> beams;
};
bool same_position(const std::array<double, 3>& a, const std::array<double, 3>& b) {
    for (std::size_t axis = 0; axis < 3; ++axis)
        if (std::abs(a[axis] - b[axis]) > 1e-9)
            return false;
    return true;
}
UniformChain uniform_chain(const DocumentView& view,
                           const records::Mesh& mesh,
                           const records::GeometryLine& line) {
    std::map<EntityId, records::Node> nodes;
    std::map<EntityId, std::vector<records::Beam>> edges;
    std::size_t count{};
    view.visit(RecordTraits<records::Node>::type_id, [&](const Record& record) {
        const auto& node = record->get<records::Node>();
        if (node.mesh == mesh.id)
            nodes.emplace(node.id, node);
    });
    view.visit(RecordTraits<records::Beam>::type_id, [&](const Record& record) {
        const auto& beam = record->get<records::Beam>();
        if (beam.mesh == mesh.id) {
            if (!nodes.contains(beam.nodes[0]) || !nodes.contains(beam.nodes[1]))
                throw RecordError(ErrorCode::invalid_input, "Mesh beam crosses its node domain");
            edges[beam.nodes[0]].push_back(beam);
            edges[beam.nodes[1]].push_back(beam);
            ++count;
        }
    });
    if (!count || nodes.size() != count + 1)
        throw RecordError(ErrorCode::invalid_input,
                          "Regeneration requires one nonempty line chain");
    std::optional<EntityId> start;
    for (const auto& [id, node] : nodes) {
        const auto degree = edges[id].size();
        if (degree != 1 && degree != 2)
            throw RecordError(ErrorCode::invalid_input,
                              "Regeneration rejects branched or isolated nodes");
        if (degree == 1 && same_position(node.position, line.start)) {
            if (start)
                throw RecordError(ErrorCode::invalid_input, "Mesh start is ambiguous");
            start = id;
        }
    }
    if (!start)
        throw RecordError(ErrorCode::invalid_input, "Mesh start no longer matches its geometry");
    UniformChain result;
    std::set<EntityId> visited;
    auto current = *start;
    for (;;) {
        result.nodes.push_back(nodes.at(current));
        const records::Beam* next{};
        for (const auto& beam : edges.at(current))
            if (!visited.contains(beam.id)) {
                if (next)
                    throw RecordError(ErrorCode::invalid_input, "Mesh traversal is ambiguous");
                next = &beam;
            }
        if (!next)
            break;
        visited.insert(next->id);
        result.beams.push_back(*next);
        current = next->nodes[0] == current ? next->nodes[1] : next->nodes[0];
        if (result.nodes.size() > count)
            throw RecordError(ErrorCode::invalid_input, "Mesh contains a cycle");
    }
    if (visited.size() != count || result.nodes.size() != nodes.size())
        throw RecordError(ErrorCode::invalid_input, "Mesh contains disconnected chains");
    const auto& end = result.nodes.back().position;
    for (std::size_t index = 0; index < result.nodes.size(); ++index) {
        std::array<double, 3> expected{};
        for (std::size_t axis = 0; axis < 3; ++axis)
            expected[axis] =
                line.start[axis] + (end[axis] - line.start[axis]) * double(index) / double(count);
        if (!same_position(result.nodes[index].position, expected))
            throw RecordError(ErrorCode::invalid_input, "Mesh is no longer uniformly spaced");
    }
    return result;
}
} // namespace
TaskRequest regenerate_line_mesh_task(const RecordSnapshot& snapshot,
                                      Caller caller,
                                      records::MeshId mesh,
                                      std::uint32_t segments,
                                      std::string replacement_policy,
                                      std::string key) {
    if (!segments || segments > 100000)
        throw RecordError(ErrorCode::resource_limit, "Line segments must be between 1 and 100000");
    if (replacement_policy != "reject_unmapped")
        throw RecordError(ErrorCode::invalid_input,
                          "Only reject_unmapped replacement is supported",
                          "replacement_policy");
    const auto signature = record_wire::strings(std::array<std::string, 4>{
        "mesh.line.regenerate.v1", mesh.value, record_wire::number(segments), replacement_policy});
    TaskRequest request{std::move(caller),
                        std::move(key),
                        "mesh.regenerate_line",
                        signature,
                        {snapshot.info.document, snapshot.info.revision, {}},
                        {}};
    request.work = [base = snapshot.records, mesh, segments, signature](
                       const TaskControl& control) {
        control.checkpoint();
        const auto image = base.find<records::Mesh>(mesh);
        if (!image || !image->get<records::Mesh>().geometry ||
            image->get<records::Mesh>().origin != "geometry")
            throw RecordError(
                ErrorCode::invalid_input, "Mesh is not a generated line mesh", "mesh_id");
        const auto previous = image->get<records::Mesh>();
        const auto geometry = base.find<records::GeometryLine>(*previous.geometry);
        if (!geometry)
            throw RecordError(ErrorCode::invalid_input, "Mesh geometry is missing");
        const auto& line = geometry->get<records::GeometryLine>();
        const auto chain = uniform_chain(base, previous, line);
        const bool retain = chain.beams.size() == segments;
        // A changed segment count cannot infer a mapping for external references.
        // EditSession's complete reference validation rejects the whole candidate.
        EditSession edit(base);
        if (!retain) {
            for (const auto& beam : chain.beams)
                edit.erase({RecordTraits<records::Beam>::type_id, beam.id.value});
            for (const auto& node : chain.nodes)
                edit.erase({RecordTraits<records::Node>::type_id, node.id.value});
        }
        edit.update<records::Mesh>(mesh, [&](auto& value) {
            value.geometry_revision = line.geometry_revision;
            value.stale = false;
        });
        std::vector<EntityId> identities;
        identities.reserve(std::size_t(segments) + 1);
        for (std::uint32_t index = 0; index <= segments; ++index) {
            control.checkpoint();
            std::array<double, 3> position{};
            for (std::size_t axis = 0; axis < 3; ++axis)
                position[axis] = index == segments
                                     ? line.end[axis]
                                     : line.start[axis] + (line.end[axis] - line.start[axis]) *
                                                              double(index) / segments;
            const auto id =
                retain ? chain.nodes[index].id
                       : EntityId("node-" + control.task_id() + '-' + std::to_string(index));
            identities.push_back(id);
            if (retain)
                edit.update<records::Node>(id, [&](auto& node) { node.position = position; });
            else
                edit.put(records::Node{id, position, mesh});
        }
        const auto length = std::hypot(
            line.end[0] - line.start[0], line.end[1] - line.start[1], line.end[2] - line.start[2]);
        const auto orientation = std::abs((line.end[2] - line.start[2]) / length) < .9
                                     ? std::array<double, 3>{0, 0, 1}
                                     : std::array<double, 3>{0, 1, 0};
        std::optional<EntityId> common_section = chain.beams.front().section;
        if (!retain && std::any_of(chain.beams.begin(), chain.beams.end(), [&](const auto& beam) {
                return beam.section != common_section;
            }))
            throw RecordError(ErrorCode::invalid_input,
                              "Changed segmentation cannot infer multiple section assignments");
        for (std::uint32_t index = 0; index < segments; ++index) {
            control.checkpoint();
            if (retain) {
                auto beam = chain.beams[index];
                beam.orientation = orientation;
                edit.put(std::move(beam));
            } else
                edit.put(records::Beam{
                    EntityId("line-" + control.task_id() + '-' + std::to_string(index)),
                    common_section,
                    {identities[index], identities[index + 1]},
                    orientation,
                    mesh});
        }
        control.progress(.95);
        auto prepared = edit.prepare();
        if (prepared.changes.empty())
            throw RecordError(ErrorCode::invalid_input, "Mesh regeneration has no changes");
        control.checkpoint();
        return std::make_shared<const RecordTaskPayload>(
            RecordPreparedOperation{std::move(prepared),
                                    "Regenerate uniform line mesh",
                                    EntityId(mesh.value),
                                    signature,
                                    0,
                                    false});
    };
    return request;
}
} // namespace qcae
