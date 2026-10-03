# Host-controls archive and result audit 180

**No blocking discrepancy found.** Audited completed archived evidence for implementation `89241496fa4f3b80886cd6a8560019686d5c6d4e` against the report, machine record, member index, raw logs, observation/transcript files and current frozen inputs. The repository and immutable archive were not modified; this report is outside the archive.

| Audited artifact | SHA256 |
|---|---|
| `evidence.tar.gz` | `216218dcb314ad62605e8d5c446e309b85a7370c2eed728278a6f2e960b228f0` |
| `archive-index.json` | `71523cc556a75860730749e9fcdae182b5805792d246b0c8341619019b18c6c0` |
| `validation.json` | `e3f158d1dfb0b45d7c9126c684ac4b5c77329a644db93a897f1416d1d6a763d7` |
| `host-controls-contracts-2026-10-03.md` | `5f07b67605aff9994a7d6b3dee22411ce7a7834914a53bfd62ad4e312bbab26e` |
| Tested 420-input canonical dictionary | `fd726316a1bc0be0f282aaf0ed4dd8e2b82784e1d64e58c2921b74ae6272a3ce` |

The archive is exactly **154 regular file members, 920290 bytes**. Every member name, byte length and SHA256 matches the index; every indexed retained origin file independently matches the member. Missing, extra, duplicate, nonregular or mismatching entries: zero. Origins were restricted to the indexed `/private/tmp/qcae-*` paths. No runtime database, executable/library object or build tree is present as an archive member.

## Raw results and denominator

| Configuration | Raw LastTest entries / passed | Raw stdout passed lines | Recorded CTest exit | Configured inventory |
|---|---:|---:|---:|---:|
| core | 40 / 40 | 40 | 0 | 40 |
| local | 55 / 55 | 55 | 0 | 55 |
| sqlite-on | 87 / 87 | 87 | 0 | 87 |
| sqlite-off | 78 / 78 | 78 | 0 | 78 |
| sanitized | 1 / 1 | 1 | 0 | 55 |

Raw test names match each machine execution list; raw stdout totals and zero-failure summaries match. The total is **261/261**, consistent with validation.json, the report and baseline README. Sanitized CTest explicitly selected `^typed_host$`; its sole raw test is typed_host. Its full 55-test inventory is configuration evidence, not 55 sanitizer executions. ASAN_OPTIONS disables leak detection, so LSan remains NOT RUN.

All five configure/build/inventory/CTest receipt exits are zero. Actual retained compile_commands hashes match their summaries; all 70/104/119/118/104 listed compiler units use `-std=c++20`, with no GNU dialect flag. Raw build stdout/stderr contain zero compiler warning lines. Archived design/format/diff receipts have exit0. These are existing completed executions audited here, not checks rerun by the auditor.

The separate baseline contains one initial typed_host exit0 and one IPC exit1 before readiness; retained stderr reports engine exit4 and `QLocalServer::listen: Unknown error 1`. The later baseline has four exit0 executions for typed_host, IPC, MCP and durable MCP. Its source/product candidate dictionaries exactly match the initial baseline dictionaries, and its completion record states four unchanged baseline cases. All six baseline attempts remain separate from the 261 CTest denominator. The older changes.commit record's 263 executions/261 passes/2 failures remains historical and is not used as this batch's current count.

## Actual SQLite and recovery

Both package ON/OFF host-controls observation files contain exactly **101 refusals, 5 reads, 6 replays, 8 fresh history writes and two engine exits [-15,-15]**. Each recorded refusal request/response matches an actual transcript entry; all five explicit-v1 read results, six replay receipts and eight write receipts also match the transcripts. The observation flags preserve complete logical state around all recorded refusals and reads; the frozen test's actual passed fixture performs the same full-row comparisons around its replay/lookup paths. The archive records observations/transcripts and initial/final/recovered logical snapshots, rather than retaining a separate full SQLite snapshot pair for every individual refusal.

For each scenario, transcript readback independently reconstructs the complete final pre-recovery and recovered entity.query/entity.fields blocks: the same two stable IDs, query rows and exposed fields. Final and recovered history are exactly equal; revision is 10 in both, document identity is stable, epoch changes. All three recovered original undo/redo/changes.commit facts preserve transaction_id, committed_revision and entity_id and report current revision/content with replayed=true. Recovery contains five expected old-epoch refusals. Eight nonreplayed history writes account for initial revision2 → final revision10. SIGTERM exit-15 is retained as the actual termination result; no physical WAL-byte equality is claimed.

## Source and scope

The freeze dictionary has exactly420 entries and equals the matrix candidate's complete dictionary. Recomputing sorted relative-path→SHA256 canonical JSON produces the stated fd726316… digest; this is expressly not C4-source-v1. The seven archived commit source files match their tested dictionary hashes and the actual implementation commit's seven paths:

| Operational source | Tested SHA256 |
|---|---|
| `CMakeLists.txt` | `6906b75127d94d16e7721e9af663e1f0fcef025ef2ce912e2a405919228ef1aa` |
| `adapters/engine_api/src/ipc_api.cpp` | `6fafc3cd23841ab8cd2d31f0d7d5a9aba93b1620f29e309146b07c4cc13ebd34` |
| `schemas/operations/document_changes.json` | `855db11f8b932c68642653de4f30a0ad870776891030ce0f26a23bf560cd912c` |
| `schemas/operations/host_reads.json` | `350f29adce5dd83ff3be9b51f285ef8dc9ab8792b9363dd4b557245ee0f02396` |
| `tests/typed_host_tests.cpp` | `a7a37c40bb6a3de51a6a812064ac9d03af4ef2db5a45bfa54579883699057b0e` |
| `tests/mcp_bridge_tests.py` | `5d830b86a1b2fa2bf0f41dddb6ea0752a4318f8d95fbe8daf531f77b64836da3` |
| `tests/host_controls_ipc_tests.py` | `d3eaf238ddeae8373701228302dbb7a8d2dad14a368ef809f26a7454b7b8c5ba` |

At audit time, private checkout `/private/tmp/qcae-framework-delivery80` differs from the tested420 only in `adapters/engine_api/README.md`: tested `3b3b07f10cb20ed7d5641e798e4218992764d37600adfcca64833d236fe97b27`, documentation-phase `e53d929f140c98eb0b9cb4ce9d789380d6f867e0f9acfa9195024439600731e6`. All other419 inputs match. The primary checkout currently matches all420 tested inputs, so its README documentation change had not yet been forwarded at this read; all419 other inputs agree between both checkouts. This is an informational document-phase timing distinction, not an operational source change or a need to rerun products.

The report/README/handoff keep the unrefreshed f2d2e7f package, original cumulative180-second graphical failures and old-binary69-row isolation diagnostic in their prior scopes. All five new configurations disable desktop; no new graphical root-cause repair or full GUI pass is asserted. The archived operations.get plan is READ_ONLY_PLAN_NOT_IMPLEMENTED. Real solver/full external AI, performance, SDK coverage, previously measured budget failures and complete SK/P0 remain outside current acceptance.

Auditor product execution, tests, generation, build/configure, validation gates, Git mutations and repository edits: **NOT RUN**. Only read-only archive/log/source/hash comparison and separate review-file creation were performed; no private external configuration was read.
