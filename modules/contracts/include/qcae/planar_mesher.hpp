#pragma once
#include "qcae/types.hpp"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace qcae {
// A closed loop of straight edges in millimetres. Corners are listed once, in order; the last
// corner connects back to the first.
struct PlanarLoop {
    std::vector<std::array<double, 3>> corners_mm;
    bool operator==(const PlanarLoop&) const = default;
};

// A flat region: one outer loop with optional holes, all in one plane. The corner order of the
// outer loop sets the region normal by the right-hand rule; the order of hole corners does not
// matter.
struct PlanarRegion {
    PlanarLoop outer;
    std::vector<PlanarLoop> holes;
    bool operator==(const PlanarRegion&) const = default;
};

struct PlanarMeshOptions {
    double element_size_mm{};
    // A request whose estimated or actual triangle count exceeds this limit is refused.
    std::uint32_t max_triangles{};
};

// The mesh along one input edge. Loop 0 is the outer loop and loop i is hole i - 1; edge j runs
// from corner j to the next corner. Each segment is a pair of node indices.
struct PlanarBoundaryEdge {
    std::uint32_t loop{};
    std::uint32_t edge{};
    std::vector<std::array<std::uint32_t, 2>> segments;
    bool operator==(const PlanarBoundaryEdge&) const = default;
};

// Triangles index into nodes_mm and turn counter-clockwise about the region normal.
struct PlanarMesh {
    std::vector<std::array<double, 3>> nodes_mm;
    std::vector<std::array<std::uint32_t, 3>> triangles;
    std::vector<PlanarBoundaryEdge> boundary;
    bool operator==(const PlanarMesh&) const = default;
};

// Meshes a flat region with three-node triangles. An implementation is deterministic: the same
// region and options always give the same mesh. identity() names the mesher and its version so
// that a stored mesh can record what produced it.
class IPlanarMesher {
  public:
    virtual ~IPlanarMesher() = default;
    virtual std::string identity() const = 0;
    virtual Result<PlanarMesh> mesh(const PlanarRegion&, const PlanarMeshOptions&) const = 0;
};
} // namespace qcae
