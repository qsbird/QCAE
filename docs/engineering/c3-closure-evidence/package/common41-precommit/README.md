# Source fixes and actual precommit checks, build41

The selection fake now supplies the revision of the rows actually generated,
matching the production query contract. Root reviewed the test-only change,
including the two delivered missing/wrong-version negative cases and their
socket barrier. The original positive function and its assertions retain their
original bytes. The independently recorded native selection suite passed 63
QtTest entries; its original RED remains in the separate selection receipt.

Root independently reviewed the staging helper authored by
`/root/lean_refresh_review`. It resolves the complete target before directory or
copy side effects, rejects existing/dangling targets and the output-root link,
and admits only a new canonical `.app` inside the workspace output root. The
original Release and empty-observer requirements are retained. Root separately
ran all 13 actual preflight tests, including normal paths that reach the copy
boundary; no deployment program or native GUI ran in those tests.

Root fixed two observed freeze-reader input failures: a non-object tool-image
entry no longer raises AttributeError without a receipt, and an oversized
integer pixel ratio is rejected before floating-point conversion can overflow.
Both actual CLI REDs and the original source are retained. The complete isolated
freeze contract suite passed 26 tests. These synthetic Git/metadata fixtures
are tool-contract checks, never product, hardware or solver acceptance.

The adjacent receipt records exact working-source hashes, original commands and
raw evidence. It deliberately has no invented new source commit: it was written
before that commit exists. Design, C++ format and diff checks passed. The next
actual common-commit matrix must independently bind and run all four build/test
configurations and quality gates before the C4 mechanism can be frozen.
