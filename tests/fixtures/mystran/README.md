# MYSTRAN 19.0.0 parser reference

`cantilever-19.0.0.F06` is copied from the actual local MYSTRAN 19.0.0
run output at `runtime/solvers/mystran-19.0.0/smoke-decimal-tmpdir/cantilever.F06`.
It contains the 21-GRID, 20-CBAR cantilever in mm-N-MPa units. This is a
reference for result-format parsing and frozen GRID mapping, not a fresh solver
execution or independent numerical acceptance record. Parser mutation and
continuation tests are synthetic and do not create numerical evidence.

Sanitization removes the generated numeric run stamp, the MYSTRAN BEGIN/END
timestamp lines and the measured CPU-time line, and trims trailing whitespace.
The release banner, input echo, diagnostics, component headings and numerical
result rows are preserved. No absolute machine paths are retained.

Original SHA-256: `d38d7b82fd2bda2588327c004a064c8d3e213895002830af9d7a46257901a22e`

Sanitized SHA-256: `2f30cbcd88dd9cd1eef52a8d1218b95fbfae9989547f2de67c1b9f319fb6bbde`

The original solver-run output remains local and unchanged. The associated
availability report and full M4 evidence are separate from this checked-in
parser reference.
