# Line geometry feature

`qcae_geometry_features` contributes typed `RecordPrepare` handlers through the
application's existing coordinator. Inputs use explicit canonical millimetres;
the operation boundary must normalize user quantities before calling them.

`create_line_handler` produces one independent GeometryId with finite, distinct
endpoints. `line_geometry_signature` supplies the same normalized input signature
used by `RecordApplication::execute`, so retries retain the original receipt and
primary entity ID. `move_line_endpoint_handler` increments geometry revision and
marks every bound mesh stale in the same prepared change. It does not silently
retarget existing mesh or analysis references.

`register_handlers` in `geometry_operations.cpp` exposes these handlers as the typed
operations `geometry.create_line`, `geometry.evaluate_line` and `geometry.move_endpoint`; the
engine host calls it while composing its operations.

## Flat regions

`geometry.create_region` describes a flat plate: an outer loop of straight edges and optional
holes, each loop a list of corners in millimetres. `prepare_create_region` follows the standard
operation recipe. It checks the input with `measure_planar_region` first, so a bad loop is reported
against `outer` or `holes[i]`, then creates everything in one change: a `GeometryPoint` per corner,
a `GeometryLoop` per outline and the `GeometryRegion` joining them. This mirrors the points, curve
loops and plane surfaces of Gmsh's built-in geometry kernel. The `GeometryRegion` record rule
repeats the check on every commit. Regions cannot be edited yet; a mesh may name a region as its
source geometry.

The handler depends only on application/document contracts. It never writes a
database or owns another model. Test with `geometry_mesh` and `record_document`.
