# qcae_engine_api

Server-side JSON adaptation and dispatch into application, codec ports and selection services.

Public API: `qcae/ipc_api.hpp`, `qcae/ipc_model.hpp`, `qcae/ipc_selection.hpp`,
`qcae/typed_host.hpp`, `qcae/engine_contributions.hpp`

Direct dependencies: `qcae_application`, `qcae_query`, `qcae_contracts`, `Qt6::Core`

Boundary: Engine-only dependency. Clients must not include these headers or link this target.

Minimal consumer: `#include "qcae/ipc_api.hpp"`; link `qcae_engine_api` in CMake.

Validation from the repository root after configuring/building the `QCAE_BUILD_IPC=ON` (desktop also requires IPC/storage):

```sh
ctest --test-dir build-local --output-on-failure -R "^(ipc|m1_ipc|m23_ipc)$"
```

Actual ownership and links: [target manifest](../../modules/targets.json). Generated build directories are not committed.

## C2 typed engine operations

`qcae-engine` calls `assemble_engine` with static `EngineContribution` entries. Record descriptors/rules
are installed and operation contributors collected before the record registry is frozen. The
resulting registry is injected into `MemoryApplication`; `TypedHost` then installs those
operations over that same `MemoryApplication::record_application()` used by
legacy requests. Default material/mesh-editing handlers and geometry/mesh task wrappers are
registered in `engine_contributions.cpp`; the transport host handles wire context and dispatch.
`TaskService` uses `record_task_publisher`;
the engine registers `task_row_handler` before reading a workspace. Workers prepare a detached
candidate, and only the application publishes its record changes and terminal task receipt.
The service is created lazily so a recovery-required workspace can first be explicitly recovered.

Every request uses API version `1.1`, a `request_id`, `operation`, and an object `parameters`.
Document requests carry `document_id` and `document_epoch`. The seven typed mutations below
also require `expected_revision` as an unsigned decimal string and `idempotency_key`.
Caller identity comes from the engine's local OS-user session, never request parameters.

Typed requests may carry top-level `requested_version`, a positive JSON uint32 matching the
operation's discovered `version`. A mismatched installed version returns `SCHEMA_UNSUPPORTED`
before handler invocation; malformed values return `INVALID_INPUT`. Existing API 1.1 clients
may omit this field, deliberately selecting the installed typed version. Discovery publishes
`requested_version_field` and `omitted_version_policy` so new clients can pin the contract.
This is separate from transport `api_version`. Other legacy operations keep their existing request
shape and reject these new typed-only top-level context fields. The intrinsic
`project.open` input contract is described below.

A typed operation declaring `requires_expected_profile: true` requires top-level
`expected_profile: {profile_id, profile_version, definition_digest}` with no extra members.
Missing required profile context returns `needs_input`; malformed or unknown members are
rejected. This flag declares required presence: its registered handler validates the exact
target/profile identity through the application preparation path. The current seven product
typed operations are target independent and declare the flag false. No additional solver
operation or generic target matcher is installed by this contract plumbing.

Target-related handlers include the immutable profile in their normalized idempotency
signature and check applicability in `RecordPrepare`, after retained operation facts are
resolved. Thus an identical committed retry returns its old receipt after the target profile
changes, while a new request with the old profile is rejected and a changed profile under the
same key conflicts. A contract version no longer installed is rejected; clients can recover
retained committed write facts through `operations.get` without rerunning that version.

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
All `entity.query` enumeration, filtering and paging, and both directions of
`entity.references`, read `RecordSnapshot` directly. Organization views preserve their
existing owner/closure semantics. The legacy model bridge remains at other legacy edit and
format export boundaries.
Schema fields use reference strings and three-number vectors; absent optional fields are omitted.
For example, `entity.fields` returns a node's `fields.position` and optional `fields.mesh`.

Receipts expose decimal-string revisions and stable `entity_id` values for created entities.
`operations.get` with `lookup_scope: "document"`, `original_operation` and the original
`idempotency_key` queries persisted typed write receipts independently of whether that
operation's handler is currently installed. The lookup validates document/epoch and caller
scope; an unknown or unretained action/key returns `ENTITY_NOT_FOUND`. It never invokes the
operation. Mesh-start outcomes are read by
`task.status`; `operations.get` does not represent task admission. Existing `history.undo`,
`history.redo`, project save/open/recovery and legacy preview/commit requests share this history.
`capabilities.list` includes typed descriptor availability from the registered handlers.
The two-argument `TypedHost` constructor preserves existing feature registrations. Its
three-argument overload accepts an `OperationContributor` over the same `RecordApplication`
and lazy task service and replaces those default registrations. `default_operations()` returns
the built-in contributor for explicit startup composition; a missing contribution cannot
advertise its operation as available. Contribution IDs are unique, and typed contributions
cannot register or declare operations reserved by the intrinsic host controls, existing
operation catalog or `runtime.handshake`. Startup rejects these collisions before publishing
the host. This is static assembly, not dynamic loading.
No solver execution, AI bridge, C3 renderer/resource work or large-model claim is implied.

### Versioned project.open input

The intrinsic `project.open` host route decodes generated `ProjectOpenInput` before calling
its existing application lifecycle service. Discovery keeps the baseline symbolic type names
and additionally publishes `wire_input_type: ProjectOpenInput`, `wire_output_type: DocumentInfo`,
`schema_id: qcae.operation.project.open.v1`, version 1, and the actual parameter schema.
Use `{mode: "normal", path: "..."}` to open a saved project, or `{mode: "recover"}` to recover
an explicit workspace. Normal mode requires a non-empty path; recovery forbids the path field.
Missing, unknown or incorrectly typed parameters return `INVALID_INPUT` with an input field.

The route accepts optional `requested_version`: omission selects the installed version,
malformed values return `INVALID_INPUT`, and a different positive uint32 returns
`SCHEMA_UNSUPPORTED`. Rejections precede lifecycle admission and do not reserve the host key.
`expected_profile` is not accepted. The MCP bridge forwards the two closed `oneOf` branches
with the existing depth and node bounds, and advertises profile context only for descriptors
that explicitly declare `requires_expected_profile`.

Successful requests retain the original caller/key, normal-open and recovery semantics,
host outcome lookup, and application history. This adds an input contract to an existing
route; it does not register another business handler or complete every legacy contract.

### C3 versioned display resources and events

A handshake advertises `capabilities.resources_version=1` and `events_version=1` only
when the production host installs both services. Legacy `view.render_data` remains
available. New operations require `requested_version: 1`; read-only resource/event
requests never commit a model revision or create undo entries.

`view.render_resource` takes the normal document/epoch/expected_revision envelope,
plus `view_session_id` and `expected_view_revision`. Optional `base_revision` and
`base_view_revision` must be supplied together. Its response contains `mode`
(`full` or `delta`), a `manifest`, bounded `changed_ids`, and `refresh_tree`. A missing,
expired, or mismatched projection baseline produces a complete resource. Existing
point coordinates, geometry endpoints, and nonvisual edits can produce a delta;
topology and visibility changes produce a complete packet. The client validates the
installed baseline again before any VTK mutation.

A manifest identifies immutable bytes by resource ID, document, epoch, revision,
view ID/revision, byte length, SHA-256, chunk size/count and media type. Display
media types are `qcae.render.packet.v1` and `qcae.render.delta.v1`; their explicit
little-endian codec lives in the transport adapter. Core contracts contain no Qt
or VTK types. Maximum resource size is 16 MiB; the default cache holds at most
64 MiB/32 resources, with 60-second leases. Released leases become evictable.

`resources.describe` and `resources.read` require that the authoritative document,
model and view versions still match. Parameters include `resource_id`, view ID and
view revision; read also requires an unsigned decimal-string `offset`. A read
returns the pinned manifest, offset, raw length, `encoding: "base64"`, and
`data_base64`. Chunks contain at most 128 KiB raw bytes; complete framed responses
are checked against 256 KiB. This is explicitly chunked binary content inside the
existing JSON control transport, not a separate raw binary socket protocol.
`resources.release` needs only the trusted caller and resource ID, so abandoned
transfers can be released after the active document changes. No client upload or
arbitrary file access operation is exposed.

The shared resource client rejects gaps, duplicate/out-of-order chunks, malformed
base64, length/offset errors, digest changes and any context change. It publishes
bytes only after complete verification. Superseded or destroyed clients release
buffers and leases; late callbacks cannot install data in another scene.

`events.subscribe`/`events.read` take an engine instance ID and unsigned decimal
`after_sequence`; a page has at most 64 events. Notifications carry `frame_type`,
engine instance, monotonic sequence, event kind, document/epoch/revision and data.
Document, history, project metadata and persisted task changes are observed from
the same application. The feed retains 256 events and at most 64 subscribers by
default. Duplicate events are ignored; missing events, a changed engine instance,
expired retention or uncertain authoritative storage cause explicit resync.
Notifications never replace current-document/task queries. Reconnection never
replays a write. Slow socket consumers are disconnected by the host's existing
output bound and must reconnect/resynchronize.

Desktop synchronization uses notifications with a two-second authoritative poll as
fallback. A gap immediately disables scene-based selection, clears partial resource
transfers/previews, and refreshes document/task state before a full display refresh.
Normal local edits preserve the installed scene baseline until a checked delta
arrives. VTK partitions point/line geometry into blocks of 1024 entities; selection
changes update highlight cells without rebuilding the base scene. Structural
changes still rebuild. Dock/toolbar layout is a local Qt setting and never enters
the document transaction history.
