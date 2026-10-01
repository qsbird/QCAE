# Force vector independent QG-02 / QG-03 review

Reviewer: `/root/lean_refresh_review`; force source owner: root. Scope: the typed
setter/preview and their shared normalization, transaction/history, wire
contracts and existing Core/CLI/script persistence receipts. No product/test
source, CMake, shared build or GUI was changed/run by this review. Independent
checks compiled only a temporary fixture/probe and copied existing archives
into /private/tmp.

## Scoped conclusion

QG-02: no blocking product finding remains in the reviewed force slice. The
entry → typed input → one prepared edit → RecordApplication → persistence/
history path is explicit and reuses existing authority. QG-03: the relevant
Core check suite and production CLI/script operation/persistence regression
have actual GREEN receipts, with the original fixture RED logs retained.
This conclusion excludes the pending force GUI and the newly selected
SQLite 3.53.4 full matrix; it is not P0, solver or real-AI acceptance.

## Authority, units and signature

`features/analysis/include/qcae/analysis_features.hpp:13` exposes the same
standard C++ OperationPlan/RecordPrepare boundary used by existing features.
`analysis_operations.cpp:14–26` normalizes all three components using the
existing force-dimension quantity service, returning its explicit diagnostic
and adding the x/y/z field prefix. No component is omitted, made unsigned,
or inferred to use a unit. The unit registry at
`modules/parameters/src/quantities.cpp:14–15` admits N/kN; `:112–155` validates
explicit dimension, finiteness, supported normal range, conversion overflow/
underflow and retains signed values. Empty unit is MISSING_INPUT/needs_input;
mm is INVALID_UNIT/failed. The wire codec retains its input-prefixed diagnostic,
e.g. input.y.unit, and rejects unknown members/types before preparation.

`analysis_operations.cpp:28–46` derives the canonical signature from normalized
input and captures exactly that value in the prepared operation. `:73–100`
shares the same normalization for create/set: the setter updates only the
existing records::NodalForce force_n, returns its existing ID, and sets creates
false. Node reference, force identity, load-case references and unrelated
records are retained. Missing/wrong-kind IDs fail through EditSession.
There is no identity allocator use, second model, selected solver-number alias
or current-node lookup that retargets the original force.

`analysis_operations.cpp:54–69` passes trusted caller, document/epoch, expected
revision, operation-scoped normalized signature and idempotency key to direct
RecordApplication::execute. `core.cpp:618–666` holds the existing lock, checks
document/epoch first, compares retained operation/context signature for replay,
and performs prepare/commit through the same history/persistence path. Fresh
stale writes fail; same-key different parameters/context conflict. A replay
returns original transaction and committed revision plus current revision and
replayed=true (`core.cpp:155–166`); it does not reapply an undone edit. The
prepared signature is checked again before commit, and the private preview is
released by RAII on the direct path. Existing candidate/reference/profile,
history/resource and durable commit guards are unchanged.

## Preview, compatibility and lifecycle

`analysis_operations.cpp:166–184` converts the preview DTO to the same setter
plan, calls RecordApplication::preview and returns only handle, original
affected ID, creates=false and base revision. No model/history commit or
render install occurs. `core.cpp:371–450` validates current document/epoch/R,
caller and immutable candidate before storing bounded RAM preview state.
`core.cpp:469–542` commits only that caller's exact preview/context, with a
same-lock revision check; interleaving writes invalidate old handles.
`features/legacy_api/src/core.cpp:250–255` MemoryApplication delegates to its
one RecordApplication. The existing changes.commit IPC branch at
`adapters/engine_api/src/ipc_api.cpp:401–411` uses that same port, so typed
preview commits and compatibility undo/redo share authority and history.

Cancellation here means dropping the client handle and issuing no commit.
The test proves zero physical revision/history/model mutation; it does not
implement a server-side release endpoint. The existing RAM token remains
subject to max_previews=128 and is cleared after model revision changes/close
(`core.cpp:170–179`, `:389`, `:599`). Unlimited repeated cancelled previews
and immediate server resource release are not claimed. This is inherited
preview policy, not a new force-specific state store.

`schemas/operations/analysis.json` defines force.set_vector.v1 as document_write
with doc/epoch/R/key required, and force.preview_vector.v1 as preview with
doc/epoch/R required and key not required. Both declare only force_id/x/y/z,
explicit N/kN quantities and the same generated codec. The existing generator,
registry and typed host remain responsible for closed input validation and
discovery; there is no frontend-only business rule or new dependency.
CMake's new storage-gated force_vector_ipc registration (`CMakeLists.txt:254–257`)
uses the actual built engine/CLI and a 90-second bound. Its fixture needs
geometry/material/mesh services, not a configured solver.

## Actual receipts and independent checks

`../force-vector-core-tests40d.log`: analysis_checks 1/1 PASS, 0.64s (total0.65s).
The relevant test at `tests/analysis_check_tests.cpp:251–399` protects normalization,
identity/node/reference retention, one transaction/history, stale check reports,
key replay and conflicting parameters, invalid identity/unit/nonfinite values,
undo/redo, retry after undo, preview/interleaving and compatibility commit/undo.
40b/c retain the initial unit-error expectation fixture failures; source wire
behavior was unchanged while expected MISSING_INPUT/INVALID_UNIT was corrected.

`../force-vector-ipc-tests40b.log` plus
`../../force-vector-ipc40b/{cli,script}/{facts,transcript}.json`: both actual
production modes pass, 140 recorded business request/response pairs each.
Each transcript includes 13 force setters, 2 previews, 2 changes.commit calls,
2 undos and 2 project opens. Ten expected non-success responses per mode
include key/revision conflict, missing/incompatible units, missing/wrong-kind
ID, wrong numeric type, missing component, stale preview commit and expired
epoch. All snapshot assertions preserve complete flattened entity fields,
references and shared history on rejection. Same-key retry after undo is
replayed without reapplication.

Both modes save the edited engineering content, kill/restart the actual local
SQLite engine, explicitly recover with a changed epoch/same revision/identical
entities, reject old-epoch writes, then discard and normal-open the saved
.qcae into a new document/revision0 with identical entities. The original
`../force-vector-ipc-tests40.log` startup RED is preserved: a stale socket
path was mistaken for readiness after kill. The owner changed only the test
start helper (`tests/force_vector_ipc_tests.py:33–46`) to bounded actual
capabilities handshake, and the 40b run succeeds. No production retry or
commit fence was relaxed to obtain GREEN.

The independent private-probe/unit.log adds12 actual checks: N/kN and signed
zero share the canonical signature; cross-unit same-key retry and retry after
undo make no new revision; another caller cannot commit the token; preview
missing-unit/wrong-kind/stale requests reject; the canonical preview commits
with original force/node identities and exactly one revision/history cursor
advance. The probe uses the real existing fixture and library implementations,
not a second validator. Archives and command argv are SHA-bound. It neither
runs GUI/solver nor replaces common CTest.

Private fixture corrections are preserved separately: renaming the original
main required an explicit return0 in its temporary copy (compile.log); the
wire diagnostic prefix expectation was corrected from y.unit to input.y.unit
(unit-fixture-field-prefix-red.log); after undo, a new commit truncates redo,
so the assertion uses cursor/revision instead of total history row count
(unit-fixture-history-red.log). These reviewer fixture failures are not
product regressions. Final strict compile/link/run exit0.

Source and log SHAs bind this review checkpoint. Shared CMake's digest only
records the inspected file; the review covers the force test registration,
not unrelated ongoing build-baseline edits. No graphical preview/render
behavior, new SQLite image, external AI policy or whole copy budget claim is
made. All original failure evidence is retained.
