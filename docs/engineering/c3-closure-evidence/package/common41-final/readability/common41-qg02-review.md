# Common41 QG-02 source binding

Reviewed common commit: `b740b374c934f3b4624a21247b9a5deeb2ca6bf4`.
Independent committed-source digest: `36335df61c1d57bec11ba620b957d25b0ba07e3242b63f7951f891ebe3a3e5ff`.
399 committed regular source/test/tool files; all 399 working-source byte hashes match at capture. The user-owned AGENTS.md difference is preserved and does not substitute for the committed initial context. See `common41-source-binding.json`.

Reviewer: `/root/lean_refresh_review`. This is an external source review record; it is not part of the commit/digest it reviews. It references the complete responsibility table and the 245-path inventory in `prepare-review.md` / `slice-source-coverage.json` rather than rewriting historical receipts. Compared with the failed common40 commit, only six source paths changed: CMakeLists.txt, tests/desktop_selection_tests.cpp, tests/test_c4_freeze.py, new tests/test_macos_package.py, tools/freeze_c4_baseline.py and tools/stage_macos_package.py. Production GUI, force, analysis, solver/result, Core/MCP, transport, SQLite and VTK implementation bytes remain the earlier reviewed common source.

## QG-02 disposition and review boundaries

For the common production slices and bounded local staging/freeze repairs, the entry → authoritative state → adapter paths, invariants, error handling and tests are located and reviewable. No remaining concrete readability/authority/side-effect blocker was found in this finite source review. Its basis is the earlier named slice reviews, current source hash audit, and the six-path final diff review below. QG-02 source binding is complete for this stated scope.

This is not independent line-by-line rereview or formal approval of every SDK diagnostic path. The original SDK7 collector exception RED remains unchanged; private SDK8 source is not adopted into the committed preparation/collector. Full SDK instrumentation and its exception boundary are not marked QG-03 passed by this source-quality receipt. Whole-pipeline/CoreText/FreeType ownership coverage, final BP/SK12/C3 targets, actual unconfigured Nastran execution/numerical acceptance and final M6 distribution remain separately unfinished. All corresponding unknown/failed provenance labels are retained.

Overall common QG-03 remains **pending** until the root's actual same-commit, same-environment matrices and required quality tools finish. The previous `82526d8…` desktop 84/85 result stays RED. The old scoped AI/native/SDK/helper logs remain tagged to their original source and environment. The common41 snapshot cannot be marked GREEN using their counts.

## Six-path source review

### Staging boundary and author separation

`tools/stage_macos_package.py:71` now resolves the canonical workspace and allowed ignored out root, rejects a symlink at out itself and existing/dangling final target, resolves the complete requested target, and requires a strict descendant with `.app` suffix before returning it. `stage:89` performs this before its existing cache checks or any mkdir/copy/external tool. The returned canonical path is used by every later directory/loader/copy/signature operation. The old Release and explicitly empty SQLite observer cache rules are unchanged. New/nested target, absent out, and internal alias behavior are preserved.

Actual old stage() in private ROOT fixtures created Contents/MacOS outside ignored out for both lexical dotdot and an external parent symlink. The source and RED receipts remain preserved. The new 13-test actual preflight suite exercises those paths, symlink out itself, existing directory/file, existing/dangling final symlink, out/wrong suffix, normal paths and cache gates. Negative cases reject before copy/tool and with unchanged private tree; positive cases stop at a test-only copy sentinel after expected target creation. This proves local preflight behavior, not an actual Mach-O deployment or immunity to a hostile concurrent filesystem-parent replacement.

This reviewer authored the staging tool and new test only after explicit root authorization. Root independently read both files and ran the same 13 tests GREEN in 0.013s. The log is preserved as `stage-root-independent-green41.log`; root owns CMake registration and independent approval. This author's work is not labeled independent self-review.

### Freeze metadata boundary

Root authored `tools/freeze_c4_baseline.py:314–320`: exact int/float type and positive bounded DPR are checked before math.isfinite; all image entries must be mappings before row.get/set collection. Thus both actual bounded failures—nonmapping image entries and 401-byte oversized DPR integer—now fail as FreezeError before image/evidence access. Other path/hash/field type errors remain within the main function's original structured prerequisite-error boundary. No committed-source, evidence, mode, threshold, fixture, command or provenance condition was relaxed.

`tests/test_c4_freeze.py:293` invokes the actual CLI for string/None/list/int image entries and asserts exit1, a real blocked artifact, mechanism_frozen=false and no traceback. `:306` similarly protects the oversized integer with exact Invalid device pixel ratio rejection. The original full fixture positive tests remain; root's actual full log records 26/26 tests PASS, 34.406s (`common41-precommit/qcae-freeze-all-green41.log`). The intermediate 25/25 and both original metadata/ratio CLI REDs remain separate. Independent actual helper calls cover 12 finite image/DPR type/sign/range negatives, all returning FreezeError at source SHA `452b2847…654b7`, exactly this committed source. This is a finite metadata error review, not general arbitrary-JSON fuzzing or independent proof that supplied execution records actually ran.

### Selection fake and two delivery negatives

`tests/desktop_selection_tests.cpp:514` now returns entity rows and data.revision from the same current fake state; it does not echo a request's expected revision. Held-row requests redispatch against the current fake state when released, retaining coherent rows/version. The new missing/999 revision fault applies only to explicit IDs and leaves all normal selection/render/view paths untouched. The original failing positive function and every assertion remain byte-exact.

The added actual local-socket/DesktopWindow/VTK cases select line-b normally, deliver the controlled property query response, await the socket barrier after the query, and inspect real property/mesh widgets. They require an empty property display, disabled apply, and zero mesh.generate_line requests. This measures reply delivery and the existing production fence; no product validation was loosened to satisfy the fake. Root/owner evidence binds unchanged production UI SHA `08b152…95c7`, targeted positive+two-negative GREEN (five QtTest entries, 4.129s) and full single-target GREEN (63 QtTest entries, 96.32s). Those source-ready tests do not reclassify the earlier common 85-target failure; common41 must run its entire matrix.

`CMakeLists.txt:68` registers only the pure Python macos_package_preflight in the existing BUILD_TESTING block. It adds no library/dependency/feature flag. The expected new matrix totals are not evidence until CTest reports them.

## Every old review-manifest difference

`common41-review-hash-bindings.json` validates all six repository reviewed-source manifests, including external generated SDK7 dependencies. Their 50 rows now contain 44 matches, six historical differences, and zero absent files:

1. `core-mcp-independent39` typed_host.cpp old `7ea0…` is superseded by the private shared typed JSON serializer added before the numerical quota repair.
2. `numerical-persistence-review-build39` typed_host.cpp old `df307…` is superseded by exact scratch `encoded_bytes` observation. The current GUI quota source manifest and actual scratch test cover that small addition; it counts compact wire serializer output, not a socket frame.
3. `force-vector-independent40` CMakeLists.txt differs only because the pure Python preflight test was registered. All 11 other source rows still match, including force operation/schema/tests and their Core references.
4. `gui-units-quota-independent40` desktop.cpp and c3_desktop_workflow_tests.cpp are the old pre-property-fence bytes. They are superseded by the final property receipt, whose four source rows match this commit. That receipt and the actual original 43-case matrix/final force-positive tags remain distinct.
5. The same older GUI unit receipt's desktop_selection_tests.cpp is now superseded by the current-snapshot fake field plus two missing/wrong-version negatives described above; the production UI bytes did not change.

The final property receipt is 4/4 matching; SDK7 manifest is 10/10 matching. SDK7 includes diagnostic generated objects/headers outside the committed source inventory; matching them does not prove complete coverage or turn SDK8 into production adoption. The exact reconstructed 43-case pre-clamp matrix SHA/diff is a new supplemental historical fact in this private directory; no older receipt was edited.

## What final QG-03 binding still needs

Root's common41 runner must finish every actual configure/build/test and required design/format/diff/protected-AST/public-consumer/readability stage with recorded zero exits, matching raw SHA and actual binaries at this commit/digest/environment. This source review neither starts those commands nor asserts their completion. A new final execution supplement should bind their observed results, preserve all old RED evidence, and retain excluded unfinished solver/SDK/performance/release acceptances.
