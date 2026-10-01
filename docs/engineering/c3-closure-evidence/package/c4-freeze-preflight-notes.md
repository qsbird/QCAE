# C4 source freeze preflight

The read-only [tool](../../../../tools/freeze_c4_baseline.py) verifies a supplied full, existing commit checked out in the worktree, then writes only the explicitly selected new JSON. It neither commits nor creates worktrees and never runs commands found in evidence. The [temporary example](c4-freeze-preflight-demo.json) contains `environment_schema_example` and `execution_spec_example`. Its Git commits, logs and commands were real temporary tool-contract tests, using explicitly synthetic source/configuration fixtures. They are not QCAE builds, hardware observations or a QCAE freeze; the temporary repository was removed afterward.

Initial context is always read from the eight committed blobs, including committed `AGENTS.md`. Each file records its UTF-8 bytes, SHA256 and delivered bytes using the existing context reader's heading and optional trailing newline. Both content and delivery totals must fit 65,536 bytes. The tool does not deliver an executor context, so actual executor deliveries, subsequent reads and token costs remain separate. The original unfrozen draft's 38 protected files included the four generators; they were not 42 unique files. The current inventory may add protected interfaces while retaining that 38-file minimum and generator subset; the manifest reports its actual unique count. The committed immutable fixtures and target file receive their own blob identities and SHA256. Symlink/submodule inputs, duplicate paths and absent inputs are rejected.

The source-tree SHA256 algorithm is fixed as follows. Select regular commit blobs whose paths fall under `SOURCE_ROOTS` in the tool: root CMake/format files, `cmake/`, and the production/schema/test/tool directories. Sort by UTF-8 path bytes. Start SHA256 with the exact byte domain `QCAE-C4-SOURCE-SHA256-v1` followed by one NUL. For each file append its path length as an unsigned 64-bit big-endian integer, the UTF-8 path bytes, its six ASCII Git mode bytes (`100644` or `100755`), and its content SHA256 as 32 raw bytes. Commit Git tree identity is recorded separately. AGENTS and docs are separately bound initial/protection/fixture inputs; they are not silently included in this source digest.

After the actual source commit, its digest can be stamped without executing builds:

```python
from pathlib import Path
from tools.freeze_c4_baseline import Repository, committed_source_facts, source_digest

repo = Repository(Path.cwd(), actual_full_commit)
source_tree_sha256 = source_digest(committed_source_facts(repo))
```

The environment uses schema `qcae.c4-environment/1`, `frozen=true`, and exactly the fields shown in the example. Record actual non-sensitive OS/CPU/GPU/memory/backend information, compiler/CMake/clang-format/Python/Qt/VTK/SQLite versions, selected font backend, Release configuration, threads, device pixel ratio and the fixed `[1280,720]` framebuffer. `clang_format` starts with its version `21`. `tool_images` includes each required role's actual absolute image path and SHA256. `probe_evidence` names raw probe artifacts relative to the repository with their SHA256. Arbitrary environment/configuration keys and credential paths are rejected; the tool does not search hidden configuration or credential stores.

The execution input uses schema `qcae.c4-freeze-execution/1`, the actual `source_commit`, this exact `source_tree_sha256`, and the byte-exact environment JSON's `environment_manifest_sha256`. Six required `quality_gates` are `design`, `cpp_format`, `diff_check`, `protected_ast`, `public_api_consumers` and `readability`. Each has `passed`, original `commands` as argv arrays, corresponding `exit_codes`, and nonempty raw `logs`. Artifact descriptors contain only a repository-relative `path` and `sha256`. The output keeps command counts/hashes rather than exposing arbitrary arguments. Command assertions and byte integrity are verified; independent review still establishes actual execution provenance.

The four `build_matrix` entries additionally need nonempty actual `artifacts` and `configuration`:

| Matrix | build_type | qt_enabled | vtk_enabled | sqlite_enabled | asan_enabled | ubsan_enabled |
|---|---|---|---|---|---|---|
| core | Release | false | false | false | false | false |
| sqlite_headless | Release | false | false | true | false | false |
| desktop | Release | true | true | true | false | false |
| asan_ubsan | Debug | false | false | true | true | true |

The actual frozen source and every baseline file must match commit bytes and executable mode. This comparison reads actual files rather than trusting Git's `assume-unchanged` flags. `--exclude-user-instructions-diff` excludes only the unknown working `AGENTS.md` content; its committed blob remains the initial input. Staged changes and other dirty/untracked files are rejected. Explicit external execution artifacts may be untracked; they cannot exempt source, initial files, protected files, immutable fixtures or thresholds. Already committed evidence cannot be modified under that exemption. The committed block/record/metadata and node/material copy limits must also exactly retain the existing `1024/256/2/4096/65536/589824/69632` values; a lower newly committed threshold is rejected.

The manifest remains external to the baseline source commit. The committed protection definition may still have a null draft `baseline_commit`; it is a definition, not freeze evidence. The generated manifest always references the real supplied source commit and actual committed blobs. It can be committed afterward without changing which source baseline it describes. An existing output or symlink is never overwritten, and the output cannot be an input or tracked baseline file.

```sh
python3 tools/freeze_c4_baseline.py \
  --source-commit "$actual_full_commit" \
  --environment "$actual_environment_json" \
  --evidence-spec "$actual_execution_json" \
  --output "$new_external_manifest_json" \
  --exclude-user-instructions-diff
```

The [final repository preflight](c4-freeze-preflight-current-verified.json) actually exited 1 because commit `9e0a56b7496d402421940ebd31d34ae47f3ba242` lacks the committed C4 protection definition. It is blocked by the missing new source commit, without changing thresholds. The [23-case final test log](c4-freeze-preflight-selftest-verified.log) covers real temporary commits, blob/delivery identities, protected counts, dirty source even under `assume-unchanged`, unknown AGENTS exclusion, raw-log/hash/commit/environment/configuration faults, immutable-target exemptions, newly committed threshold weakening, UTF-8/packet limits and no-overwrite/credential/symlink rejection. Test calls also verify HEAD and index bytes remain unchanged. Earlier 16-/22-case logs, the first demo and first blocked preflight are retained.

QG-02 self-review: `Repository` owns only read-only Git/blob facts; `initial_packet` handles delivery accounting; `EvidenceReader` verifies explicitly supplied artifacts; execution/environment validators enforce closed schemas; `working_tree_check` protects byte identity before the final output. There is no model authority or external action in the tool. Every failure keeps `mechanism_frozen=false`. A valid mechanism manifest still hardcodes complete C3/SK-12 and SK-13 as unpassed, whole copy coverage as unknown, and executed extensions as zero. No EXT product code or execution hints were added. Root retains common gate collection, actual commit, final review and independent worktree creation.

The common-40 preparation adds `adapters/engine_api/src/typed_json.hpp`, the private declaration of the existing generic typed-dispatch writer, to the draft protected inventory. Its actual count is now 39; the four generators remain a subset, and no copy threshold changes. The isolated contract tests retain the original positive 38-file case, add a committed positive 39-file header case with its actual blob bytes/SHA256, and reject an inventory reduced to 37 with the original `38-file` contract diagnostic. Earlier demo/preflight/self-test evidence keeps its original draft inventory and has not been rewritten.

The [common-40 private validation](c4-common40-environment-validation.json) records the actual 24/24 contract result and byte-valid environment input. The first new header test's fixture accidentally committed its external execution record, then restamped that tracked file; the reader correctly rejected it as dirty. That failed log is retained, and the corrected test commits only the new header/protection definition. No rejection rule was relaxed.

The [common-40 environment parameter record](c4-common40-environment.json) binds real installed dependency images and the selected future `-j2` build parallelism. Its [sampling provenance](c4-common40-environment-provenance.json) distinguishes these frozen parameters from a source/build/acceptance freeze. Default native DPR 2, framebuffer 1280×720 and CoreText/Mac font facts use preserved actual default probes whose images were rehashed. The chosen existing Homebrew SQLite 3.53.4 has a separately readable runtime image, unlike the earlier macOS 3.51.0 shared-cache image; old logs remain in their original environment. Final matrices must explicitly link the selected new SQLite image and repeat the required storage/legacy/recovery checks before acceptance.
