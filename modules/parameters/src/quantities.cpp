#include "qcae/quantities.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <string>
#include <utility>

namespace qcae::parameters {
namespace {
constexpr std::array units{
    UnitDescriptor{"mm", Dimension::length, "mm", 1.0},
    UnitDescriptor{"m", Dimension::length, "mm", 1000.0},
    UnitDescriptor{"N", Dimension::force, "N", 1.0},
    UnitDescriptor{"kN", Dimension::force, "N", 1000.0},
    UnitDescriptor{"Pa", Dimension::pressure, "MPa", 1e-6},
    UnitDescriptor{"kPa", Dimension::pressure, "MPa", 1e-3},
    UnitDescriptor{"MPa", Dimension::pressure, "MPa", 1.0},
    UnitDescriptor{"GPa", Dimension::pressure, "MPa", 1000.0},
    UnitDescriptor{"mm2", Dimension::area, "mm2", 1.0},
    UnitDescriptor{"m2", Dimension::area, "mm2", 1e6},
    UnitDescriptor{"mm4", Dimension::inertia, "mm4", 1.0},
    UnitDescriptor{"m4", Dimension::inertia, "mm4", 1e12},
    UnitDescriptor{"rad", Dimension::angle, "rad", 1.0},
    UnitDescriptor{"deg", Dimension::angle, "rad", std::numbers::pi / 180.0},
};

template <class T>
Result<T>
failure(ErrorCode code, std::string message, std::string field, Status status = Status::failed) {
    return {status, std::nullopt, Diagnostic{code, std::move(message), std::move(field)}};
}

template <class T> Result<T> success(T value) {
    return {Status::success, std::move(value), std::nullopt};
}

const UnitDescriptor* find_unit(std::string_view symbol) {
    const auto found = std::find_if(
        units.begin(), units.end(), [symbol](const auto& unit) { return unit.symbol == symbol; });
    return found == units.end() ? nullptr : &*found;
}

Result<double> checked_value(long double value, std::string field) {
    if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<double>::max()) {
        return failure<double>(ErrorCode::invalid_input,
                               "Numeric result is nonfinite or exceeds the double range.",
                               std::move(field));
    }
    // Keep the supported numeric range explicit, rather than silently flushing denormals to zero.
    if (value != 0.0L && std::abs(value) < std::numeric_limits<double>::min()) {
        return failure<double>(ErrorCode::invalid_input,
                               "Numeric result underflows the supported normal double range.",
                               std::move(field));
    }
    return success(static_cast<double>(value));
}

Result<Vector3> transform(const CoordinateFrame& frame, const Vector3& input, bool inverse) {
    const auto valid = validate_frame(frame);
    if (!valid.ok()) {
        return {valid.status, std::nullopt, valid.error};
    }
    for (double value : input) {
        if (!std::isfinite(value)) {
            return failure<Vector3>(ErrorCode::invalid_input,
                                    "Coordinates must be finite millimeter values.",
                                    "coordinates");
        }
    }
    Vector3 output{};
    for (std::size_t row = 0; row < 3; ++row) {
        long double value = inverse ? 0.0L : static_cast<long double>(frame.origin_mm[row]);
        for (std::size_t column = 0; column < 3; ++column) {
            const long double component =
                inverse ? static_cast<long double>(input[column]) - frame.origin_mm[column]
                        : input[column];
            const long double coefficient =
                inverse ? frame.basis[column][row] : frame.basis[row][column];
            value = std::fma(coefficient, component, value);
        }
        const auto checked = checked_value(value, "coordinates");
        if (!checked.ok()) {
            return {checked.status, std::nullopt, checked.error};
        }
        output[row] = *checked.value;
    }
    return success(output);
}
} // namespace

std::span<const UnitDescriptor> unit_registry() {
    return units;
}

std::string_view canonical_unit(Dimension dimension) {
    switch (dimension) {
    case Dimension::length:
        return "mm";
    case Dimension::force:
        return "N";
    case Dimension::pressure:
        return "MPa";
    case Dimension::area:
        return "mm2";
    case Dimension::inertia:
        return "mm4";
    case Dimension::angle:
        return "rad";
    }
    return {};
}

Result<Quantity> convert_quantity(const Quantity& input,
                                  Dimension expected_dimension,
                                  std::string_view output_unit) {
    if (input.unit.empty() || output_unit.empty()) {
        return failure<Quantity>(ErrorCode::missing_input,
                                 "Input and output units must be explicit.",
                                 input.unit.empty() ? "unit" : "output_unit",
                                 Status::needs_input);
    }
    const auto* source = find_unit(input.unit);
    const auto* target = find_unit(output_unit);
    if (!source || !target || source->dimension != expected_dimension ||
        target->dimension != expected_dimension) {
        return failure<Quantity>(
            ErrorCode::invalid_unit, "Unknown unit or incompatible quantity dimension.", "unit");
    }
    if (!std::isfinite(input.value)) {
        return failure<Quantity>(
            ErrorCode::invalid_input, "Quantity value must be finite.", "value");
    }
    // Form the ratio first: converting an already large canonical value to itself must not
    // overflow.
    const long double factor =
        static_cast<long double>(source->to_canonical) / target->to_canonical;
    const long double scaled_value = static_cast<long double>(input.value) * factor;
    if (input.value != 0.0 && scaled_value == 0.0L) {
        return failure<Quantity>(
            ErrorCode::invalid_input, "Quantity conversion underflowed to zero.", "value");
    }
    const auto converted = checked_value(scaled_value, "value");
    if (!converted.ok()) {
        return {converted.status, std::nullopt, converted.error};
    }
    return success(Quantity{*converted.value, std::string(output_unit)});
}

Result<Quantity> canonical_quantity(const Quantity& input, Dimension expected_dimension) {
    const auto symbol = canonical_unit(expected_dimension);
    if (symbol.empty()) {
        return failure<Quantity>(
            ErrorCode::invalid_unit, "Unknown quantity dimension.", "dimension");
    }
    return convert_quantity(input, expected_dimension, symbol);
}

Result<bool> validate_frame(const CoordinateFrame& frame) {
    for (double value : frame.origin_mm) {
        if (!std::isfinite(value)) {
            return failure<bool>(ErrorCode::invalid_input,
                                 "Coordinate origin must be finite millimeter values.",
                                 "origin_mm");
        }
    }
    for (const auto& row : frame.basis) {
        for (double value : row) {
            if (!std::isfinite(value)) {
                return failure<bool>(ErrorCode::invalid_input,
                                     "Coordinate basis must contain finite values.",
                                     "basis");
            }
        }
    }
    for (std::size_t first = 0; first < 3; ++first) {
        for (std::size_t second = first; second < 3; ++second) {
            long double dot = 0.0L;
            for (std::size_t row = 0; row < 3; ++row) {
                dot += static_cast<long double>(frame.basis[row][first]) * frame.basis[row][second];
            }
            const long double expected = first == second ? 1.0L : 0.0L;
            if (!std::isfinite(dot) || std::abs(dot - expected) > frame_tolerance) {
                return failure<bool>(ErrorCode::invalid_input,
                                     "Coordinate axes must be orthonormal within 1e-9.",
                                     "basis");
            }
        }
    }
    const auto& b = frame.basis;
    const long double determinant =
        static_cast<long double>(b[0][0]) * (static_cast<long double>(b[1][1]) * b[2][2] -
                                             static_cast<long double>(b[1][2]) * b[2][1]) -
        static_cast<long double>(b[0][1]) * (static_cast<long double>(b[1][0]) * b[2][2] -
                                             static_cast<long double>(b[1][2]) * b[2][0]) +
        static_cast<long double>(b[0][2]) * (static_cast<long double>(b[1][0]) * b[2][1] -
                                             static_cast<long double>(b[1][1]) * b[2][0]);
    if (!std::isfinite(determinant) || std::abs(determinant - 1.0L) > frame_tolerance) {
        return failure<bool>(ErrorCode::invalid_input,
                             "Coordinate basis must be a proper rotation with determinant +1.",
                             "basis");
    }
    return success(true);
}

Result<Vector3> local_to_global(const CoordinateFrame& frame, const Vector3& local_mm) {
    return transform(frame, local_mm, false);
}

Result<Vector3> global_to_local(const CoordinateFrame& frame, const Vector3& global_mm) {
    return transform(frame, global_mm, true);
}

ScalarTable1D::ScalarTable1D(Dimension x_dimension,
                             Dimension y_dimension,
                             std::vector<TablePoint> canonical_points)
    : x_dimension_(x_dimension), y_dimension_(y_dimension), points_(std::move(canonical_points)) {}

Result<ScalarTable1D> ScalarTable1D::create(Dimension x_dimension,
                                            Dimension y_dimension,
                                            std::vector<TablePoint> points) {
    if (points.size() < 2) {
        return failure<ScalarTable1D>(
            ErrorCode::invalid_input, "A linear table requires at least two points.", "points");
    }
    for (std::size_t index = 0; index < points.size(); ++index) {
        auto x = canonical_quantity(points[index].x, x_dimension);
        auto y = canonical_quantity(points[index].y, y_dimension);
        if (!x.ok()) {
            x.error->field = "points[" + std::to_string(index) + "].x." + x.error->field;
            return {x.status, std::nullopt, x.error};
        }
        if (!y.ok()) {
            y.error->field = "points[" + std::to_string(index) + "].y." + y.error->field;
            return {y.status, std::nullopt, y.error};
        }
        points[index] = {*x.value, *y.value};
        if (index > 0 && points[index - 1].x.value >= points[index].x.value) {
            return failure<ScalarTable1D>(
                ErrorCode::invalid_input,
                "Table abscissae must be strictly increasing after unit conversion.",
                "points");
        }
    }
    return success(ScalarTable1D{x_dimension, y_dimension, std::move(points)});
}

Result<Quantity> ScalarTable1D::evaluate(const Quantity& x, OutsidePolicy outside_policy) const {
    if (points_.size() < 2) {
        return failure<Quantity>(ErrorCode::invalid_input,
                                 "Table has no valid point range, possibly after a move.",
                                 "points");
    }
    if (outside_policy != OutsidePolicy::reject && outside_policy != OutsidePolicy::clamp) {
        return failure<Quantity>(ErrorCode::invalid_input, "Unknown table range policy.", "policy");
    }
    const auto query = canonical_quantity(x, x_dimension_);
    if (!query.ok()) {
        return query;
    }
    const double value = query.value->value;
    if (value < points_.front().x.value || value > points_.back().x.value) {
        if (outside_policy == OutsidePolicy::reject) {
            return failure<Quantity>(
                ErrorCode::invalid_input, "Query lies outside the table range.", "x");
        }
        return success(value < points_.front().x.value ? points_.front().y : points_.back().y);
    }
    const auto high =
        std::lower_bound(points_.begin(), points_.end(), value, [](const auto& point, double v) {
            return point.x.value < v;
        });
    if (high->x.value == value) {
        return success(high->y);
    }
    const auto& low = *(high - 1);
    const long double scale = std::max(std::abs(low.x.value), std::abs(high->x.value));
    const long double fraction =
        (static_cast<long double>(value) / scale - static_cast<long double>(low.x.value) / scale) /
        (static_cast<long double>(high->x.value) / scale -
         static_cast<long double>(low.x.value) / scale);
    if (!std::isfinite(fraction) || fraction <= 0.0L || fraction >= 1.0L) {
        return failure<Quantity>(ErrorCode::invalid_input,
                                 "Interpolation fraction is outside the supported numeric range.",
                                 "x");
    }
    const auto result = checked_value(std::lerp(static_cast<long double>(low.y.value),
                                                static_cast<long double>(high->y.value),
                                                fraction),
                                      "y");
    if (!result.ok()) {
        return {result.status, std::nullopt, result.error};
    }
    return success(Quantity{*result.value, std::string(canonical_unit(y_dimension_))});
}

std::span<const TablePoint> ScalarTable1D::points() const {
    return points_;
}

Dimension ScalarTable1D::x_dimension() const {
    return x_dimension_;
}

Dimension ScalarTable1D::y_dimension() const {
    return y_dimension_;
}
} // namespace qcae::parameters
