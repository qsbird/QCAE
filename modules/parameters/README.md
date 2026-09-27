# qcae_parameters

Pure C++20 units, proper coordinate frames and immutable linear scalar tables for AP-09/BP-09. This module owns arithmetic and metadata; it owns no document, transaction, GUI or persistence state.

Public API: `qcae/quantities.hpp`, namespace `qcae::parameters`. Link `qcae_parameters`; direct dependencies are `qcae_foundation` and `qcae_contracts`. The interface uses only foundation `Quantity`, `Result`, status/error enums and standard containers.

`unit_registry()` returns read-only fixed descriptors for length (`mm`, `m`), force (`N`, `kN`), pressure (`Pa`, `kPa`, `MPa`, `GPa`), area (`mm2`, `m2`), inertia (`mm4`, `m4`) and angle (`rad`, `deg`). Canonical units are `mm`, `N`, `MPa`, `mm2`, `mm4`, `rad`. `canonical_quantity` and `convert_quantity` require an expected dimension. Empty units return `needs_input/missing_input`; unknown or dimension-mismatched units return `failed/invalid_unit`. Unit symbols are case-sensitive. Negative values are permitted by arithmetic; physical constraints belong to the calling feature.

Nonfinite values, conversion overflow and subnormal outputs/underflow to zero return `failed/invalid_input`. Zero itself is valid. The supported nonzero output range is the normal finite double range. Conversion computes the unit ratio before multiplying, so a same-unit value does not overflow merely because its canonical equivalent is larger.

`CoordinateFrame` stores millimeter origin and a 3×3 basis whose columns are local axes in global coordinates. Its basis must be finite, orthonormal and have determinant +1 within absolute tolerance `1e-9`. `local_to_global` applies basis then origin; `global_to_local` uses the transpose after subtracting origin. Inputs/outputs are explicitly millimeter vectors, with no GUI axis conventions. Reflections, scale/shear/singular bases, nonfinite coordinates and numeric range failures return structured errors.

`ScalarTable1D::create` requires at least two points, typed x/y dimensions, finite unit-bearing quantities and strictly increasing x after canonical conversion. The stored points are immutable canonical quantities. `evaluate` requires an explicit `OutsidePolicy::reject` or `OutsidePolicy::clamp`; it performs linear interpolation in the canonical units and returns canonical y. Duplicate/nonincreasing points, unknown units and nonfinite/range failures are rejected. No implicit extrapolation, mutable registry or metaprogramming framework is introduced.

The isolated parameter test covers BP-09 (1m→1000mm, 210GPa→210000MPa, translated point (11,22,33)mm, interpolation 1.5), all catalog dimensions, inverse conversions/rotation, mixed-unit tables and meaningful negative numeric/frame/table cases. After root configuration and registration:

```sh
cmake --build build-core --target qcae_parameter_tests
ctest --test-dir build-core --output-on-failure -R '^parameters$'
```

This arithmetic slice alone does not establish persistent coordinate/table entities, generic editor integration, full SK-08 or complete AP-09 acceptance.
