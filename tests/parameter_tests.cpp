#include "qcae/quantities.hpp"
#include <cmath>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string>

using namespace qcae;
using namespace qcae::parameters;

namespace {
void check(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <class T> const T& good(const Result<T>& result, const std::string& message) {
    check(result.ok(), message + (result.error ? ": " + result.error->message : ""));
    check(!result.error, message + ": unexpected diagnostic on success");
    return *result.value;
}

template <class T> void bad(const Result<T>& result, ErrorCode code, const std::string& message) {
    const auto status = code == ErrorCode::missing_input ? Status::needs_input : Status::failed;
    check(!result.ok() && !result.value && result.status == status && result.error &&
              result.error->code == code,
          message);
}

void near(double actual, double expected, const std::string& message, double tolerance = 1e-12) {
    check(std::isfinite(actual) && std::abs(actual - expected) <= tolerance, message);
}

void vector_near(const Vector3& actual, const Vector3& expected, const std::string& message) {
    for (std::size_t axis = 0; axis < 3; ++axis) {
        near(actual[axis], expected[axis], message + " axis " + std::to_string(axis));
    }
}

void unit_conversion() {
    const auto length = good(canonical_quantity({1, "m"}, Dimension::length), "BP-09 length");
    check(length.unit == "mm", "length canonical unit");
    near(length.value, 1000, "BP-09 1m equals 1000mm");
    const auto pressure =
        good(canonical_quantity({210, "GPa"}, Dimension::pressure), "BP-09 pressure");
    check(pressure.unit == "MPa", "pressure canonical unit");
    near(pressure.value, 210000, "BP-09 210GPa equals 210000MPa");

    near(good(convert_quantity({2500, "mm"}, Dimension::length, "m"), "inverse length").value,
         2.5,
         "millimeters to meters");
    near(good(canonical_quantity({2.5, "kN"}, Dimension::force), "force").value, 2500, "kN to N");
    near(good(convert_quantity({2500, "N"}, Dimension::force, "kN"), "inverse force").value,
         2.5,
         "N to kN");
    near(good(canonical_quantity({1e6, "Pa"}, Dimension::pressure), "Pa").value, 1, "Pa to MPa");
    near(
        good(canonical_quantity({1000, "kPa"}, Dimension::pressure), "kPa").value, 1, "kPa to MPa");
    near(good(convert_quantity({1, "MPa"}, Dimension::pressure, "Pa"), "inverse Pa").value,
         1e6,
         "MPa to Pa",
         1e-9);
    near(good(canonical_quantity({2, "m2"}, Dimension::area), "area").value, 2e6, "m2 to mm2");
    near(good(convert_quantity({2e6, "mm2"}, Dimension::area, "m2"), "inverse area").value,
         2,
         "mm2 to m2");
    near(good(canonical_quantity({2, "m4"}, Dimension::inertia), "inertia").value,
         2e12,
         "m4 to mm4");
    near(good(convert_quantity({2e12, "mm4"}, Dimension::inertia, "m4"), "inverse inertia").value,
         2,
         "mm4 to m4");
    near(good(canonical_quantity({180, "deg"}, Dimension::angle), "degrees").value,
         std::numbers::pi,
         "degrees to radians");
    near(
        good(convert_quantity({std::numbers::pi, "rad"}, Dimension::angle, "deg"), "radians").value,
        180,
        "radians to degrees");
    near(good(canonical_quantity({-2, "kN"}, Dimension::force), "negative force").value,
         -2000,
         "signed quantities remain signed");
    near(good(canonical_quantity({0, "Pa"}, Dimension::pressure), "zero pressure").value,
         0,
         "zero is not underflow");

    check(unit_registry().size() == 14, "fixed read-only unit metadata catalog");
    for (const auto& descriptor : unit_registry()) {
        check(descriptor.to_canonical > 0 && std::isfinite(descriptor.to_canonical),
              "catalog scales must be finite and positive");
        check(descriptor.canonical_symbol == canonical_unit(descriptor.dimension),
              "catalog canonical metadata agrees with conversions");
        const auto canonical =
            good(canonical_quantity({1, std::string(descriptor.symbol)}, descriptor.dimension),
                 "registry unit is convertible");
        near(canonical.value, descriptor.to_canonical, "registry scale matches conversion");
    }
}

void invalid_units_and_numeric_ranges() {
    bad(canonical_quantity({1, ""}, Dimension::length),
        ErrorCode::missing_input,
        "missing source unit");
    bad(convert_quantity({1, "mm"}, Dimension::length, ""),
        ErrorCode::missing_input,
        "missing target unit");
    bad(canonical_quantity({1, "MM"}, Dimension::length),
        ErrorCode::invalid_unit,
        "case-sensitive unknown unit");
    bad(canonical_quantity({1, "N"}, Dimension::length),
        ErrorCode::invalid_unit,
        "wrong source dimension");
    bad(convert_quantity({1, "mm"}, Dimension::length, "N"),
        ErrorCode::invalid_unit,
        "wrong target dimension");
    bad(convert_quantity({1, "mm"}, Dimension::length, "inch"),
        ErrorCode::invalid_unit,
        "unknown target unit");
    bad(canonical_quantity({1, "mm"}, static_cast<Dimension>(999)),
        ErrorCode::invalid_unit,
        "unknown dimension");
    for (const auto value : {std::numeric_limits<double>::infinity(),
                             -std::numeric_limits<double>::infinity(),
                             std::numeric_limits<double>::quiet_NaN()}) {
        bad(canonical_quantity({value, "mm"}, Dimension::length),
            ErrorCode::invalid_input,
            "nonfinite source rejected");
    }
    bad(canonical_quantity({std::numeric_limits<double>::max(), "m"}, Dimension::length),
        ErrorCode::invalid_input,
        "conversion overflow rejected");
    bad(canonical_quantity({std::numeric_limits<double>::min(), "Pa"}, Dimension::pressure),
        ErrorCode::invalid_input,
        "conversion underflow rejected");
    bad(canonical_quantity({std::numeric_limits<double>::denorm_min(), "mm"}, Dimension::length),
        ErrorCode::invalid_input,
        "subnormal output rejected explicitly");
    const auto large =
        good(convert_quantity({std::numeric_limits<double>::max(), "m"}, Dimension::length, "m"),
             "same-unit conversion avoids overflowing an unnecessary intermediate");
    check(large.value == std::numeric_limits<double>::max(), "finite maximum preserved");
}

void coordinate_frames() {
    CoordinateFrame frame;
    frame.origin_mm = {10, 20, 30};
    check(good(validate_frame(frame), "identity frame"), "frame validated");
    vector_near(good(local_to_global(frame, {1, 2, 3}), "BP-09 coordinates"),
                {11, 22, 33},
                "BP-09 translation");
    vector_near(good(global_to_local(frame, {11, 22, 33}), "inverse identity frame"),
                {1, 2, 3},
                "inverse translation");
    frame.basis = {{{0, -1, 0}, {1, 0, 0}, {0, 0, 1}}};
    vector_near(good(local_to_global(frame, {1, 2, 3}), "rotated frame"),
                {8, 21, 33},
                "columns define local axes in global coordinates");
    vector_near(good(global_to_local(frame, {8, 21, 33}), "inverse rotated frame"),
                {1, 2, 3},
                "inverse uses transpose of proper basis");

    const auto diagonal = std::sqrt(0.5);
    frame.basis = {{{diagonal, -diagonal, 0}, {diagonal, diagonal, 0}, {0, 0, 1}}};
    const auto global = good(local_to_global(frame, {-4, 5, 6}), "45 degree transform");
    vector_near(good(global_to_local(frame, global), "45 degree inverse"),
                {-4, 5, 6},
                "rotated round trip");

    auto bad_frame = CoordinateFrame{};
    bad_frame.basis[0][0] = -1;
    bad(validate_frame(bad_frame), ErrorCode::invalid_input, "reflection determinant rejected");
    bad_frame.basis[0][0] = 2;
    bad(validate_frame(bad_frame), ErrorCode::invalid_input, "scaled axis rejected");
    bad_frame.basis[0][0] = 0;
    bad(validate_frame(bad_frame), ErrorCode::invalid_input, "singular basis rejected");
    bad_frame = {};
    bad_frame.basis[0][1] = 1e-8;
    bad(validate_frame(bad_frame), ErrorCode::invalid_input, "sheared axes rejected");
    bad_frame = {};
    bad_frame.basis[0][0] += 2e-10;
    check(good(validate_frame(bad_frame), "frame numerical tolerance"), "1e-9 tolerance applied");
    bad_frame.origin_mm[0] = std::numeric_limits<double>::infinity();
    bad(validate_frame(bad_frame), ErrorCode::invalid_input, "nonfinite origin rejected");
    bad_frame = {};
    bad_frame.basis[0][1] = std::numeric_limits<double>::quiet_NaN();
    bad(local_to_global(bad_frame, {1, 2, 3}),
        ErrorCode::invalid_input,
        "nonfinite basis rejected");
    bad(global_to_local(CoordinateFrame{}, {std::numeric_limits<double>::infinity(), 0, 0}),
        ErrorCode::invalid_input,
        "nonfinite coordinate rejected");
    bad_frame = {};
    bad_frame.origin_mm[0] = std::numeric_limits<double>::max();
    bad(local_to_global(bad_frame, {std::numeric_limits<double>::max(), 0, 0}),
        ErrorCode::invalid_input,
        "coordinate overflow rejected");
    bad(local_to_global(CoordinateFrame{}, {std::numeric_limits<double>::denorm_min(), 0, 0}),
        ErrorCode::invalid_input,
        "coordinate underflow rejected");
}

void scalar_tables() {
    const auto table = good(ScalarTable1D::create(Dimension::length,
                                                  Dimension::force,
                                                  {{{0, "mm"}, {1, "N"}}, {{1, "mm"}, {2, "N"}}}),
                            "BP-09 scalar table");
    const auto value =
        good(table.evaluate({0.5, "mm"}, OutsidePolicy::reject), "BP-09 interpolation");
    near(value.value, 1.5, "BP-09 (0,1),(1,2) at x0.5 equals 1.5");
    check(value.unit == "N", "table returns canonical y unit");
    check(table.x_dimension() == Dimension::length && table.y_dimension() == Dimension::force,
          "table dimensions exposed read-only");
    near(good(table.evaluate({0, "mm"}, OutsidePolicy::reject), "first knot").value,
         1,
         "first knot");
    near(good(table.evaluate({1, "mm"}, OutsidePolicy::reject), "last knot").value, 2, "last knot");
    bad(table.evaluate({-1, "mm"}, OutsidePolicy::reject),
        ErrorCode::invalid_input,
        "below range rejects");
    bad(table.evaluate({2, "mm"}, OutsidePolicy::reject),
        ErrorCode::invalid_input,
        "above range rejects");
    near(good(table.evaluate({-1, "mm"}, OutsidePolicy::clamp), "low clamp").value, 1, "low clamp");
    near(
        good(table.evaluate({2, "mm"}, OutsidePolicy::clamp), "high clamp").value, 2, "high clamp");
    bad(table.evaluate({0.5, "mm"}, static_cast<OutsidePolicy>(999)),
        ErrorCode::invalid_input,
        "invalid range policy rejects");
    bad(table.evaluate({0.5, ""}, OutsidePolicy::reject),
        ErrorCode::missing_input,
        "query unit required");
    bad(table.evaluate({0.5, "N"}, OutsidePolicy::reject),
        ErrorCode::invalid_unit,
        "query dimension checked");
    bad(table.evaluate({std::numeric_limits<double>::quiet_NaN(), "mm"}, OutsidePolicy::reject),
        ErrorCode::invalid_input,
        "query must be finite");

    const auto mixed =
        good(ScalarTable1D::create(Dimension::length,
                                   Dimension::force,
                                   {{{0, "m"}, {1, "kN"}}, {{0.001, "m"}, {2, "kN"}}}),
             "table normalizes supplied units");
    near(good(mixed.evaluate({0.0005, "m"}, OutsidePolicy::reject), "mixed query").value,
         1500,
         "query and ordinates normalized");
    check(mixed.points()[1].x.unit == "mm" && mixed.points()[1].y.unit == "N",
          "stored table metadata uses canonical units");

    for (const auto& points : std::vector<std::vector<TablePoint>>{
             {},
             {{{0, "mm"}, {1, "N"}}},
             {{{1, "mm"}, {1, "N"}}, {{0, "mm"}, {2, "N"}}},
             {{{1, "m"}, {1, "N"}}, {{1000, "mm"}, {2, "N"}}},
             {{{0, "mm"}, {1, "N"}}, {{std::numeric_limits<double>::quiet_NaN(), "mm"}, {2, "N"}}},
             {{{0, "mm"}, {1, "N"}},
              {{1, "mm"}, {std::numeric_limits<double>::infinity(), "N"}}}}) {
        bad(ScalarTable1D::create(Dimension::length, Dimension::force, points),
            ErrorCode::invalid_input,
            "invalid table shape/order/numbers rejected");
    }
    bad(ScalarTable1D::create(
            Dimension::length, Dimension::force, {{{0, "mm"}, {1, ""}}, {{1, "mm"}, {2, "N"}}}),
        ErrorCode::missing_input,
        "table ordinate unit required");
    bad(ScalarTable1D::create(
            Dimension::length, Dimension::force, {{{0, "N"}, {1, "N"}}, {{1, "mm"}, {2, "N"}}}),
        ErrorCode::invalid_unit,
        "table abscissa dimension checked");

    const auto maximum = std::numeric_limits<double>::max();
    const auto extreme = good(ScalarTable1D::create(Dimension::length,
                                                    Dimension::force,
                                                    {{{-maximum, "mm"}, {-maximum, "N"}},
                                                     {{maximum, "mm"}, {maximum, "N"}}}),
                              "wide but finite table");
    near(good(extreme.evaluate({0, "mm"}, OutsidePolicy::reject), "wide interval interpolation")
             .value,
         0,
         "stable interpolation avoids overflowing coordinate/ordinate differences");
    auto source = table;
    const auto moved = std::move(source);
    near(good(moved.evaluate({0.5, "mm"}, OutsidePolicy::reject), "moved table").value,
         1.5,
         "move retains the canonical data");
    if (source.points().empty()) {
        bad(source.evaluate({0.5, "mm"}, OutsidePolicy::reject),
            ErrorCode::invalid_input,
            "moved-from table fails safely");
    }
    const auto minimum = std::numeric_limits<double>::min();
    const auto tiny =
        good(ScalarTable1D::create(Dimension::length,
                                   Dimension::force,
                                   {{{0, "mm"}, {minimum, "N"}}, {{1, "mm"}, {-minimum, "N"}}}),
             "small normal ordinates");
    bad(tiny.evaluate({0.25, "mm"}, OutsidePolicy::reject),
        ErrorCode::invalid_input,
        "interpolated underflow rejected");
    near(good(tiny.evaluate({0.5, "mm"}, OutsidePolicy::reject), "exact interpolated zero").value,
         0,
         "true cancellation to zero accepted");
}
} // namespace

int main() {
    try {
        unit_conversion();
        invalid_units_and_numeric_ranges();
        coordinate_frames();
        scalar_tables();
        std::cout << "PASS: BP-09 parameter arithmetic and unit/frame/table rejection cases\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
