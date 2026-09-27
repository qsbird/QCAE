# qcae_engine_api

Server-side JSON adaptation and dispatch into application, codec ports and selection services.

Public API: `qcae/ipc_api.hpp`, `qcae/ipc_model.hpp`, `qcae/ipc_selection.hpp`

Direct dependencies: `qcae_application`, `qcae_query`, `qcae_contracts`, `Qt6::Core`

Boundary: Engine-only dependency. Clients must not include these headers or link this target.

Minimal consumer: `#include "qcae/ipc_api.hpp"`; link `qcae_engine_api` in CMake.

Validation from the repository root after configuring/building the `QCAE_BUILD_IPC=ON` (desktop also requires IPC/storage):

```sh
ctest --test-dir build-local --output-on-failure -R "^(ipc|m1_ipc|m23_ipc)$"
```

Actual ownership and links: [target manifest](../../modules/targets.json). Generated build directories are not committed.

## C2 typed engine operations

`qcae-engine` constructs `TypedHost` over the same `MemoryApplication::record_application()`
used by legacy requests. The host registers the generated typed inputs, material/mesh-editing
feature handlers, and geometry/mesh task wrappers. `TaskService` uses `record_task_publisher`;
the engine registers `task_row_handler` before reading a workspace. Workers prepare a detached
candidate, and only the application publishes its record changes and terminal task receipt.
The service is created lazily so a recovery-required workspace can first be explicitly recovered.

Every request uses API version `1.1`, a `request_id`, `operation`, and an object `parameters`.
Document requests carry `document_id` and `document_epoch`. The seven typed mutations below
also require `expected_revision` as an unsigned decimal string and `idempotency_key`.
Caller identity comes from the engine's local OS-user session, never request parameters.

| Operation | Parameters | Success data |
| --- | --- | --- |
| `geometry.create_line` | `start_mm`, `end_mm`: three-number arrays | change receipt with `entity_id` |
| `mesh.generate_line` | `geometry_id`: string; `segments`: positive integer, up to 100000 | `task_id`, `state`, `progress`, `events`, `input_revision` |
| `material.create` | `name`, `young_modulus: {value, unit}`, optional `poisson_ratio` | change receipt with `entity_id` |
| `material.set_young_modulus` | `entity_id`, `young_modulus: {value, unit}` | change receipt |
| `section.create` | `name`, `material_id`, `area_mm2`, `i1_mm4`, `i2_mm4`, `torsion_mm4` | change receipt with `entity_id` |
| `beam.assign_section` | `beam_ids`: string array; `section_id`: string | change receipt |
| `node.move` | `entity_id`, `position_mm`: three-number array | change receipt |
| `task.status` | `task_id` | task state/events, terminal `receipt` or `diagnostic` |
| `task.cancel` | `task_id` | task state/events and `accepted` cancellation acknowledgement |
| `task.reconcile` | empty object | `reconciled`: reload task facts after a definite state-write failure |
| `entity.fields` | `entity_id` | `entity_id`, `kind`, `revision`, `fields` keyed by entity schema field names |

`task.status`, `task.cancel`, `task.reconcile`, and `entity.fields` require document/epoch but no expected revision.
`task.reconcile` is an explicit control action by the trusted local host caller. It checks the
current session and only succeeds when workers and the queue are quiescent. It can durably
mark unfinished rows interrupted after a definite state-write rollback; it never reruns work
or changes the model revision. An uncertain application commit still requires explicit
`project.open` recovery first. Failed reconciliation remains retryable.
A mesh is initially unassigned; create a material and section and assign its beams explicitly.
`entity.query` basic paging for geometry, mesh, nodes, beams, materials and sections reads the
record view directly. Existing query filters and desktop requests keep their compatible paths.
Schema fields use reference strings and three-number vectors; absent optional fields are omitted.
For example, `entity.fields` returns a node's `fields.position` and optional `fields.mesh`.

Receipts expose decimal-string revisions and stable `entity_id` values for created entities.
`operations.get` with `lookup_scope: "document"`, `original_operation` and the original
`idempotency_key` queries persisted typed write receipts. Mesh-start outcomes are read by
`task.status`; `operations.get` does not represent task admission. Existing `history.undo`,
`history.redo`, project save/open/recovery and legacy preview/commit requests share this history.
`capabilities.list` includes typed descriptor availability from the registered handlers.
No solver execution, AI bridge, C3 renderer/resource work or large-model claim is implied.
