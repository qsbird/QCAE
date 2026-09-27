#pragma once
#include "qcae/types.hpp"
#include <array>
#include <span>
#include <string_view>
#include <vector>

namespace qcae::parameters {

enum class Dimension { length, force, pressure, area, inertia, angle };

struct UnitDescriptor {
    std::string_view symbol;
    Dimension dimension;
    std::string_view canonical_symbol;
    double to_canonical;
};

// Fixed read-only metadata for field editors and service validation.
[[nodiscard]] std::span<const UnitDescriptor> unit_registry();
[[nodiscard]] std::string_view canonical_unit(Dimension dimension);
[[nodiscard]] Result<Quantity>
convert_quantity(const Quantity& input, Dimension expected_dimension, std::string_view output_unit);
[[nodiscard]] Result<Quantity> canonical_quantity(const Quantity& input,
                                                  Dimension expected_dimension);

using Vector3 = std::array<double, 3>;
using Basis3 = std::array<Vector3, 3>;

struct CoordinateFrame {
    Vector3 origin_mm{};
    // basis[row][column]: each column is a local axis expressed in global coordinates.
    Basis3 basis{{{1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0}}};
};

inline constexpr double frame_tolerance = 1e-9;
[[nodiscard]] Result<bool> validate_frame(const CoordinateFrame& frame);
[[nodiscard]] Result<Vector3> local_to_global(const CoordinateFrame& frame,
                                              const Vector3& local_mm);
[[nodiscard]] Result<Vector3> global_to_local(const CoordinateFrame& frame,
                                              const Vector3& global_mm);

enum class OutsidePolicy { reject, clamp };

struct TablePoint {
    Quantity x;
    Quantity y;
};

class ScalarTable1D {
  public:
    [[nodiscard]] static Result<ScalarTable1D>
    create(Dimension x_dimension, Dimension y_dimension, std::vector<TablePoint> points);
    // No implicit extrapolation policy: callers must choose reject or clamp.
    [[nodiscard]] Result<Quantity> evaluate(const Quantity& x, OutsidePolicy outside_policy) const;
    [[nodiscard]] std::span<const TablePoint> points() const;
    [[nodiscard]] Dimension x_dimension() const;
    [[nodiscard]] Dimension y_dimension() const;

  private:
    ScalarTable1D(Dimension x_dimension,
                  Dimension y_dimension,
                  std::vector<TablePoint> canonical_points);
    Dimension x_dimension_;
    Dimension y_dimension_;
    std::vector<TablePoint> points_;
};

} // namespace qcae::parameters
