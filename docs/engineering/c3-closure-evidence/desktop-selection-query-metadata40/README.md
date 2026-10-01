# Selection fake entity.query snapshot metadata

REQ-15 / TST-A07 and TST-I04 regression protection.

## Bounded plan

Preserve desktop.cpp exactly and retain every assertion in
latestTreeSelectionSurvivesViewRefreshAndEnablesMeshTools. Match the production
entity.query success contract by returning data.revision for the actual snapshot
used to generate rows. The held-row helper stores requests and redispatches them
at release, so the regenerated current rows and revision remain coherent; no
older request.expected_revision is echoed as a substitute for current data.

Add real local wire/DesktopWindow/VTK widget negatives for missing and incorrect
single-entity data.revision. Resolve the selection normally, deliver the faulty
query reply, and verify that mesh controls remain disabled and no mesh operation
is sent. Query valid metadata through the original positive case still enables
mesh controls. This fake is a controlled local test server, not the production
engine.

## Original RED

common40-final-desktop-tests-original-red.log is an exact copy of the shared
82526d8f851acb9c2b837b5133fa1e50a2d5948c CTest log. Its only failure is the mesh
Apply assertion after latest tree selection. The fake returned entities/total
without data.revision; production ipc_model.cpp ok() supplies that field.
The UI version fence correctly rejects the incomplete test reply, so the selected
geometry is not admitted to ModelingTools. No product behavior is loosened.
Original source, binary and log hashes are in original-red-source-binding.json.

The positive/negative fresh build and native results will be recorded separately;
they do not relabel the original RED or the earlier AI/native unit runs.

## Fresh GREEN and readability review

Only qcae_desktop_selection_tests was rebuilt with the existing strict Release
-O3 -DNDEBUG -Wall -Wextra -Wpedantic -Werror options. Exact compile/link and
build/run commands, the test binary and unchanged production archive hashes are
in fresh-green-source-binding.json. The production desktop.cpp SHA remains
08b152f7620aef5090770e66a5385082866ec484e3cbce52f77332a7d5f395c7. The entire
original positive function, including every assertion, is byte-for-byte
unchanged; its function hash is recorded in receipt.json.

native-targeted-green.log: the original failing positive and two new actual
wire/widget negatives passed, five QtTest entries including init/cleanup,
4.129 seconds. native-full-green.log and native-full-green-raw.log: the complete
desktop_selection CTest passed in 96.32 seconds, 63/63 QtTest entries including
init/cleanup (59 existing behavior cases + 2 new negatives). The raw log retains each
case result and the delivered controlled missing/999 revision JSON.

QG-02: the engine remains the authoritative snapshot owner; this patch repairs
only the test server's representation of that contract. A fake request held
until release generates its rows and revision from the same current fake state.
The negative injection applies only to explicit IDs and never loosens UI
validation. Real QLocalSocket request/response delivery and the actual
DesktopWindow/VTK/ModelingTools widgets exercise the existing production fence.
A socket barrier follows the query before inspecting controls, and no mesh write
request is sent when the metadata is unusable. No business state, validator,
product source, dependency or original test assertion changed.

Source is frozen and the native slot has been released. This single-target result
does not relabel the original shared 85-target RED or earlier AI/native evidence.
The root's new common commit/build/full matrix remains separate.
