# Property replies during an authoritative context refresh

This bounded REQ-15/TST-I04 GUI fix changes only
`ui/desktop/src/desktop.cpp` and `tests/c3_desktop_workflow_tests.cpp`.

## Plan and original failures

A DocumentChanged fallback or event gap must immediately invalidate the property
preview and clear cached editable values. It must not wait for project.current
to return before rejecting older property replies. Responses must establish the
requested revision, and any supplied document/epoch metadata must agree.

`extracted-predicate-red.log` is the independent reviewer's original QtCore-only
source-derived predicate probe. It is not a native GUI test. Its original source
binding is retained separately.

`native-held-current-red.log` is the subsequent real DesktopWindow/production
engine/QLocalSocket proxy test of the old product. All three negative cases failed
as expected in 10.654 seconds:

- An old entity.query reply repopulated the editable −1 N property.
- An old manual force preview published a ready preview token.
- An old automatic force preview issued one changes.commit request. The proxy
  held that request, so no physical write occurred in the backend.

The injected invalidation frames are controlled transport faults. The probe
does not claim that a stale commit defeated the engine's model/version checks,
nor that the controlled frames originated from the production event producer.
All ordinary requests and replies are forwarded to the actual production
engine, while selected project.current/property replies are held.

## Product change and QG-02 review

`invalidateModelContext` centralizes the existing scene/selection invalidation,
sets the authoritative-refresh flag, and invalidates/clears property previews
and fields immediately. DocumentChanged fallback and eventGap use that same
boundary. Advancing event generation also prevents a query begun before the gap
from clearing the invalidation state.

`loadSelectedProperty` captures its property generation and rejects a reply
while refresh/scene invalidation is pending, after selection/version/generation
changed, or when its actual revision/identity metadata is inconsistent. Manual
and automatic previews use the same response fence. New property requests and
Apply are blocked during the refresh. Current legitimate error replies still
use the original operation error logging; stale or mismatched success replies
never publish a ready token or trigger commit.

`propertyReplyMatches` requires data.revision, which both current entity.query
and preview producers supply. Optional envelope revision and document/epoch
metadata must agree. Canonical string revisions are compared exactly. Older
JSON numeric revision metadata is accepted only when finite, nonnegative,
integral and at most 2^53−1, and exactly equal to the requested revision. Missing,
contradictory or unprovable metadata fails closed; no value is truncated.

The engine remains the authoritative model and commit/history coordinator. The
fix adds UI admission rules, no model or business validator, and no dependency.

## Actual verification and source boundaries

`native-property-matrix-green.log`: 43 real wire/widget cases passed in
150.889 seconds (45 QtTest entries including init/cleanup):

- Held row, manual preview, automatic preview and an already ready token, each
  followed by revision invalidation, missing summary, foreign document, foreign
  epoch or an actual eventGap wire frame, while project.current remains held.
- Row/manual/automatic replies with changed data/envelope revision, absent
  revisions, fractional/negative revision or contradictory document/epoch.
- Exact older numeric revision metadata for a valid row and manual preview.

Every case verifies zero commit requests and that the actual backend document,
revision, complete model fields and history remain unchanged. Per-case wire
frames, held responses, widget values and screenshots are in `green/`.

`native-force-positive-green.log` preserves the initial successful normal
preview/cancel/one-transaction edit/GUI undo/process-kill/recovery run. Its actual
screenshot shows the full wrapped Node identity and global `(0,-2,0) N`. That run
also exposed a QLabel heightForWidth value of −1 for empty text, which produced
a minimum-size warning. The final presentation-only refinement clamps the
minimum height to zero. `native-force-clamped-green.log` is the fresh strict-build
force rerun after that guard; the earlier warning log remains unchanged.

The 43-case matrix keeps its original source binding in
`native-matrix-source-binding.json`. It is not relabelled as a run of the later
height clamp. The final source SHA and positive rerun are separately bound in
`receipt.json`; the root's subsequent shared build/matrix covers that final SHA.
These are private fresh owned frontend TU/moc compilations and private adapter
links to the root-built production engine, not the shared frontend Release
target. The existing session5/6/7 AI unit/camera evidence retains its original
source SHA and is not relabelled by this fix. Full M5/solver acceptance is not
established.
