#include "qcae/gmsh_planar_mesher.hpp"
#include "qcae/planar_geometry.hpp"
#include <gmsh.h>
#include <clocale>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

namespace qcae {
namespace {
using Vector = std::array<double, 3>;

// The mesh area may differ from the region area only by rounding.
constexpr double area_tolerance = 1e-9;
// Gmsh element type numbers.
constexpr int gmsh_line2 = 1;
constexpr int gmsh_triangle3 = 2;

Vector minus(const Vector& a, const Vector& b) {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}
Vector cross(const Vector& a, const Vector& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
double dot(const Vector& a, const Vector& b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

Result<PlanarMesh> refused(ErrorCode code, std::string message, std::string field) {
    return {Status::failed, std::nullopt, Diagnostic{code, std::move(message), std::move(field)}};
}

std::optional<Diagnostic> invalid_options(const PlanarMeshOptions& options) {
    if (!std::isfinite(options.element_size_mm) || options.element_size_mm <= 0)
        return Diagnostic{ErrorCode::invalid_input,
                          "The element size must be a positive number of millimetres",
                          "element_size_mm"};
    if (options.max_triangles == 0)
        return Diagnostic{
            ErrorCode::invalid_input, "The triangle limit must be positive", "max_triangles"};
    return std::nullopt;
}

// Gmsh state is process-wide; every call into Gmsh holds this lock.
std::mutex gmsh_mutex;

std::optional<std::string> environment_value(const char* name) {
    const char* value = std::getenv(name);
    return value ? std::optional<std::string>(value) : std::nullopt;
}
void restore_environment_value(const char* name, const std::optional<std::string>& value) {
    if (value)
        ::setenv(name, value->c_str(), 1);
    else
        ::unsetenv(name);
}

// Starts Gmsh once per process, without user configuration files or terminal output. On start-up
// Gmsh adds its executable's directory to PATH and PYTHONPATH and switches the locale to C.UTF-8.
// Gmsh 4.15.2 documents -noenv and -nolocale against this, but its option parser rejects both and
// exits the process, so the changes are undone here instead.
void start_gmsh_once() {
    if (gmsh::isInitialized())
        return;
    const auto path = environment_value("PATH");
    const auto python_path = environment_value("PYTHONPATH");
    const std::string locale = std::setlocale(LC_ALL, nullptr);
    gmsh::initialize(0, nullptr, /*readConfigFiles=*/false, /*run=*/false);
    restore_environment_value("PATH", path);
    restore_environment_value("PYTHONPATH", python_path);
    std::setlocale(LC_ALL, locale.c_str());
    gmsh::option::setNumber("General.Terminal", 0);
}

// Adds a model and removes it on every exit path: after a failure Gmsh would otherwise keep the
// failed model loaded and current.
class ScopedModel {
  public:
    ScopedModel() {
        gmsh::model::add("qcae-planar-region");
    }
    ~ScopedModel() {
        try {
            gmsh::model::remove();
        } catch (...) {
            // A destructor must not throw; the next call adds a fresh model either way.
        }
    }
    ScopedModel(const ScopedModel&) = delete;
    ScopedModel& operator=(const ScopedModel&) = delete;
};

struct LoopTags {
    int curve_loop{};
    std::vector<int> edges; // one Gmsh line per edge, in corner order
};

// Adds a loop's corners and straight edges to Gmsh's built-in geometry kernel. Every corner
// carries the target element size.
LoopTags add_loop(const PlanarLoop& loop, double element_size) {
    std::vector<int> points;
    for (const auto& corner : loop.corners_mm)
        points.push_back(gmsh::model::geo::addPoint(corner[0], corner[1], corner[2], element_size));
    LoopTags tags;
    for (std::size_t i = 0; i < points.size(); ++i)
        tags.edges.push_back(gmsh::model::geo::addLine(points[i], points[(i + 1) % points.size()]));
    tags.curve_loop = gmsh::model::geo::addCurveLoop(tags.edges);
    return tags;
}

// The elements of one type on one Gmsh entity, as node indices.
template <std::size_t Nodes>
std::vector<std::array<std::uint32_t, Nodes>>
elements(int dimension, int tag, int type, const std::map<std::size_t, std::uint32_t>& index_of) {
    std::vector<int> types;
    std::vector<std::vector<std::size_t>> element_tags, node_tags;
    gmsh::model::mesh::getElements(types, element_tags, node_tags, dimension, tag);
    std::vector<std::array<std::uint32_t, Nodes>> result;
    for (std::size_t i = 0; i < types.size(); ++i) {
        if (types[i] != type)
            throw std::runtime_error("Gmsh produced an unexpected element type");
        for (std::size_t first = 0; first + Nodes <= node_tags[i].size(); first += Nodes) {
            std::array<std::uint32_t, Nodes> element{};
            for (std::size_t j = 0; j < Nodes; ++j)
                element[j] = index_of.at(node_tags[i][first + j]);
            result.push_back(element);
        }
    }
    return result;
}

// Builds the region in Gmsh, meshes it and copies the mesh out. Gmsh reports errors by throwing.
// The caller holds gmsh_mutex.
PlanarMesh mesh_with_gmsh(const PlanarRegion& region, double element_size) {
    ScopedModel model;
    gmsh::option::setNumber("Mesh.Algorithm", 6); // Frontal-Delaunay, Gmsh's default in 2D
    std::vector<LoopTags> loops{add_loop(region.outer, element_size)};
    for (const auto& hole : region.holes)
        loops.push_back(add_loop(hole, element_size));
    std::vector<int> curve_loops;
    for (const auto& loop : loops)
        curve_loops.push_back(loop.curve_loop);
    const int surface = gmsh::model::geo::addPlaneSurface(curve_loops);
    gmsh::model::geo::synchronize();
    gmsh::model::mesh::generate(2);

    // Only the nodes of the surface and its boundary: Gmsh gives every geometry point a node.
    std::vector<std::size_t> node_tags;
    std::vector<double> coordinates, parametric;
    gmsh::model::mesh::getNodes(node_tags, coordinates, parametric, 2, surface, true, false);
    PlanarMesh mesh;
    std::map<std::size_t, std::uint32_t> index_of;
    for (std::size_t i = 0; i < node_tags.size(); ++i) {
        index_of.emplace(node_tags[i], static_cast<std::uint32_t>(i));
        mesh.nodes_mm.push_back(
            {coordinates[3 * i], coordinates[3 * i + 1], coordinates[3 * i + 2]});
    }
    mesh.triangles = elements<3>(2, surface, gmsh_triangle3, index_of);
    for (std::uint32_t loop = 0; loop < loops.size(); ++loop)
        for (std::uint32_t edge = 0; edge < loops[loop].edges.size(); ++edge)
            mesh.boundary.push_back(
                {loop, edge, elements<2>(1, loops[loop].edges[edge], gmsh_line2, index_of)});
    return mesh;
}

// Gmsh turns all triangles of a surface the same way. Turns them counter-clockwise about the
// region normal, then checks that they cover exactly the region, which guards the contract
// against Gmsh misbehaving on a valid region.
std::optional<Diagnostic> orient_and_check(PlanarMesh& mesh, const PlanarRegionShape& shape) {
    auto twice_area = [&](const std::array<std::uint32_t, 3>& triangle) {
        const auto& a = mesh.nodes_mm[triangle[0]];
        return dot(
            cross(minus(mesh.nodes_mm[triangle[1]], a), minus(mesh.nodes_mm[triangle[2]], a)),
            shape.normal);
    };
    if (!mesh.triangles.empty() && twice_area(mesh.triangles.front()) < 0)
        for (auto& triangle : mesh.triangles)
            std::swap(triangle[1], triangle[2]);
    double area = 0;
    for (const auto& triangle : mesh.triangles) {
        const double doubled = twice_area(triangle);
        if (!(doubled > 0))
            return Diagnostic{ErrorCode::invalid_input,
                              "Gmsh produced a degenerate or inverted triangle",
                              "outer"};
        area += doubled / 2;
    }
    if (std::abs(area - shape.area) > area_tolerance * shape.area)
        return Diagnostic{
            ErrorCode::invalid_input, "Gmsh's triangles do not cover the region", "outer"};
    return std::nullopt;
}
} // namespace

GmshPlanarMesher::GmshPlanarMesher() {
    const std::lock_guard lock(gmsh_mutex);
    start_gmsh_once();
}

std::string GmshPlanarMesher::identity() const {
    const std::lock_guard lock(gmsh_mutex);
    std::string version;
    gmsh::option::getString("General.Version", version);
    return "gmsh " + version;
}

Result<PlanarMesh> GmshPlanarMesher::mesh(const PlanarRegion& region,
                                          const PlanarMeshOptions& options) const {
    if (const auto problem = invalid_options(options))
        return {Status::failed, std::nullopt, *problem};
    // Gmsh can hang on regions whose edges cross or touch, so only valid regions reach it.
    const auto shape = measure_planar_region(region);
    if (!shape.ok())
        return forward_failure<PlanarMesh>(shape);

    // A running Gmsh mesh cannot be interrupted, so refuse oversized requests before starting.
    // The estimate assumes edges no shorter than the element size: an equilateral triangle with
    // edge h has area h * h * sqrt(3) / 4. Shorter edges refine the mesh around them beyond the
    // estimate; the count is checked again after meshing.
    const double size = options.element_size_mm;
    const double estimate = shape.value->area / (size * size * std::sqrt(3.0) / 4);
    const std::string limit = std::to_string(options.max_triangles);
    if (estimate > options.max_triangles)
        return refused(ErrorCode::resource_limit,
                       "About " + std::to_string(std::llround(estimate)) +
                           " triangles would exceed the limit of " + limit +
                           "; use a larger element size",
                       "element_size_mm");
    PlanarMesh mesh;
    try {
        const std::lock_guard lock(gmsh_mutex);
        mesh = mesh_with_gmsh(region, size);
    } catch (const std::exception& error) {
        return refused(ErrorCode::invalid_input,
                       std::string("Gmsh could not mesh the region: ") + error.what(),
                       "outer");
    }
    if (mesh.triangles.size() > options.max_triangles)
        return refused(ErrorCode::resource_limit,
                       std::to_string(mesh.triangles.size()) + " triangles exceed the limit of " +
                           limit + "; use a larger element size",
                       "element_size_mm");
    if (const auto problem = orient_and_check(mesh, *shape.value))
        return {Status::failed, std::nullopt, *problem};
    return success(std::move(mesh));
}
} // namespace qcae
