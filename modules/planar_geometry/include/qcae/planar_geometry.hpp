#pragma once
#include "qcae/planar_mesher.hpp"

namespace qcae {
// Lengths below this count as zero in planar region checks, in millimetres.
inline constexpr double planar_tolerance_mm = 1e-6;

struct PlanarRegionShape {
    std::array<double, 3> normal; // unit normal, by the right-hand rule along the outer loop
    double area{};                // square millimetres: the outer loop minus the holes
};

// Checks that a region is a simple polygon with holes, then measures it. In a valid region every
// loop has at least three finite corners and no two neighbouring corners coincide; all corners lie
// in the plane of the outer loop; no loop turns back along its own edge; no two edges cross or
// touch, apart from neighbouring edges at their shared corner; and every hole lies inside the outer
// loop and outside every other hole. Diagnostics name the loop at fault: "outer" or "holes[i]".
[[nodiscard]] Result<PlanarRegionShape> measure_planar_region(const PlanarRegion&);
} // namespace qcae
