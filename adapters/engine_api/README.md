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
The assembly also retains the actual owning codec/Profile binding and validates and retains the
factory's frozen render contributions. The production host uses these selected services for IPC,
Profile support checks and rendering;
the build flag only selects the default contribution list. Nastran's binding retains the same
coordinator State and codec used by its operation callbacks. Duplicate bindings/factories,
invalid ownership and invalid Profile definitions are rejected before the assembly is returned
and the typed host is published. Socket listening and some workspace initialization happen earlier.
Without a render factory, the existing generic projector remains available. The actual six-category
startup catalog is described below; full graphical regression and SK/P0 acceptance remain open.
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
This is separate from transport `api_version`. The five intrinsic project lifecycle routes
below and `changes.commit` also accept this operation version. Other legacy operations keep their existing request
shape and reject the new context fields.

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

### Versioned project lifecycle inputs

The five intrinsic host routes decode generated inputs before calling their existing application
lifecycle services. Discovery retains the baseline symbolic type names and adds the actual
`wire_input_type`, `wire_output_type: DocumentInfo`, `schema_id: qcae.operation.<operation>.v1`,
version 1 and a closed parameter schema.

| Host route | Generated input | Parameters |
| --- | --- | --- |
| `project.create` | `ProjectCreateInput` | required non-empty `name`; the existing application byte quota still applies |
| `project.open` | `ProjectOpenInput` | `{mode: "normal", path: "..."}` or `{mode: "recover"}`; normal requires a non-empty path, recovery forbids it |
| `project.save` | `ProjectSaveInput` | optional string `path`, including the empty string; original application location semantics |
| `project.save_as` | `ProjectSaveAsInput` | optional string `path`, including the empty string; original application success/error semantics |
| `project.close` | `ProjectCloseInput` | required `policy`: `discard` or `keep_recovery` |

Missing required, unknown or incorrectly typed parameters return `INVALID_INPUT` with an input
field. Optional `requested_version` selects the installed version when omitted; malformed values
return `INVALID_INPUT`, and a different positive uint32 returns `SCHEMA_UNSUPPORTED`. Rejections
precede lifecycle admission and do not reserve the host key. `expected_profile` is not accepted.
The MCP bridge forwards the same schemas, including open's two closed `oneOf` branches, within
its existing depth and node bounds. It advertises Profile context only for descriptors explicitly
declaring `requires_expected_profile`.

Successful calls retain the original caller/key namespaces, outcomes, lookup and application
history. Omitted and empty save paths retain their existing normalization and replay behavior;
explicit saved paths retain their existing signatures. Normal open and recovery remain distinct.
These contracts adapt existing routes; they do not register duplicate business handlers or
complete every legacy contract. Actual tests and packaged execution are described in the
[local delivery evidence](../../docs/engineering/core-local-delivery-2026-10-03.md).

### Versioned preview commit input

`changes.commit` decodes generated `ChangesCommitInput` with one required non-empty string
`preview_id`, no extra fields, schema `qcae.operation.changes.commit.v1`, and installed version 1.
Discovery adds `wire_input_type: ChangesCommitInput` and `wire_output_type: ChangeReceipt`.
Omitted/v1 calls use the same version policy as lifecycle inputs. Malformed or unsupported
versions and input shapes are refused before the existing application commit; `expected_profile`
is not accepted. The original document/epoch/revision/key context, caller namespace, receipt,
undo and persistent outcome lookup remain authoritative. No TypedHost registration is added
for this reserved compatibility route. Stable identity and fields survive explicit recovery;
storage iteration order is not an identity contract. Actual SQLite refusal/replay/recovery tests,
the corrected test-only ordering assumption and retained graphical limits are in
[the commit-contract evidence](../../docs/engineering/changes-commit-contract-2026-10-03.md).

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

## Actual static contribution discovery

Successful production `capabilities.list` adds `package_contributions_version:1` and an array
`package_contributions`. Each `contribution_id` owns six arrays: `core`, `operations`, `codecs`,
`validation`, `ui` and `render`. The source is the selected assembly and actual registration
changes over its frozen record registry and the same application used by all clients.

- Core record entries contain `kind:record`, stable `record_type`, name and current version;
  named rule entries contain `kind:rule` and ID tied to actual appended rule indices.
- Operations contain actual name, version, schema_id and available status.
- Codecs contain the selected owned `profile_ref`; validation contains owned callback ID/version.
- UI references this contributor's available read-only operation and installed version.
- Render lists the actual selected factory's stable record_type/topology entries.

The Nastran contribution `qcae.nastran` registers all six categories. Its export handler uses
the registered validator before task/file side effects while retaining original replay checks.
Catalog reads require completed operation/UI registration; production publishes only after
successful TypedHost construction. This is trusted static startup metadata, not a dynamic
plugin ABI or a generic callback invocation operation. Host intrinsic routes remain in the
global operation catalog. Default OFF and ON platform-only assemblies omit the Nastran owner;
generic rendering fallback is not attributed to that absent package.

Actual scope, full SQLite row observations and the unclosed desktop selection timeout are in
[the contribution validation](../../docs/engineering/package-contributions-2026-10-03.md).
