# C3 lean display refresh QG-02 review

2026-10-01. Independent source review of `ui/desktop/src/desktop.cpp`,
`adapters/engine_api/src/render_service.cpp`, `tests/desktop_selection_tests.cpp`
and `tests/render_service_tests.cpp`. This reviewer does not edit those product
or test files and does not run shared builds or GUI. Client/host/event/JSON helpers
are separately owned boundary references, not the final review subjects here.

## Review status

The normal lean refresh has explicit authority and version fences, and does not
introduce a second engineering model. All seven findings below are closed by
source reread of the display owner's final repairs. This scoped source is ready
for common validation; no remaining product blocker was found in the reviewed
paths. QG-02 closure still requires the repaired common-build GUI regression
evidence. The traces describe source before repair; this reviewer did not run
the GUI reproductions.

1. **Pending view update can lose the latest hidden intent.** Before repair,
   `applyDocumentInfo` called `updateView(hidden_ids_)` on a model revision change.
   If `view.update(H1)` was pending, `updateView` overwrote `queued_hidden_` with
   the old confirmed H0. An already queued user H2 was lost; without H2, the
   captured pending H1 was also displaced. The old-revision callback then chose
   this H0 through `queued_hidden_.value_or(hidden)` and sent H0 at the new model
   revision. The display owner confirmed this source path.
2. **Malformed fallback IDs can stamp an unchanged tree as current.** Before
   repair, rejected `changed_rows` fell through to
   `refreshChangedRows(data["changed_ids"].toArray())`. Missing/null/non-array
   IDs became an empty array, and the empty branch immediately called
   `treeRefreshCompleted()`. After valid R+1 display installation the tree still
   showed old labels, but `treeRevision=R+1` and the pipeline could report idle.
   Existing bad-row tests kept valid changed IDs, so they did not cover this.
3. **Late same-context create reply can replace a newer camera/view.** Before
   repair, `createView` callbacks checked only document/epoch/model revision.
   Recovery create A(C0) could be held while a camera change and old view conflict
   admitted create B(C1). B could install first, then A could replace its view ID
   and confirmed camera in the same model context. With both camera timers already
   expired, the old callback had no compensating camera update. The new generation
   and pending fences must reject A, retain latest camera/hidden intent and ensure
   the installed packet is followed by the necessary ordinary view update.
4. **Post-repair legacy installation must drain the same intent queue.** The
   owner's resource-path repair adds a final camera comparison and hidden queue
   drain after installation. On first reread, legacy `view.render_data` still
   installed its packet and evaluated selection without that drain. Since the
   camera timer now calls a pure refresh which waits during create, C1 could be
   lost after a held C0 create if the client had no resource capability. A user
   hidden intent queued during that create could remain pending indefinitely.
   This new regression was reported before build30; both installation paths
   now use the same post-install intent handling and a legacy negative.
5. **A failed legacy reply at the same model revision can strand display recovery.**
   Previously, a matched failed or malformed `view.render_data` reply only cleared
   pending state and returned. If the preceding camera/hidden update advanced V,
   the installed packet stayed at old V, while subsequent same-R polls saw no
   changed model or forced refresh. The repaired branch retains the old installed
   scene, marks it invalid/full, and schedules a generation/document/epoch-fenced
   authoritative poll. Unmatched late replies cannot clear a newer pending render.
6. **Legacy packet content must be checked before VTK installation.** The final
   reread found that point coordinates were checked only for array length, then
   `toDouble()` converted a string such as `"bad"` into 0. IDs also lacked a single
   uniqueness set across points, beams and geometry lines. A duplicate line ID
   makes VTK's `locations.emplace` fail and `installedVersion()` become absent,
   but desktop still stamped its rendered version and scene as valid. Both paths
   can publish a malformed scene. The bounded repair is strict finite numeric
   point checks and one cross-primitive identity set in `decodePacket`, followed
   by the existing recovery branch before any `setPacket` or row publication.
   The owner's same-function audit also identified header defaults accepting
   absent/wrong-type document or epoch, missing revision becoming 0 at R0, and
   non-boolean optional visibility. The final decoder rejects these while retaining
   valid legacy integral revision values and omitted visibility's true default;
   explicit boolean false is preserved for every primitive. Normal host packets
   already carry strict headers and finite point values. These checks all happen
   before `setPacket`, keeping old VTK state intact on rejection.
7. **Camera debounce must fence selection admission and publication.** Before
   the final repair, `viewportMatchesContext` checked installed model/view fields
   and request flags but not actual versus confirmed camera, either camera timer,
   or the hidden intent queue. Starting from idle, `standardView(C1)` changes the
   real camera immediately; a pick during debounce could send old V(C0), and a
   held selection reply from C0 could publish while C1 was already visible. This
   violates AR-07's explicit camera/view consistency. The final shared predicate
   checks actual versus confirmed camera, both real timers and the hidden queue
   for pick admission, pending selection evaluation and both selection response
   publications. Stable-ID selection intent is reevaluated only after the ordinary
   C1 update and installation, without changing model authority.

Minimum reproductions for the owner's fake:

| Case | Request/response order | Required observation |
| --- | --- | --- |
| H0/H1/H2 | Idle H0 → hold update(H1) → user queues H2 → legal DocumentChanged R+1 → release old update | Next new-R update uses H2. Without H2 it uses H1. Cover old request executed before delayed success and old request rejected after the model changed. |
| Missing IDs | Valid lean R+1 display, `refresh_tree=false`, invalid/incomplete rows and missing/non-array `changed_ids` | Do not stamp the old tree current. Reload authoritative rows; old tree stays stale until that query finishes. |
| Reordered create | Hold recovery A(C0), change to C1, admit B(C1), install B, release A | A cannot publish view/pending/camera state or rows. Current camera and hidden intent remain confirmed after complete installation. |
| Camera during create | Hold one create, change camera, let both real debounce timers expire, release create | Complete installation triggers normal update for the latest fingerprint; no false idle or selection admission while camera context is stale. |
| Legacy installation | Repeat held-create camera and queued-hidden cases with resource capability absent | `view.render_data` finishes installation before sending the latest ordinary update, and drains the same queue as the resource path. |
| Legacy same-R failure | Advance the view through an ordinary same-R update, then return failed/missing-version render reply and hold its retry | Old installed V stays unchanged; selection and idle remain blocked; release of the valid retry installs the new V without a model write. |
| Legacy malformed content | Return a matching-version packet with a string coordinate or duplicate ID | No VTK replacement or valid-version stamp; the same fenced recovery is used. |
| Camera selection admission | From idle orient to C1, then pick before debounce sends its ordinary update | No `selection.evaluate` at old V(C0). After C1's update and installation, a fresh pick can be evaluated. |
| Camera selection publication | Hold C0 selection response, orient to C1, then release it during debounce | Old camera's reply cannot replace selected IDs or highlights; a new explicit selection after C1 synchronization works. |

The source reread confirms `requestViewRefresh` is a pure refresh while a view or
render request is pending; it does not manufacture an H0 hidden queue. User
`updateView` calls retain the latest H2. An old-model success learns only a real
same-view returned V or V+1, then sends H2 or captured H1 at the new R; an old-model
failure recreates with that intent. `createView` now shares the view generation
and pending fence, so same-context A/B admission is serialized. Replacement or
cleared documents invalidate old generations. A camera changed while create is
held remains the actual viewport camera and is compared after installation.

Both resource and legacy installation now call the same `drainViewIntents` after
their complete packet/delta installation. Legacy requests also set and clear
their own matched render pending state. Missing/invalid fallback IDs reload the
authoritative owners/tree, while a legitimate explicit empty array is accepted.
These helpers keep user intent, display admission and model authority in their
existing owners instead of adding a second scene or engineering model.

Final repair locations are `decodePacket` at `desktop.cpp:324`,
`drainViewIntents` at `desktop.cpp:1736`, the legacy render recovery/installation
at `desktop.cpp:1985`, and `viewportMatchesContext` plus both selection response
gates at `desktop.cpp:2161`. `RenderService::dispatch` remains the authoritative
snapshot/rebase/row adapter. Named helpers centralize these rules without a new
core dependency or duplicate state owner.

## Authority, fences and installation

`RenderService::dispatch` takes one `RecordSnapshot` from the authoritative
application for the requested document/epoch, checks expected model revision and
uses the same snapshot for projection and row DTOs. Client view and tree data are
discardable projections. Rebase and row selection are explicit boolean opts;
render wire version 3 does not implicitly opt into them. The host capability
advertisement and client negotiation still need their separate stable review.

The desktop summary fast path requires the same document and epoch, an exact
`base_revision` equal to known R, a bounded complete 11-field summary whose
identity/version equal the event, explicit `resync_required=false`, and numeric
next revision exactly R+1 without overflow. Missing/malformed summaries, jumps,
wrong base/document/epoch and resync use authoritative `project.current`.
Event arrival fences old display, selection and current-query generations before
the replacement summary/query is applied. A previously pending current response
cannot overwrite the accepted event version.

Rebase admission additionally requires `pipelineIdle()`, supported resources,
an installed matching packet and tree, no current/selection/view/render work,
no camera debounce, no queued hidden intent, bounded view revision V, and an
actual camera fingerprint equal to the confirmed stored camera. The event
handler computes this eligibility before invalidating the old scene.

On the engine, opt-in rebase uses `inspect_view`, which still checks trusted
caller/document/epoch. Before mutation it requires stored model revision B,
target R>B, both base fields, stored view revision equal to expected V and base V,
and V below its maximum. It then reuses `update_view` with the stored hidden IDs
and camera, requires model R and view exactly V+1, and verifies those states did
not change. The real projector baseline decides delta/full; a client's claimed
base cannot authorize a different installed projector baseline. Display metadata
does not commit or change model history. Old selection page/combine handles are
rejected after the V+1 update.

The desktop predicts only V+1 for a rebase target. Before admission, the callback
must still match generation/document/epoch/R/view and original V. After a valid
manifest or acknowledgement accepts that target, matching changes to V+1. This
avoids rejecting the client's own resource byte callback with its captured old V.
Old response manifests are released without clearing a newer pending state.

`finishRenderResource` is reached only after successful acknowledgement delta
application or complete resource fetch, media/encoding check, packet/delta decode,
exact target-version check and VTK installation. Only then are `rendered_version`,
scene validity and changed rows published. Inline acknowledgement and binary
delta still check their original installed base at VTK application. Bad target
versions on rebase recreate a disposable view; transfer/decode failure preserves
old installed scene and requires a full retry. The repaired creation path must
keep the original generation/queue protections described above.

Rows carry a five-field exact version, `rows_complete=true`, matching unique
stable changed IDs, bounded count and a 64KiB conservative recursive byte budget.
Coordinate and source number fields are checked. Shared `applyRows` updates the
existing tree nodes and owner rows, preserving identity and ownership filters.
Topology/reference changes, full projection, non-query record identities or
incomplete rows force authoritative tree queries. The new fallback-ID validation
must preserve a legitimate empty set while rejecting missing metadata.

## Existing test protection and remaining tests

The scoped tests cover negotiated and legacy paths, nonempty and inline
empty delta, summary faults (version/document/epoch/name/count/size/base/jump/
missing/resync), row completeness/version/field/coordinate/identity/size/duplicate
faults, wrong rebase target recreation, held rebase with real camera timers and
queued hidden intent, old current reply, document replacement, timeouts, event
gaps, old packet selection and selection response reordering. The engine test
checks wrong opt types/base/view/owner before mutation, exact V+1 with preserved
camera/hidden state, old selection rejection, repeated stale rebase refusal,
row count/byte fallback and unchanged model history.

The owner added four H1/H2 success/conflict rows, thirteen bad-row/ID shapes with
held fallback queries, four held-create camera/hidden rows across resource and
legacy capabilities, and eight same-R legacy failure/recovery rows. Their assertions
retain old installed/tree versions until the valid replacement completes and
block selection/idle during the transition. They are source-reviewed regression
definitions, awaiting the common GUI run. The eight legacy faults include failed,
missing-view-version, malformed coordinates, same-kind and cross-kind duplicate
IDs, non-boolean visibility, missing document and missing revision at R0. A positive
legacy numeric-version test protects compatibility. One held-selection-get camera
test protects both fresh-pick admission during debounce and old response refusal,
then successful stable-ID reevaluation at the new view version. Useful additional engine negatives are old epoch,
deleted hidden entity, view V at its maximum, actual projector baseline mismatch,
and a post-rebase projection/encoding/resource failure followed by recovery.
They must retain the distinction between a committed model and disposable view
metadata: a failed display operation cannot roll back the engineering edit.

## Actual ledger and log shape limits

The archived `ledger-lean-build29-diagnostic.json` contains 20 N=1000 samples
(10 node and 10 material). Both endpoint frame traces are marked complete. The
node sequence is edit → DocumentChanged/HistoryChanged → render → read/release;
material is edit → the same events → render, with no read/release. There are no
ordinary `project.current`, `view.update` or `entity.query` frames in those
samples. Material render encoding and resource publication counters are zero,
and GPU upload arguments are zero. These are archived observations, not a new run.

The acceptance numerator is `contract_copy_bytes`, including H: node
237469–248601 bytes and material 94170–97532 bytes. Every material sample still
violates the frozen 69632-byte limit. `measured_copy_bytes` is the subset excluding
H (node 202472–213712, material 81620–83706) and cannot replace that acceptance
numerator. Metadata is 34433–35901 and 12550–14002 bytes respectively. The report explicitly retains
`whole_pipeline_owned_copy_coverage=unknown` and `complete_sk12_passed=false`.
Per-sample counter presence cannot certify whole Qt/VTK owned buffers or C4.

The desktop log remains active: every HistoryChanged appends a real message and
`appendLog` reads the real document's empty state and character counts before and
after `appendPlainText`, including warm-up and failures. The current log has no
maximum block count or clearing operation. Thus conservative piece-table prefix
cost depends on accumulated log length, not only this edit's message length.
The data owner confirms the old build29 binary had not yet connected the append
tracker: its socket counter cannot prove the new real log append cost. New text
and ViewSession counters must be cited from a later unified diagnostic build.
`PlainTextAppendCopies` is a boundary reference owned by the data agent. Its
recorded independent Qt 6.11.1 offscreen probe uses real QPlainTextEdit/QTextDocument
and passed warm-up, nonempty/empty/4096-character append shapes, clear invalidation
and the multiline unknown guard. The evidence and scope are in
`c3-closure-evidence/qt-text-source-audit.json`; this reviewer did not run that
probe. It observes public runtime shape and the guarded piece-table upper bound,
not font/layout/paint invocation counts or whole SDK copies. Unsupported shapes
remain sticky unknown and whole-pipeline coverage remains unknown. The unified
build30 diagnostic still must demonstrate the connected production log counters.

Archive status is preserved: `lean-targeted-build29.log` includes an initial
render-service test failure; `render-service-signature-ctest.log` records its
later pass. Those logs do not certify the newly identified client repairs.
This reviewer read
[`lean-bad-packet-red-build30.log`](c3-closure-evidence/package/lean-bad-packet-red-build30.log):
the test-only bad-coordinate and duplicate-ID rows both actually failed against
the unrepaired build30 UI because no recovery render was requested (33.097 s;
two substantive failures, setup/cleanup passed). This records the defect before
the decoder repair, not a repaired pass. The final stable source diff is reviewed;
final QG-02 status awaits its new common-build regression evidence. This review
does not mark C3, SK-12, C4 or full P0 complete.
