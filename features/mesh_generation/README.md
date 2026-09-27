# Uniform line mesh task

`line_mesh_task` captures an immutable `RecordSnapshot`, a GeometryId, segment
count and optional section assignment. It returns a `TaskRequest`; the host starts
it on `TaskService`. `line_mesh_identity(task_id)` identifies its output, and the
successful task receipt also contains that MeshId as `primary_entity`. Geometry and
section references are resolved against that immutable snapshot only when new work
runs, after task-start deduplication. A completed task therefore remains replayable
after undo removes its source entities; a new task with missing sources fails
without publishing a model change.

The worker computes real endpoint interpolation and consecutive two-node
connectivity. Ten segments on the 1000 mm line produce eleven nodes and ten line
elements. All records carry explicit mesh ownership and geometry revision; generated
IDs are checked for collisions. Deterministic transverse orientation remains
nonparallel to the line. Progress is bounded, cancellation is checked throughout
generation and again before publication, and the application rejects stale input.

The existing generated `Beam` record represents this subset's Line2 topology with
an optional section assignment. This permits geometry/mesh creation before physics
configuration without duplicating topology authority. A section, material and other
analysis inputs remain necessary for Nastran readiness/export. The strict legacy
Beam model is unchanged; its explicit bridge uses an empty section ID for an
unassigned element, which strict legacy validation/export rejects.

This feature currently creates a new mesh. It does not claim mapped replacement or
remeshing of a previously referenced mesh. Those operations must implement the
frozen `reject_unmapped` policy before they can be advertised. `geometry_mesh` tests
ten successes, ten accepted cancellations and ten stale candidates with deterministic
barriers, exact coordinates/connectivity and model revision assertions.
`geometry_mesh_sqlite` runs the same thirty cases against distinct SQLite workspaces;
each case reopens its store and verifies persisted task state, transaction and primary
entity identities. Both variants emit one JSON evidence row per case and also verify
100 undo/redo rounds around a one-node move, with zero semantic differences and an
exact revision increase of 201. These are component tests, not complete SK-06/SK-07
or process-crash acceptance evidence.
