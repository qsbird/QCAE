# First actual common-commit matrix, build40

This is the original run of source commit
`82526d8f851acb9c2b837b5133fa1e50a2d5948c`, source digest
`7384f4d00e6c65beca7288e0630bad1c77cb37e944aa2eb61b5ef0085b20739d`.
`actual-commands.json` records original argv arrays, exits, elapsed time, per-stage
source checks and raw-log hashes. The Core Release matrix passed 38/38; SQLite
Release and ASan/UBSan each passed 44/44. The desktop Release build passed.

Desktop CTest passed 84/85 in 656.19 seconds. Its sole failure was
`desktop_selection`, whose original `latestTreeSelectionSurvivesViewRefreshAndEnablesMeshTools`
case did not enable the modeling Apply button. Its old fake `entity.query`
response omitted `data.revision`; the actual production query response supplies
that field. The new property response fence correctly rejected the incomplete
fake. This folder retains the full failure and the actual executed exit code 8.

`c3_desktop_workflow` passed in 365.25 seconds on this exact source, including the
actual-engine workflows, the property response fault cases and the separately
supplied session5 snapshot. That snapshot was saved by an independent harness
after an earlier actual AI run; this desktop test does not rerun the AI session.

The runner stopped on the failed desktop matrix, so its subsequent architecture,
protected AST and public-consumer gates were not executed by this run. This is
not a passing freeze prerequisite. The corrected test fixture and the following
common-commit matrix have separate source bindings and raw evidence.
