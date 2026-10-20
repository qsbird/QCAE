#include "qcae/planar_geometry.hpp"
#include <cmath>
#include <string>
#include <utility>

namespace qcae {
namespace {
using Vector = std::array<double, 3>;
using Corners = std::vector<Vector>;

// A point in plane coordinates.
struct Flat {
    double x{}, y{};
};

Vector minus(const Vector& a, const Vector& b) {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}
Vector cross(const Vector& a, const Vector& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
double dot(const Vector& a, const Vector& b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
double length(const Vector& a) {
    return std::sqrt(dot(a, a));
}
Vector scaled(const Vector& a, double factor) {
    return {a[0] * factor, a[1] * factor, a[2] * factor};
}

Result<PlanarRegionShape> refused(std::string message, std::string field) {
    return {Status::failed,
            std::nullopt,
            Diagnostic{ErrorCode::invalid_input, std::move(message), std::move(field)}};
}
std::string loop_name(std::size_t loop) {
    return loop == 0 ? "outer" : "holes[" + std::to_string(loop - 1) + "]";
}

// Twice the loop's area along its right-hand normal: the sum over the triangles fanned out from
// the first corner.
Vector twice_area_vector(const Corners& corners) {
    Vector sum{};
    for (std::size_t i = 1; i + 1 < corners.size(); ++i) {
        const auto part = cross(minus(corners[i], corners[0]), minus(corners[i + 1], corners[0]));
        for (std::size_t axis = 0; axis < 3; ++axis)
            sum[axis] += part[axis];
    }
    return sum;
}

// Twice the signed area of triangle a, b, c: positive when it turns counter-clockwise.
double turn(const Flat& a, const Flat& b, const Flat& c) {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

// Whether p lies on segment a-b, within the tolerance.
bool on_segment(const Flat& a, const Flat& b, const Flat& p) {
    const double dx = b.x - a.x, dy = b.y - a.y;
    const double edge = std::hypot(dx, dy);
    if (std::abs(turn(a, b, p)) > planar_tolerance_mm * edge) // distance from the line
        return false;
    const double along = ((p.x - a.x) * dx + (p.y - a.y) * dy) / edge; // distance along it
    return along >= -planar_tolerance_mm && along <= edge + planar_tolerance_mm;
}

// Whether segments a-b and c-d share a point; touching counts.
bool segments_meet(const Flat& a, const Flat& b, const Flat& c, const Flat& d) {
    if (on_segment(a, b, c) || on_segment(a, b, d) || on_segment(c, d, a) || on_segment(c, d, b))
        return true;
    return (turn(a, b, c) > 0) != (turn(a, b, d) > 0) && (turn(c, d, a) > 0) != (turn(c, d, b) > 0);
}

// Whether p lies inside the loop, by counting edge crossings of a ray towards +x. The point must
// not lie on the loop.
bool inside(const std::vector<Flat>& loop, const Flat& p) {
    bool result = false;
    for (std::size_t i = 0, j = loop.size() - 1; i < loop.size(); j = i++) {
        const auto& a = loop[i];
        const auto& b = loop[j];
        if ((a.y > p.y) != (b.y > p.y) && p.x < a.x + (b.x - a.x) * (p.y - a.y) / (b.y - a.y))
            result = !result;
    }
    return result;
}
} // namespace

Result<PlanarRegionShape> measure_planar_region(const PlanarRegion& region) {
    std::vector<const Corners*> loops{&region.outer.corners_mm};
    for (const auto& hole : region.holes)
        loops.push_back(&hole.corners_mm);

    // Each loop on its own: enough corners, finite coordinates, no coinciding neighbours.
    for (std::size_t loop = 0; loop < loops.size(); ++loop) {
        const auto& corners = *loops[loop];
        if (corners.size() < 3)
            return refused("A loop needs at least three corners", loop_name(loop));
        for (const auto& corner : corners)
            for (const double coordinate : corner)
                if (!std::isfinite(coordinate))
                    return refused("Corner coordinates must be finite", loop_name(loop));
        for (std::size_t i = 0; i < corners.size(); ++i)
            if (length(minus(corners[(i + 1) % corners.size()], corners[i])) <= planar_tolerance_mm)
                return refused("Neighbouring corners must not coincide", loop_name(loop));
    }

    // The outer loop fixes the plane and the normal.
    const auto& outer = region.outer.corners_mm;
    const Vector twice_outer = twice_area_vector(outer);
    const double twice_outer_area = length(twice_outer);
    if (!(twice_outer_area > 0))
        return refused("The outer loop encloses no area", "outer");
    PlanarRegionShape shape{scaled(twice_outer, 1 / twice_outer_area), twice_outer_area / 2};
    for (std::size_t loop = 0; loop < loops.size(); ++loop)
        for (const auto& corner : *loops[loop])
            if (std::abs(dot(minus(corner, outer[0]), shape.normal)) > planar_tolerance_mm)
                return refused("Every corner must lie in the plane of the outer loop",
                               loop_name(loop));

    // Plane coordinates: x along the first outer edge, y = normal x that edge.
    const Vector x_axis = scaled(minus(outer[1], outer[0]), 1 / length(minus(outer[1], outer[0])));
    const Vector y_axis = cross(shape.normal, x_axis);
    std::vector<std::vector<Flat>> flat(loops.size());
    for (std::size_t loop = 0; loop < loops.size(); ++loop)
        for (const auto& corner : *loops[loop])
            flat[loop].push_back(
                {dot(minus(corner, outer[0]), x_axis), dot(minus(corner, outer[0]), y_axis)});

    // At no corner may a loop turn back along the edge it arrived on.
    for (std::size_t loop = 0; loop < flat.size(); ++loop) {
        const auto& corners = flat[loop];
        const std::size_t count = corners.size();
        for (std::size_t i = 0; i < count; ++i) {
            const auto& before = corners[(i + count - 1) % count];
            const auto& corner = corners[i];
            const auto& after = corners[(i + 1) % count];
            if (on_segment(before, corner, after) || on_segment(corner, after, before))
                return refused("The loop turns back along its own edge", loop_name(loop));
        }
    }

    // No two edges may meet, apart from neighbouring edges at their shared corner.
    struct Edge {
        std::size_t loop{}, index{};
        Flat start, end;
    };
    std::vector<Edge> edges;
    for (std::size_t loop = 0; loop < flat.size(); ++loop)
        for (std::size_t i = 0; i < flat[loop].size(); ++i)
            edges.push_back({loop, i, flat[loop][i], flat[loop][(i + 1) % flat[loop].size()]});
    for (std::size_t i = 0; i < edges.size(); ++i)
        for (std::size_t j = i + 1; j < edges.size(); ++j) {
            const auto& a = edges[i];
            const auto& b = edges[j];
            const std::size_t count = flat[a.loop].size();
            const bool neighbours = a.loop == b.loop && (b.index == a.index + 1 ||
                                                         (a.index == 0 && b.index == count - 1));
            if (!neighbours && segments_meet(a.start, a.end, b.start, b.end))
                return refused("Edges of the region cross or touch", loop_name(b.loop));
        }

    // With no edges meeting, one corner tells on which side of another loop a hole lies.
    for (std::size_t hole = 1; hole < flat.size(); ++hole) {
        if (!inside(flat[0], flat[hole][0]))
            return refused("A hole lies outside the outer loop", loop_name(hole));
        for (std::size_t other = 1; other < hole; ++other)
            if (inside(flat[other], flat[hole][0]) || inside(flat[hole], flat[other][0]))
                return refused("A hole lies inside another hole", loop_name(hole));
    }

    for (const auto& hole : region.holes)
        shape.area -= length(twice_area_vector(hole.corners_mm)) / 2;
    return success(shape);
}
} // namespace qcae
