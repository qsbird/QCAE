# Actual AI snapshots in the native desktop

This is a bounded TST-I01/TST-I02/TST-I10 GUI slice. The original models are the
actual Codex/MCP session5, session6 and session7 portable snapshots, each created
and saved by the real AI unit workflow. The source snapshot files remain local
under `build-c3-ai-units-snapshots/`; their SHA-256 values and each independent
GUI invocation are recorded in `receipt.json`. No solver or full M5 acceptance
is established here.

## Plan and ownership

Only `ui/desktop/src/desktop.cpp` and
`tests/c3_desktop_workflow_tests.cpp` changed in this slice. The UI reuses the
actual `entity.query` row to display section area, I1, I2 and J in mm²/mm⁴ and
the nodal force in global N. It retains exactly four existing editors. Selecting
a force gives the existing XYZ editors N captions; nodes keep mm and materials
keep MPa. Other kinds have cleared, disabled XYZ editors.

The force preview uses the root-owned typed `force.preview_vector` operation,
then the existing `changes.commit` path. Numeric text parsing is mechanical UI
conversion; the engine owns unit, entity and physical validation. No new GUI
model, business rule, write coordinator or persistence path was introduced.

## Preserved failures

- `old-ui-red.log`: the old desktop actually normal-opened session5 and passed
  source-model/unit queries, but had no `propertyDetails` widget for section or
  force. Raw model/backend JSON and the old screenshots remain in `red/`.
- `test-action-setup-red.log`: the test first searched for `Open project`
  instead of the actual `Open project…` QAction.
- `test-reference-schema-setup-red.log`: the test first assumed a beam had
  `material_id`; the actual beam references its section, which references the
  material. That test assumption was corrected without changing the model.
- `test-history-applied-setup-red.log`: preview/cancel/commit/GUI undo really
  succeeded, then the test wrongly expected the undo history item to retain
  `applied: true`. The corrected assertion preserves transaction ID and label
  and requires `applied: false`, as the existing history API specifies.

## Actual green evidence

The private test build compiles fresh owned desktop and QtTest/moc translation
units with the existing strict Release options, then links private copies of
the frozen frontend adapters. It does not write the shared build. This is a
private adapter diagnostic, not the common frontend Release target. The engine
is the root-built production binary. The build log and private archive/source
hashes are retained here; `receipt.json` explicitly distinguishes an engine hash
at archive time from a per-launch hash claim.

Each `aiSnapshotUnitsAndCameraAreConsistent` invocation actually opens its
original snapshot through the normal GUI file dialog. It queries all 21 node
coordinates, all 20 beam references, the section/material/force values and the
complete 49-entity field image from the same active engine document. It checks
1000 mm length, 210000 MPa E, 100 mm² area, 833.333 mm⁴ I1/I2, 1400 mm⁴ J and
global `(0,-1,0) N`, then selects those entities in the visible property dock.

The camera test sends a real left-button mouse drag to the VTK widget, verifies
the actual VTK position/focal point/view-up and fingerprint changed, then checks
the complete model, force ID/vector, revision and history remain identical. It
also verifies the original snapshot bytes are unchanged. Raw before/after JSON,
visible screenshots and actual backend facts are in `green/`.

The default native backend is cocoa/QMacStyle with `.AppleSystemUIFont`, no
font-engine override, and the actual Apple M5 OpenGL 4.1 Metal renderer. No
offscreen or FreeType substitution was used.

`aiSnapshotForcePreviewUndoAndRecoveryUseSharedHistory` runs against session5:
preview and cancel leave model/revision/history untouched; a second preview and
Apply change creates exactly one revision and one persistent history item.
Only the existing force vector becomes `(0,-2,0) N`; its ID, node reference and
every other model field are unchanged. GUI Undo restores the original complete
model and marks the same transaction unapplied. Killing/restarting the real
engine and invoking GUI Recover workspace retains the document, revision and
history while renewing the epoch. Its raw `force-shared-history` JSON and
screenshots are retained.

The original committed screenshot correctly shows no selection because revision
admission cleared the old property state. It remains preserved and is not claimed
as a visible −2 N readback. A subsequent test-only refinement selects another row
and then the same force through normal mouse selection, verifies the returned
XYZ editors and physical-value label show `(0,-2,0) N`, and takes the new
`AI-force-e2fcb43a-5464-4bd3-9caa-ae4c1ee65cd1-committed-minus2N.png` screenshot.
This entire force preview/commit/readback/undo/recovery invocation passed in
10.345 seconds. The receipt preserves the earlier unit/rotation runs with their
original source SHA and explicitly records the later test-only source change;
the three unit runs were not repeated or relabelled.

Normal open creates a fresh working document at revision 0 with an empty active
undo history. The force test concerns the new working history; it does not claim
that the portable snapshot retained the AI construction undo stack. It uses a
temporary workspace which is removed at completion. A subsequent real AI undo
handoff and save/reopen flow are separate evidence requirements.

## QG-02 source review

`selectedPhysicalDetails` is a read-only presentation of engine-returned fields;
malformed numeric/vector data displays unavailable rather than guessed zeros.
`loadSelectedProperty` retains document, epoch, revision and selected-ID guards
before accepting a row. Clearing or changing selection clears the old preview
and values. `previewProperty` retains generation/context guards;
`previewAndCommit` adds the same late-preview guard for property edits while
keeping import separate. The authoritative effect is still the engine's
`RecordApplication` preview and one existing commit/history transaction. New
force operations and their schemas belong to the root implementation, not this
UI slice.

The independent strict build, design check, clang-format check and diff check
passed at this checkpoint. Shared frontend Release build and the existing six
GUI/MIX workflows/selection regression are the root's subsequent common checks;
their results must not be inferred from these private diagnostics.
