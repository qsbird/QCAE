#include "qcae/gmsh_planar_mesher.hpp"
#include <algorithm>
#include <clocale>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace qcae;
namespace {
using Point = std::array<double, 3>;
constexpr PlanarMeshOptions coarse{10, 50000};

void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

Point minus(const Point& a, const Point& b) {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}
Point cross(const Point& a, const Point& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
double dot(const Point& a, const Point& b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
double length(const Point& a) {
    return std::sqrt(dot(a, a));
}

PlanarLoop rectangle(double x0, double y0, double x1, double y1) {
    return {{{x0, y0, 0}, {x1, y0, 0}, {x1, y1, 0}, {x0, y1, 0}}};
}
const PlanarLoop plate_outline = rectangle(0, 0, 200, 100);

// A regular polygon standing in for a round hole.
PlanarLoop polygon(Point centre, double radius, int sides) {
    PlanarLoop loop;
    for (int i = 0; i < sides; ++i) {
        const double angle = 2 * std::numbers::pi * i / sides;
        loop.corners_mm.push_back({centre[0] + radius * std::cos(angle),
                                   centre[1] + radius * std::sin(angle),
                                   centre[2]});
    }
    return loop;
}
double polygon_area(double radius, int sides) {
    return sides * radius * radius * std::sin(2 * std::numbers::pi / sides) / 2;
}
const PlanarRegion plate_with_hole{plate_outline, {polygon({100, 50, 0}, 20, 32)}};

PlanarMesh meshed(const GmshPlanarMesher& mesher,
                  const PlanarRegion& region,
                  const PlanarMeshOptions& options = coarse) {
    const auto result = mesher.mesh(region, options);
    if (!result.ok())
        throw std::runtime_error("Meshing failed: " + result.error->message);
    return *result.value;
}

void require_refused(const Result<PlanarMesh>& result,
                     ErrorCode code,
                     const std::string& field,
                     const char* message) {
    require(!result.ok() && result.error && result.error->code == code &&
                result.error->field == field,
            message);
}

// What every mesh must satisfy: triangles turn counter-clockwise about the normal and cover the
// expected area, every node lies in the plane and belongs to a triangle, and the segments of every
// boundary edge lie on that edge and add up to its length.
void require_sound(const PlanarRegion& region, const PlanarMesh& mesh, Point normal, double area) {
    require(!mesh.triangles.empty(), "The mesh has no triangles");
    std::vector<bool> used(mesh.nodes_mm.size());
    double covered = 0;
    for (const auto& triangle : mesh.triangles) {
        const auto& a = mesh.nodes_mm[triangle[0]];
        const double doubled =
            dot(cross(minus(mesh.nodes_mm[triangle[1]], a), minus(mesh.nodes_mm[triangle[2]], a)),
                normal);
        require(doubled > 0, "A triangle is not counter-clockwise about the region normal");
        covered += doubled / 2;
        for (const auto node : triangle)
            used[node] = true;
    }
    require(std::abs(covered - area) <= 1e-9 * area, "The triangles do not cover the region");
    require(std::all_of(used.begin(), used.end(), [](bool value) { return value; }),
            "A node belongs to no triangle");
    const auto& origin = region.outer.corners_mm.front();
    for (const auto& node : mesh.nodes_mm)
        require(std::abs(dot(minus(node, origin), normal)) < 1e-9, "A node lies off the plane");

    std::vector<const PlanarLoop*> loops{&region.outer};
    std::size_t edges = region.outer.corners_mm.size();
    for (const auto& hole : region.holes) {
        loops.push_back(&hole);
        edges += hole.corners_mm.size();
    }
    require(mesh.boundary.size() == edges, "The mesh does not report every boundary edge");
    for (const auto& edge : mesh.boundary) {
        const auto& corners = loops.at(edge.loop)->corners_mm;
        const Point start = corners.at(edge.edge);
        const Point direction = minus(corners[(edge.edge + 1) % corners.size()], start);
        double total = 0;
        for (const auto& segment : edge.segments) {
            for (const auto node : segment)
                require(length(cross(minus(mesh.nodes_mm[node], start), direction)) /
                                length(direction) <
                            1e-9,
                        "A boundary node lies off its edge");
            total += length(minus(mesh.nodes_mm[segment[1]], mesh.nodes_mm[segment[0]]));
        }
        require(std::abs(total - length(direction)) <= 1e-9 * length(direction),
                "The boundary segments do not cover their edge");
    }
}

void a_rectangle_is_covered_by_counter_clockwise_triangles(const GmshPlanarMesher& mesher) {
    const PlanarRegion plate{plate_outline, {}};
    require_sound(plate, meshed(mesher, plate), {0, 0, 1}, 200 * 100);
}

void a_hole_is_left_out_of_the_mesh(const GmshPlanarMesher& mesher) {
    const auto mesh = meshed(mesher, plate_with_hole, {3, 50000});
    require_sound(plate_with_hole, mesh, {0, 0, 1}, 200 * 100 - polygon_area(20, 32));
    // No triangle centre lies inside the circle inscribed in the hole polygon.
    const double inscribed = 20 * std::cos(std::numbers::pi / 32);
    for (const auto& triangle : mesh.triangles) {
        Point centre{};
        for (const auto node : triangle)
            for (std::size_t axis = 0; axis < 3; ++axis)
                centre[axis] += mesh.nodes_mm[node][axis] / 3;
        require(length(minus(centre, {100, 50, 0})) > inscribed, "A triangle lies in the hole");
    }
}

void the_same_input_gives_the_same_mesh(const GmshPlanarMesher& mesher) {
    require(meshed(mesher, plate_with_hole) == meshed(mesher, plate_with_hole),
            "Meshing the same region twice gave different meshes");
}

void the_outer_loop_sets_the_normal(const GmshPlanarMesher& mesher) {
    PlanarLoop clockwise = plate_outline;
    std::reverse(clockwise.corners_mm.begin(), clockwise.corners_mm.end());
    const PlanarRegion facing_down{clockwise, {}};
    require_sound(facing_down, meshed(mesher, facing_down), {0, 0, -1}, 200 * 100);
    const PlanarRegion upright{{{{0, 0, 0}, {200, 0, 0}, {200, 0, 100}, {0, 0, 100}}}, {}};
    require_sound(upright, meshed(mesher, upright), {0, -1, 0}, 200 * 100);
}

void invalid_requests_are_refused_before_meshing(const GmshPlanarMesher& mesher) {
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    const auto invalid = ErrorCode::invalid_input;
    const PlanarRegion plate{plate_outline, {}};
    require_refused(mesher.mesh(plate, {0, 50000}),
                    invalid,
                    "element_size_mm",
                    "A zero element size was accepted");
    require_refused(mesher.mesh(plate, {nan, 50000}),
                    invalid,
                    "element_size_mm",
                    "A non-finite element size was accepted");
    require_refused(
        mesher.mesh(plate, {10, 0}), invalid, "max_triangles", "A zero limit was accepted");
    // Region checks are measure_planar_region's (planar_geometry_tests); the mesher applies them.
    require_refused(mesher.mesh({{{{0, 0, 0}, {1, 0, 0}}}, {}}, coarse),
                    invalid,
                    "outer",
                    "A two-corner loop was accepted");
}

// Gmsh 4.15.2 hangs on these regions, so they must be refused before Gmsh sees them.
void regions_that_hang_gmsh_never_reach_it(const GmshPlanarMesher& mesher) {
    const PlanarRegion plate{plate_outline, {}};
    const auto before = meshed(mesher, plate);
    const auto invalid = ErrorCode::invalid_input;
    require_refused(
        mesher.mesh({{{{0, 0, 0}, {200, 100, 0}, {200, 0, 0}, {0, 60, 0}}}, {}}, coarse),
        invalid,
        "outer",
        "A crossing outline was meshed");
    require_refused(
        mesher.mesh({plate_outline, {{{{0, 0, 0}, {200, 0, 0}, {100, 40, 0}}}}}, coarse),
        invalid,
        "holes[0]",
        "A hole sharing an outline edge was meshed");
    require_refused(
        mesher.mesh({plate_outline, {rectangle(50, 20, 80, 50), rectangle(50, 20, 80, 50)}},
                    coarse),
        invalid,
        "holes[1]",
        "Identical holes were meshed");
    require(meshed(mesher, plate) == before, "A refused region changed later meshes");
}

void oversized_requests_are_refused(const GmshPlanarMesher& mesher) {
    const PlanarRegion plate{plate_outline, {}};
    require_refused(mesher.mesh(plate, {0.1, 50000}),
                    ErrorCode::resource_limit,
                    "element_size_mm",
                    "A request far over the limit was meshed");
    // A limit below the actual count but above the estimate: only the check after meshing sees it.
    const auto actual = static_cast<std::uint32_t>(meshed(mesher, plate).triangles.size());
    require(200 * 100 / (10 * 10 * std::sqrt(3.0) / 4) < actual - 1,
            "Test setup: the estimate must stay under the limit");
    require_refused(mesher.mesh(plate, {10, actual - 1}),
                    ErrorCode::resource_limit,
                    "element_size_mm",
                    "A mesh over the limit was returned");
}

void concurrent_calls_get_the_same_mesh(const GmshPlanarMesher& mesher) {
    const auto expected = meshed(mesher, plate_with_hole);
    std::vector<std::optional<PlanarMesh>> results(4);
    std::vector<std::thread> threads;
    for (auto& result : results)
        threads.emplace_back([&mesher, &result] {
            auto meshed = mesher.mesh(plate_with_hole, coarse);
            if (meshed.ok())
                result = std::move(*meshed.value);
        });
    for (auto& thread : threads)
        thread.join();
    for (const auto& result : results)
        require(result && *result == expected, "A concurrent call returned a different mesh");
}

std::optional<std::string> environment_value(const char* name) {
    const char* value = std::getenv(name);
    return value ? std::optional<std::string>(value) : std::nullopt;
}
} // namespace

int main() {
    try {
        const auto path = environment_value("PATH");
        const auto python_path = environment_value("PYTHONPATH");
        const std::string locale = std::setlocale(LC_ALL, nullptr);
        const GmshPlanarMesher mesher; // starts Gmsh
        require(environment_value("PATH") == path && environment_value("PYTHONPATH") == python_path,
                "Starting Gmsh changed the environment");
        require(locale == std::setlocale(LC_ALL, nullptr), "Starting Gmsh changed the locale");
        require(mesher.identity().starts_with("gmsh "), "The identity does not name Gmsh");

        a_rectangle_is_covered_by_counter_clockwise_triangles(mesher);
        a_hole_is_left_out_of_the_mesh(mesher);
        the_same_input_gives_the_same_mesh(mesher);
        the_outer_loop_sets_the_normal(mesher);
        invalid_requests_are_refused_before_meshing(mesher);
        regions_that_hang_gmsh_never_reach_it(mesher);
        oversized_requests_are_refused(mesher);
        concurrent_calls_get_the_same_mesh(mesher);
        std::cout << "PASS: " << mesher.identity()
                  << " planar meshes: coverage, orientation, holes, determinism, refusals, "
                     "limits, concurrency and unchanged environment\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
