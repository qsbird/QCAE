# qcae_legacy_api

`MemoryApplication` preserves the existing `qcae/core.hpp` API while delegating
all authority, transactions, history and lifecycle to one `RecordApplication`.
Handlers edit generated records directly; legacy `ModelSnapshot` conversion is
an explicit read/export operation. `record_application()` exposes the same
application for newer feature handlers, never a second document instance.

Public interfaces: `qcae/core.hpp`, `qcae/legacy_migration.hpp`. Dependencies:
`qcae_application`, `qcae_document` and `qcae_contracts`. Generic application code
must never depend on this compatibility feature.

`legacy_state.*` contains the isolated v1 project/v2 workspace readers and old
model-history validation. Migration replays the old baseline and complete history,
including redo entries, then derives stable-key record changes. Save-intent tokens
and snapshot bytes remain exact for reconciliation. Migration returns detached
rows; the host commits them to a separate destination and leaves the old workspace
untouched.

An embedder supplying only `IWorkspaceStore` uses a compatibility blob adapter.
That adapter serializes its row image and is not the incremental SQLite path.
Production SQLite implements `IRecordStore` directly and bypasses this fallback.

Minimal consumer: `#include "qcae/core.hpp"`; link `qcae_legacy_api`.
Run `ctest --test-dir build-core --output-on-failure -R
"^(core|model|persistence|allocation_atomicity)$"`.

The optional trailing `MemoryApplication` constructor argument accepts
`std::vector<OwnedRowHandler>`. Composition hosts register task/artifact row
owners here; the facade forwards them to its authoritative application without
linking any runtime feature implementation.

## Legacy migration boundary

Saved v1 `.qcae` projects use ordinary `open_document`: the legacy payload is
converted to records, its engineering IDs/project/content identities survive,
and opening allocates a new DocId/Epoch and starts an empty workspace history.
Opening writes only the host's working destination. It does not rewrite the
project snapshot.

A v2 legacy recovery workspace uses an explicit separate-destination sequence:

1. Read the quiescent source with `read_legacy_workspace_readonly` from
   `qcae/sqlite_store.hpp`. Do not construct a source writer.
2. Pass the returned `StoredWorkspace` to `migrate_legacy_workspace` with the
   destination record registry and configured limits.
3. Turn its detached `LoadedRows.rows` into `RowMutation` values and commit one
   `StoreBatch` with expected generation zero to a fresh destination row store.
4. Construct the application over the destination and explicitly call
   `recover_document`. Recovery retains DocId/revision/content state, issues a
   new Epoch, and rejects write contexts from the old Epoch.

The source generation is provenance, not the destination generation. Legacy
transaction/content-state IDs, the full history including its redo suffix,
idempotent operation outcomes, and pending-save token/snapshot verification
survive conversion. Save reconciliation succeeds only when the published target
has the original token and exact original snapshot bytes. A changed target cannot
become a successful save outcome. Unsupported input or failed validation must
leave both the source and an uncommitted destination unchanged.

`legacy_migration` runs against all six immutable fixtures in
`tests/fixtures/legacy`. It verifies fixture and expected-JSON SHA-256 values,
compares all twelve engineering collections and every authority reference to the
frozen golden values, checks redo's exact material change, facts, save-intent
reconciliation and stale Epoch rejection, and requires zero changed source bytes.
Its JSON reader uses the existing QtCore dependency, so this test is included
when storage and IPC builds are enabled. It is migration evidence, not a claim of
solver execution, real AI integration or complete P0 acceptance.
