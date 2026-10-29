// Loads and supports along one straight edge of a meshed region.
#include "qcae/analysis_features.hpp"
#include "qcae/region_geometry.hpp"
#include <algorithm>
#include <cmath>

namespace qcae::features::analysis {
using namespace operations;
namespace {
using Vector = std::array<double, 3>;

// Mesh nodes closer than this to an edge lie on it. Gmsh places boundary nodes on straight edges
// up to rounding, and interior nodes are much farther away.
constexpr double on_edge_tolerance_mm = 1e-6;

Vector minus(const Vector& a, const Vector& b) {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}
double dot(const Vector& a, const Vector& b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
double length(const Vector& a) {
    return std::sqrt(dot(a, a));
}
Vector cross(const Vector& a, const Vector& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

// A mesh node on an edge and the share of the edge it stands for: half of each neighbouring
// segment, as a fraction of the edge length. The shares of one edge add up to one.
struct EdgeNode {
    EntityId node;
    double share{};
};

// The mesh nodes on edge `edge` of loop `loop` (0 is the outer loop, i is hole i - 1) of the region
// the mesh was derived from, ordered from the edge's first corner to its second.
std::vector<EdgeNode> edge_nodes(const DocumentView& view,
                                 const EntityId& mesh_id,
                                 std::uint64_t loop,
                                 std::uint64_t edge) {
    const auto mesh_record = view.find<records::Mesh>(records::MeshId(mesh_id.value));
    if (!mesh_record)
        throw RecordError(ErrorCode::entity_not_found, "Mesh does not exist", "mesh_id");
    const auto& mesh = mesh_record->get<records::Mesh>();
    const auto region_record =
        mesh.geometry ? view.find<records::GeometryRegion>(*mesh.geometry) : nullptr;
    if (!region_record)
        throw RecordError(
            ErrorCode::invalid_input, "Edge loads need a mesh derived from a region", "mesh_id");
    if (mesh.stale)
        throw RecordError(
            ErrorCode::invalid_input, "The mesh is out of date with its region", "mesh_id");
    const auto region = records::planar_region(view, region_record->get<records::GeometryRegion>());
    const PlanarLoop* outline = loop == 0                        ? &region.outer
                                : loop - 1 < region.holes.size() ? &region.holes[loop - 1]
                                                                 : nullptr;
    if (!outline)
        throw RecordError(ErrorCode::invalid_input, "The region has no such loop", "loop");
    const auto& corners = outline->corners_mm;
    if (edge >= corners.size())
        throw RecordError(ErrorCode::invalid_input, "The loop has no such edge", "edge");
    const Vector start = corners[edge];
    const Vector direction = minus(corners[(edge + 1) % corners.size()], start);
    const double edge_length = length(direction);

    // Position along the edge of every mesh node that lies on it.
    std::vector<std::pair<double, EntityId>> on_edge;
    view.visit(RecordTraits<records::Node>::type_id, [&](const Record& record) {
        const auto& node = record->get<records::Node>();
        if (node.mesh != mesh.id)
            return;
        const Vector offset = minus(node.position, start);
        const double along = dot(offset, direction) / edge_length;
        const double distance = length(cross(offset, direction)) / edge_length;
        if (distance <= on_edge_tolerance_mm && along >= -on_edge_tolerance_mm &&
            along <= edge_length + on_edge_tolerance_mm)
            on_edge.emplace_back(along, node.id);
    });
    std::sort(on_edge.begin(), on_edge.end());
    if (on_edge.size() < 2 || on_edge.front().first > on_edge_tolerance_mm ||
        on_edge.back().first < edge_length - on_edge_tolerance_mm)
        throw RecordError(
            ErrorCode::invalid_input, "No mesh nodes cover the edge from end to end", "edge");

    std::vector<EdgeNode> result;
    for (std::size_t i = 0; i < on_edge.size(); ++i) {
        const double before = i == 0 ? 0 : on_edge[i].first - on_edge[i - 1].first;
        const double after = i + 1 == on_edge.size() ? 0 : on_edge[i + 1].first - on_edge[i].first;
        result.push_back({on_edge[i].second, (before + after) / 2 / edge_length});
    }
    return result;
}

void require_load_case(const PlanEdit& edit, const EntityId& id) {
    if (!edit.view().find<records::LoadCase>(id))
        throw RecordError(ErrorCode::entity_not_found, "Load case does not exist", "load_case_id");
}
} // namespace

// Each node carries the force times its share of the edge: the consistent nodal loads of a uniform
// edge traction on straight-sided elements.
Result<OperationPlan> prepare_add_edge_force(const LoadCaseAddEdgeForceInput& input) {
    auto canonical = canonical_copy(input);
    for (const auto& [component, name] :
         {std::pair{&canonical.x, "x"}, {&canonical.y, "y"}, {&canonical.z, "z"}})
        if (auto normalized = normalize(*component, parameters::Dimension::force, name);
            !normalized.ok())
            return forward_failure<OperationPlan>(normalized);
    if (canonical.x.value == 0 && canonical.y.value == 0 && canonical.z.value == 0)
        return failed<OperationPlan>(
            RecordError(ErrorCode::invalid_input, "The edge force must not be zero", "x"));
    return record_plan(
        std::move(canonical), [](const LoadCaseAddEdgeForceInput& value, PlanEdit& edit) {
            require_load_case(edit, value.load_case_id);
            std::vector<EntityId> forces;
            for (const auto& node :
                 edge_nodes(edit.view(), value.mesh_id, value.loop, value.edge)) {
                const EntityId id = edit.allocate();
                edit.put(records::NodalForce{id,
                                             node.node,
                                             {value.x.value * node.share,
                                              value.y.value * node.share,
                                              value.z.value * node.share}});
                forces.push_back(id);
            }
            edit.update<records::LoadCase>(value.load_case_id, [&](auto& load_case) {
                load_case.forces.insert(load_case.forces.end(), forces.begin(), forces.end());
            });
            return ChangeSummary{"Add edge force", value.load_case_id, 0, false};
        });
}

// One constraint over all mesh nodes of the edge.
Result<OperationPlan> prepare_add_edge_constraint(const LoadCaseAddEdgeConstraintInput& input) {
    return record_plan(
        canonical_copy(input), [](const LoadCaseAddEdgeConstraintInput& value, PlanEdit& edit) {
            require_load_case(edit, value.load_case_id);
            records::Constraint constraint{edit.allocate(), {}, value.dofs};
            for (const auto& node : edge_nodes(edit.view(), value.mesh_id, value.loop, value.edge))
                constraint.nodes.push_back(node.node);
            const EntityId id = constraint.id;
            edit.put(std::move(constraint));
            edit.update<records::LoadCase>(
                value.load_case_id, [&](auto& load_case) { load_case.constraints.push_back(id); });
            return ChangeSummary{"Add edge constraint", value.load_case_id, 0, false};
        });
}
} // namespace qcae::features::analysis
