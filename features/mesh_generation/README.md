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

`register_handlers` in `mesh_generation_operations.cpp` registers `mesh.generate_line` and
`mesh.regenerate_line`. The feature builds the `TaskRequest`; the host passes a `StartTask`
callback that starts it on `TaskService` and reports status in the format its other
background operations share.

## Region meshes

`region_mesh_task` meshes a flat region (`GeometryRegion`) with triangles through the
`IPlanarMesher` port. The host registers `mesh.generate_region` with `register_region_mesh` and
the mesher it created at start-up; without one, as in a build without Gmsh, the operation is
listed as unavailable with the reason. The element size is checked before the task starts, since it
depends only on the input. The region is resolved inside the task, so a completed task still replays
after undo removes its region. The mesher cannot be interrupted: a cancel that arrives while it runs
takes effect right after it returns, and nothing is published. Requests are capped at 20,000
triangles, about half a second of meshing and 30,000 records. The published Mesh is bound to the
region revision, with a Node per mesh node and a Tri3 per triangle. A failure inside the task
reaches the caller as the task's diagnostic message; the task service reports its code as invalid
input of the task. `region_mesh` and `region_mesh_sqlite` test the task with a stand-in mesher;
`region_mesh_ipc` meshes a plate with a hole through the engine with Gmsh.

The line mesher below currently creates a new mesh. It does not claim mapped replacement or
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
