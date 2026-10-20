#include "qcae/planar_geometry.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

using namespace qcae;
namespace {
using Point = std::array<double, 3>;

void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

PlanarLoop rectangle(double x0, double y0, double x1, double y1, double z = 0) {
    return {{{x0, y0, z}, {x1, y0, z}, {x1, y1, z}, {x0, y1, z}}};
}
const PlanarLoop plate = rectangle(0, 0, 200, 100);

PlanarRegionShape measured(const PlanarRegion& region) {
    const auto result = measure_planar_region(region);
    if (!result.ok())
        throw std::runtime_error("A valid region was refused: " + result.error->message);
    return *result.value;
}

bool near(double value, double expected) {
    return std::abs(value - expected) <= 1e-9 * std::max(1.0, std::abs(expected));
}

void require_refused(const PlanarRegion& region,
                     const std::string& field,
                     const std::string& message,
                     const char* failure) {
    const auto result = measure_planar_region(region);
    require(!result.ok() && result.error->code == ErrorCode::invalid_input &&
                result.error->field == field && result.error->message == message,
            failure);
}

void valid_regions_are_measured() {
    const auto flat = measured({plate, {}});
    require(near(flat.area, 200 * 100) && flat.normal == Point{0, 0, 1},
            "A counter-clockwise plate must face +z");

    PlanarLoop clockwise = plate;
    std::reverse(clockwise.corners_mm.begin(), clockwise.corners_mm.end());
    require(measured({clockwise, {}}).normal == Point{0, 0, -1}, "A clockwise plate must face -z");

    const auto upright = measured({{{{0, 0, 0}, {200, 0, 0}, {200, 0, 100}, {0, 0, 100}}}, {}});
    require(near(upright.area, 200 * 100) && upright.normal == Point{0, -1, 0},
            "An upright plate was measured wrongly");

    const PlanarLoop l_shape{
        {{0, 0, 0}, {200, 0, 0}, {200, 40, 0}, {40, 40, 0}, {40, 100, 0}, {0, 100, 0}}};
    require(near(measured({l_shape, {}}).area, 200 * 40 + 40 * 60),
            "A concave outline was measured wrongly");

    const auto with_holes =
        measured({plate, {rectangle(20, 20, 40, 40), rectangle(100, 30, 130, 60)}});
    require(near(with_holes.area, 200 * 100 - 20 * 20 - 30 * 30), "Holes were not subtracted");

    const PlanarLoop straight_through{{{0, 0, 0}, {100, 0, 0}, {200, 0, 0}, {200, 100, 0}}};
    require(near(measured({straight_through, {}}).area, 200 * 100 / 2),
            "A corner on a straight edge was refused");
}

void loops_need_proper_corners() {
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    require_refused({{{{0, 0, 0}, {1, 0, 0}}}, {}},
                    "outer",
                    "A loop needs at least three corners",
                    "A two-corner loop was accepted");
    require_refused({plate, {{{{50, 50, 0}, {60, 50, 0}}}}},
                    "holes[0]",
                    "A loop needs at least three corners",
                    "A two-corner hole was accepted");
    require_refused({{{{0, 0, 0}, {200, 0, 0}, {nan, 100, 0}}}, {}},
                    "outer",
                    "Corner coordinates must be finite",
                    "A non-finite corner was accepted");
    require_refused({{{{0, 0, 0}, {200, 0, 0}, {200, 0, 0}, {0, 100, 0}}}, {}},
                    "outer",
                    "Neighbouring corners must not coincide",
                    "Coinciding neighbours were accepted");
}

void the_outer_loop_fixes_the_plane() {
    require_refused({{{{0, 0, 0}, {100, 0, 0}, {200, 0, 0}}}, {}},
                    "outer",
                    "The outer loop encloses no area",
                    "A straight outer loop was accepted");
    require_refused({{{{0, 0, 0}, {200, 0, 0}, {200, 100, 5}, {0, 100, 0}}}, {}},
                    "outer",
                    "Every corner must lie in the plane of the outer loop",
                    "A warped outer loop was accepted");
    require_refused({plate, {rectangle(50, 30, 70, 50, 1)}},
                    "holes[0]",
                    "Every corner must lie in the plane of the outer loop",
                    "A hole off the plane was accepted");
}

// These include the shapes that made Gmsh 4.15.2 hang: a crossing outline, a hole sharing an
// outline edge and two identical holes.
void edges_must_not_cross_or_touch() {
    const std::string crossing = "Edges of the region cross or touch";
    require_refused({{{{0, 0, 0}, {200, 100, 0}, {200, 0, 0}, {0, 60, 0}}}, {}},
                    "outer",
                    crossing,
                    "A crossing outline was accepted");
    require_refused({{{{0, 0, 0}, {200, 0, 0}, {200, 100, 0}, {100, 0, 0}, {0, 100, 0}}}, {}},
                    "outer",
                    crossing,
                    "An outline touching itself was accepted");
    require_refused({{{{0, 0, 0}, {200, 0, 0}, {200, 100, 0}, {200, 50, 0}, {0, 100, 0}}}, {}},
                    "outer",
                    "The loop turns back along its own edge",
                    "An outline turning back was accepted");
    require_refused({plate, {rectangle(190, 10, 220, 30)}},
                    "holes[0]",
                    crossing,
                    "A hole crossing the outline was accepted");
    require_refused({plate, {{{{0, 0, 0}, {200, 0, 0}, {100, 40, 0}}}}},
                    "holes[0]",
                    crossing,
                    "A hole sharing an outline edge was accepted");
    require_refused({plate, {{{{100, 0, 0}, {120, 20, 0}, {80, 20, 0}}}}},
                    "holes[0]",
                    crossing,
                    "A hole touching the outline was accepted");
    require_refused({plate, {rectangle(50, 20, 80, 50), rectangle(50, 20, 80, 50)}},
                    "holes[1]",
                    crossing,
                    "Identical holes were accepted");
    require_refused({plate, {rectangle(20, 20, 60, 60), rectangle(40, 40, 80, 80)}},
                    "holes[1]",
                    crossing,
                    "Crossing holes were accepted");
}

void holes_lie_inside_the_outline_and_apart() {
    require_refused({plate, {rectangle(300, 10, 320, 30)}},
                    "holes[0]",
                    "A hole lies outside the outer loop",
                    "A hole outside the outline was accepted");
    require_refused({plate, {rectangle(20, 20, 80, 80), rectangle(40, 40, 60, 60)}},
                    "holes[1]",
                    "A hole lies inside another hole",
                    "A hole inside a hole was accepted");
    require_refused({plate, {rectangle(40, 40, 60, 60), rectangle(20, 20, 80, 80)}},
                    "holes[1]",
                    "A hole lies inside another hole",
                    "A hole around a hole was accepted");
}
} // namespace

int main() {
    try {
        valid_regions_are_measured();
        loops_need_proper_corners();
        the_outer_loop_fixes_the_plane();
        edges_must_not_cross_or_touch();
        holes_lie_inside_the_outline_and_apart();
        std::cout << "PASS: planar regions: valid shapes measured; bad corners, warped planes, "
                     "crossing or touching edges and misplaced holes refused\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
