# Core/MCP contract independent review

Reviewer: `/root/lean_refresh_review`, who did not implement these Core/MCP changes.
Scope: indexed owned facts, revision-fenced auxiliary CAS, engine read/intrinsic
metadata, and stdlib MCP metadata projection. No repository source, tests,
CMake, shared build, external configuration, or GUI was edited/run by this review.
Local probes used isolated `/private/tmp` workspaces and no external API/solver.

## Current conclusion

QG-02: the source snapshot in reviewed-source-sha256.json has no remaining blocking finding in this scope.
The final MCP fixes close the actual malformed-metadata failure and catalog
publication gaps found during review. QG-01 has the owner's actual 226-file
clang-format log. QG-03: the Core Release/ASan runs and four MCP/typedHost targets have actual
passing receipts, including the expanded 14-case malformed-peer regression.
The shared 39d log also contains a separate IPC fixture failure, described below;
its corrected rerun passes. Independent final probes supplement these receipts.

## Core authority, cost and fences

- `modules/application/include/qcae/record_application.hpp:83` exposes only
  standard C++ borrowed immutable row references and an overflow flag. It adds
  no Qt/SQLite/MCP type or second application state.
- `modules/application/src/core.cpp:737` takes the existing state mutex,
  checks active document/epoch, rejects empty/NUL/over-256-byte prefixes and
  limits outside 1..32. `:754` seeks the existing ordered map with lower_bound,
  stops at the first other space/prefix, returns at most limit pointers, and
  inspects one further matching row for overflow. Cost is O(log N + limit)
  bounded key/pointer work; payload shared_ptr identity is preserved. Owners
  are deliberately not filtered here: module validators check returned owners
  without making the indexed scan skip an unbounded set of other-owner rows.
- `core.cpp:683` keeps the old DocumentRef entry point. `:688` supplies the
  new optional revision to the same private path. `:697` holds the same mutex
  through caller/document/epoch/revision admission, candidate CAS, persist and
  RAM swap. There is no unlocked admission-to-CAS interval. A Revision value
  of zero is still checked because this is optional presence, not integer truth.
- `core.cpp:702` rejects stale expected_revision before copying/persisting.
  `application_state.cpp:742` retains exact prior-payload/absence CAS; `:746`
  retains key/owner immutability and payload validation. Definite persistence
  rollback and unknown acknowledgement keep the original Core error semantics.
  Candidate writes still clone the bounded owned-row map and perform storage
  work under the mutex; this is not a constant-time write or measured latency
  claim. The row count/byte quotas remain the existing application limits.
- The old DocumentRef task publisher still has no new model-revision condition,
  so background immutable execution facts can finish after physical edits under
  their documented original-input context. Both paths keep caller validity,
  active epoch, save-intent and owned-row CAS constraints. Trusted modules still
  validate principal/owner linkage; nonblank Caller is not a new authorization.

Actual tests: prefix read Release 1/1 (0.67s), ASan 1/1 (1.96s);
fenced write Release 1/1 (0.49s), ASan 1/1 (2.24s). See repository package logs
`owned-prefix-read-tests39.log`, `owned-prefix-read-sanitizers-tests39.log`,
`owned-write-fence-tests39b.log`, `owned-write-fence-sanitizers-tests39b.log`.
The initial fixture signature failure in `owned-write-fence-tests39.log` remains
recorded; it was corrected to the actual normalized signature, not suppressed.
`tests/record_application_tests.cpp:422` seeds 82 rows across spaces/owners and
checks 32+overflow, exact/empty prefix, shared payload pointers, invalid limits,
NUL/long prefix and epoch. `:490` checks zero/current/stale revisions, no
persistence on rejection, caller/epoch, exact CAS, and legacy task behavior.

## Engine metadata and actual dispatch

- `adapters/engine_api/src/ipc_api.cpp:43` describes only the explicitly known
  legacy contracts. `entity.query` names match `ipc_model.cpp:326` exactly:
  kind/name_contains/ids/view/owner_id/offset/limit, with integer offset 0..100000
  and limit 0..1000 matching `:315`. Missing schema means undescribed legacy
  metadata, not an invented empty-input contract.
- `ipc_api.cpp:59` entity.references requires entity_id and restricts direction
  to incoming/outgoing, matching `ipc_model.cpp:354`. Empty model.summary and
  project.status match `ipc_api.cpp:343`. `ipc_api.cpp:211` rejects non-object
  parameters before typed or legacy dispatch.
- `typed_host.cpp:298` gives the four intrinsic operations real fields:
  entity.fields/entity_id, task.status and task.cancel/task_id, task.reconcile
  explicit closed empty parameters. Their branches `:340`, `:351`, `:379`
  require active doc/epoch and exact parameters; task query/cancel uses trusted
  caller and task/document linkage. Requested contract version remains checked
  at `:333`. No request actor or approval value acquires authority.
- The units workflow names the existing analysis.check operation; operation
  fields and quantity unit choices still come from the actual registered
  descriptors rather than MCP business rules.

Actual owner MCP logs: `mcp-contracts-tests39.log` is 4/4 PASS, 3.40s for
mcp_bridge, mcp_protocol, mcp_bridge_durable and typed_host. Additional independent
real engine probe used frozen `build-c3-contracts-release39/qcae-engine`, its own
socket/SQLite DB, and checked 16 boundary cases (zero/max pagination, excessive
offset/limit, fractional/bool pagination, invalid ID type/kind, unknown query
field, invalid reference direction, unknown summary/status/intrinsic fields and
non-object task.reconcile). All expected statuses passed; model revision and
engine SHA256 were unchanged. Evidence: `actual-engine/observations.json`,
`actual-engine/transcript.json`, `engine-contract-probe.log` in this review folder.

## MCP findings, fixes and independent evidence

1. Actual reviewed bridge source failed on a 20000-level engine JSON metadata
   subtree (144001 bytes, below 1MiB): no tools/list response, bridge exit 1 with
   RecursionError, one capabilities.list request, no business operation or retry.
   Python 3.14 successfully handled 1200/5000/12000 levels, so the earlier
   1200-layer PASS was preserved rather than called RED. Exact reviewed source
   and SHA256 are `bridge-reviewed-source.py` and `.sha256`; failure is in
   `metadata-probe-red.log` / `deep-engine-json.json`.
2. One-level parameters_schema checking accepted inner `type: []`; a string
   requires_document flag was treated as truthy. Their original returned tool
   contracts are preserved in `inner-type-array.json` and
   `invalid-context-flag.json`.
3. Interim schema validation rejected tools/list but had already installed the
   malformed catalog: a subsequent tools/call forwarded probe.read. Actual
   packet observation is `cached-bad-schema.json`; the fake peer has no business
   model. The large-integer numeric bound concern was already fixed when the
   interim probe ran: `huge-minimum.json` is GREEN, not invented failure evidence.
4. Actual normal solver.configuration declares typed fields=[], but the old
   bridge left its parameters object open. Actual frozen metadata and old bridge
   projection are in `empty-typed-parameters.json`.

Final source checks: `apps/mcp/bridge.py:98` maps engine RecursionError to
TransportError. `:134` checks only a bounded metadata subset (depth 16, nodes
128, width 128), required/property/items/type/enum/size/numeric shapes; large
integers use explicit float_info bounds rather than overflow-prone isfinite
conversion. `:185` distinguishes explicit fields=[] from absent legacy metadata,
closing empty typed inputs without guessing unknown legacy fields. `:233`
checks context flags as booleans. `:246` builds a candidate, validates every
tool_schema and complete-entry serialization, then publishes catalog once at
`:257`. Failed discovery cannot install a partial catalog. Existing business
payloads are still forwarded unchanged; all physical rules, unknown argument
errors, transactions, profile/revision checks and diagnostics stay in engine.

Independent final probes in `green/` assert all five abnormal cases return
-32000, ping then succeeds, bridge exits 0 with no stderr, and no business call
is sent. Rejected catalog subsequent call performs only a second discovery.
Actual empty typed schema is now closed; completely undescribed legacy remains
the original open fallback. Fixed source snapshot SHA is retained. These GREEN
results are in `green/metadata-probe-green.log`,
`green/metadata-followup-green.log`, `green/empty-typed-parameters.json`.

## Final common receipts and source binding

`../mcp-metadata-ipc-tests39d.log` records four passing targets: mcp_bridge
(0.16s), mcp_protocol (2.04s), mcp_bridge_durable (0.16s), and typed_host
(0.02s). The protocol source contains fourteen scenarios, including deep JSON,
inner type arrays, schema depth/width, large integers, non-boolean context,
lost reply, and bad status; failed discovery is followed by tools/call and ping.
The same CTest invocation is **4/5, not 5/5**: its new unavailable-solver IPC
fixture omitted the required expected_profile context and correctly received
MISSING_INPUT before dispatch. The failure remains recorded. The fixture now
reads the real Nastran ProfileRef from capabilities and passes it unchanged
(`tests/ipc_tests.py:227`); `../ipc-unavailable-tests39e.log` records IPC 1/1
PASS (1.08s, total 1.09s), including unavailable rejection without model change.

QG-01 receipt is `../format-build39c-interim.log`: 226 C++ files match
clang-format 21. Core QG-03 receipts are the four exact prefix/fence logs listed
above. This receipt closes the scoped QG-01/02/03 Core/MCP review, while retaining
the earlier RED evidence and the common invocation's distinct fixture failure.

The SHA file preserves the reviewed snapshot rather than replacing historical
digests with later source. At archiving, seven of its eight current paths still
match; typed_host.cpp has since changed for the separately owned actual-JSON
size helper. That addition and the solver numerical-history JSON quota bug are
outside this Core/MCP metadata review. Their later review and tests must bind to
their own source checkpoint. No runtime database, socket, build artifact, or
credential file is included in this evidence directory.

## Remaining scope limits

The metadata validator intentionally supports the currently emitted structural
subset; it is not a full JSON Schema implementation, a physical rule engine,
or an authentication layer. Future schema keyword/shape changes must update its
explicit contract and tests. Reads borrow immutable row snapshots; callers must
not treat them as authorization or automatically current facts. Source cost
analysis is not an end-to-end latency/large-model benchmark. This review provides
no GUI, real solver, paid AI API, full SDK, or complete P0 acceptance claim.
