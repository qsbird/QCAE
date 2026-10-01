# C3 domain readability review (QG-02)

Status: 2026-09-30 source review and bounded repairs ready for common-build review. This document
does not certify C3/C4, performance, P0, solver execution or numerical validity.
The integration source is mutable; archived test logs describe their recorded
builds, not every current source change. No shared build or GUI is run by this
reviewer.

## Bounded repair plan and behavior protection

The review identified two concrete contract gaps. Before the common baseline is
frozen, repair only `adapters/engine_api/src/nastran_contribution.cpp`,
`profiles/nastran/src/nastran_package.cpp`, `tests/nastran_artifact_ipc_tests.py`
and `tests/nastran_package_tests.cpp`. Preserve all six genuine legacy fixtures
byte for byte. Do not change public write permissions, introduce dependencies,
run shared builds, or extend the ongoing lean refresh work.

1. Introduce one local artifact/task consistency check, reused before publication,
   export replay, reconcile (including replay), published artifact lookup and
   frozen input resolution. Require the stored manifest to equal the existing
   canonical constructor's output from frozen input and file map. Compare the
   artifact's original task ID, principal, operation, document ID/epoch/revision
   and exact profile with the persisted task. A succeeded task must carry the
   exact artifact ID, input revision and SHA256 of the stored final manifest;
   the published database flag and succeeded state must agree.
   Reconcile must reject mismatches before any owned-row update or success event.
   Recovery compares both records' original input epochs, not the new active
   document epoch. Files must still pass the existing exact manifest/digest check.
2. Restrict migration to the real frozen 0.1.0 profile definition and the installed
   0.2.0 profile. The historical identity is
   `qcae.nastran.linear-static / 0.1.0 /
   sha256:bb856be32336514d1aebf67b524ed0403664291cb4ba9e473e922ae7bb71b662`.
   It is recorded in `tests/fixtures/legacy/manifest.json` and all six golden
   fixture expectations; the old generator's eight logical files at `9e0a56b`
   independently reproduce that digest. An arbitrary matching, nonempty digest
   is not a supported old definition. Existing schema decoding and full candidate
   validation remain mandatory after identity validation.
3. Protect normal export/replay/reconcile/recovery, file corruption rejection,
   unchanged model revision and event counts. Add carried-file task field damage
   samples through ordinary project open, rather than a production row write
   endpoint. Rejected reconcile must leave stored facts and task events unchanged.
   Protect physical fields, source numbering, dirty migrated state, new save path,
   replay and source-file hash; reject an invented digest even when the stored
   bindings and caller's migration claim agree.

## Entry, authoritative state and effects

| Slice | Entry and authoritative state | External effect and boundary |
| --- | --- | --- |
| Analysis edits | Typed operation inputs → analysis operations → `EditSession` → `RecordApplication::execute` | One validated model transaction and bounded history; GUI/CLI use the same services. Force values normalize to canonical units. |
| Completeness checks | `CheckService::run` snapshots the document, applies the two-rule catalog and stores caller-scoped report facts | Auxiliary owned rows only; model revision/history do not advance. Reports retain original version and rule catalog. |
| Nastran export | Installed-profile validation → immutable artifact plan and frozen physical input → runtime task → artifact coordinator | Durable intent/task admission, local staging, independent semantic readback, final manifest and atomic database completion facts. No solver process. |
| Fixture results | `result.read_fixture` → caller-scoped published artifact resolver → frozen-input-aware fixture reader → result service | Explicit fixture facts only. Metadata, physical fingerprint and solver-number map are validated; caller does not supply authoritative frozen input. |
| Profile migration | Explicit migrate operation → protected project read → old identity check and `EditSession` → `open_migrated_document` | Validated new active document, new ID/epoch, dirty state and no source save path; the source file is preserved. |
| Line geometry | `geometry.evaluate_line` → current immutable document view → `evaluate_line` | Pure mm coordinate interpolation, finite normalized parameter in [0,1]; response carries actual model revision. |
| Line mesh regeneration | `mesh.regenerate_line` → captured immutable view → runtime task → prepared record change → application publisher | Exactly one model transaction and actual change receipt; expected revision, cancellation and history use the existing application path. |

The application document is the model authority. Artifact/task/check/result rows
are bounded facts with registered owner/schema validators, not a second mutable
engineering model. Typed request parsing rejects undeclared parameters, and the
host supplies the caller. No public JSON operation accepts a task completion
receipt, raw owned-row payload or replacement authoritative frozen input.

## Required invariants and error behavior

Analysis checks are deliberately limited to missing load and missing constraint
for the controlled linear-static workflow. Structural references and profile
rules are separate validation. Check replay requires identical original request
semantics; new checks require the current expected revision. A report is current
only with the same record version, target binding and rule catalog; older reports
remain retained as stale evidence. This is not an engineering approval.

The export input includes physical records, selected load case/force/constraint,
exact profile, unit convention and a typed stable-ID-to-solver-number map. The
canonical signature excludes storage order and organization metadata. SHA256 in
the final manifest covers that exact signature. Independent import/re-export
checks engineering cards and numbering after the staged files are written.
Filesystem staging and database completion are two durability domains, not one
cross-resource transaction: the final manifest is the publication marker;
recovery retains interrupted work and explicit reconcile verifies the files
before completing the atomic task/artifact/reconcile fact batch.

The runtime task codec already requires exactly one receipt for success. A model
receipt must advance revision and name the transaction. An artifact receipt must
name an artifact, retain the task input revision and contain a lowercase SHA256.
Those structural checks alone cannot establish that independently carried task
and artifact rows describe the same operation. The new shared consistency check
must establish that relationship before publication is treated as authoritative.
It must preserve idempotent reconcile without appending a duplicate success event.
Replaying a retained reconcile fact additionally requires a completed published
artifact, while current file verification remains distinct from historical
verification. Export task replay validates the linked artifact facts without
rewriting the original completion or re-running the export.

Fixture data are user-provided numerical samples. The reader requires
`source_kind=fixture`, fixed reader version, displacement/XYZ/mm/node/global,
the supported case/frame, finite values, the exact frozen fingerprint and exact
namespace/number map. These checks establish association and schema, not whether
the numbers came from a solver or are correct. Result applicability compares the
current physical signature with frozen input: physical changes stale the result;
organization/display changes do not; undo can restore applicability. Original
document/epoch/revision provenance remains available. Neither successful export,
reconcile, fixture read nor task success may be labeled real solver execution.

Migration must resolve a known old semantic definition, not just equal strings
chosen by the importer. `solver-profiles.md` requires an explicit migration with
field/semantic changes identified. The current narrow migration changes profile
bindings in `AnalysisDefinition.target.profile` and `SourceIdentifier.profile`
while preserving physical fields and source numbering. This is the migration's
field/semantic change description; the existing operation response still returns
document identity, revision and dirty state rather than a new report DTO. Unsupported old
definitions or record fields fail before activation; ordinary open still refuses
an unsupported old profile. The old source cannot become an implicit save target.

Line regeneration validates a geometry-owned uniform connected chain and rejects
ambiguous/disconnected/cyclic topology. With equal segment count it retains node
and beam IDs, connectivity and section assignment while rebinding geometry
revision. Replacement cannot invent mappings for external references: full
candidate validation rejects the whole transaction when references remain
unmapped. Moving an endpoint marks linked meshes stale; regeneration clears that
state only for the evaluated current geometry. Cancel before commit publishes no
model; cancellation after commit cannot claim rollback. Stable IDs, solver
numbers, storage positions and render identifiers remain distinct.

## Readability and verification assessment

The domain helpers expose their scope through small typed functions: canonical
force normalization, two-rule check evaluation, frozen input codec, fixture
reader, uniform-chain regeneration and Nastran semantic readback. The application
and runtime retain the transaction/receipt responsibilities. The local artifact
adapter owns exclusive creation, safe relative paths, quotas, file SHA256 and
flush/rename sequencing; Qt JSON remains in the engine adapter. No core public
interface acquires Qt, VTK or SQLite types.

The coordinator currently combines manifest construction, pending admission,
task publishing, lookups and operation registration in one file. Splitting it
now would expand this repair's scope. A single named artifact/task consistency
helper makes the missing invariant explicit and prevents lookup/reconcile/result
paths from silently diverging. This is the smallest maintainable repair.

Existing source tests cover completeness/staleness/replay, fixture provenance and
metadata rejection, task receipt structure, local publication failure boundaries,
semantic readback, export/reconcile/recovery, migration/save/recovery and line
regeneration's retained-ID/replacement/reference cases. The bounded additions
must exercise genuine persisted task encoding with mismatched principal,
operation, original version/profile and receipt fields, as well as a fully
matching but unsupported old profile digest. Source review is not test execution.

## Completed repair and static evidence

`nastran_contribution.cpp::validate_artifact_task` now checks the canonical
manifest, linked task identity/principal/operation, original document/epoch/
revision/profile, success/publication agreement and exact receipt. `find_task`
reuses it for lookup, fixture input resolution and reconcile. Admission and
publication use the same check, and publication additionally compares persisted
intent with the in-memory admitted frozen intent before filesystem publication.
All mismatch paths return or throw before `update_owned_rows`; reconcile's
proposed success event and flag changes are local values until the existing
atomic compare-and-swap batch commits. This source audit does not claim that the
new damage samples have already run.

`migrate_nastran_profile` now rejects any source other than the genuine frozen
0.1.0 triple and any destination other than the actual installed 0.2.0 codec.
The existing analysis-kind/reference/candidate checks remain. Package tests use
the genuine historical digest, reject an invented digest that matches every
stored binding and input claim, and reject an unavailable destination digest.

The artifact IPC test adds 11 structurally valid carried-project damage cases:
principal, operation, original document ID, epoch, revision, each of the three
profile strings, receipt artifact ID, receipt manifest SHA256, and a coordinated
task/receipt/frozen revision change that preserves cross-row agreement but
contradicts the final manifest. It uses an ordinary saved `.qcae` SQLite backup
and ordinary project open. It checks rejection by get, fixture input resolution
and two new reconcile keys, byte equality of every task/artifact owned fact
(therefore their events), and unchanged model revision. The unchanged carried
project is a positive control for original epoch comparison and explicit fixture
labeling. No production raw-row write endpoint or permission change was added.

Recorded static checks for this repair:

- Python syntax compilation of `nastran_artifact_ipc_tests.py`: pass, without
  executing the engine or writing bytecode.
- Clang-format 21 dry-run on the three owned C++ files: pass.
- `python3 tools/check_design.py`: pass (162 links, 185 JSON files, 32 modules,
  41 operation descriptors). This is a design check only.
- `git diff --check` and explicit no-index whitespace checks for all five owned
  new files: pass.
- All six legacy binary fixture hashes and their expectation/transcript hashes:
  equal the frozen manifest. No golden fixture was rewritten.
- The global `python3 tools/check_cpp_format.py` read found concurrent formatting
  violations in `tests/desktop_selection_tests.cpp:284–307`; this reviewer did
  not edit that other owner's file. The common gate must be repeated after the
  other change finishes.

## Common-build execution after the scoped review

The root integrator built the repaired sources in the Release desktop tree.
`package/package-profile-hardening-tests.log` records the package test passing
1/1. `package/lean-targeted-build29.log` records `nastran_package`,
`nastran_artifact_ipc` and `result_fixture_ipc` passing, including all 11 carried
project damage cases above. These are actual engine/SQLite/IPC tests; the fixture
reader remains explicitly labeled fixture. The same targeted run passed 10/11
tests: its sole failure was an unrelated render-service test's prepared signature
disagreeing with the input signature. That test fixture was corrected and the
separate `package/render-service-signature-ctest.log` records its passing rerun.
The initial failure log is retained.

These logs are under `docs/engineering/c3-closure-evidence/` and describe mutable
build29 sources. They establish that the scoped contract damage tests have run;
they do not establish a frozen final Release or the performance/C4 gates. QG-02
still requires review of the final integrated diff; no full C3/C4 completion is
claimed here.
