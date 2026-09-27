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

The handler depends only on application/document contracts. It never writes a
database or owns another model. Test with `geometry_mesh` and `record_document`.
