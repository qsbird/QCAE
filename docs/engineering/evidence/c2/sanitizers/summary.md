# Independent C2 ASan / UBSan validation

Configuration: Debug, IPC ON, STORAGE ON, DESKTOP OFF,
QCAE_ENABLE_SANITIZERS ON, Qt prefix /opt/homebrew/opt/qt, AppleClang 21,
SQLite 3.51.0. Build concurrency 2; CTest concurrency 2.

Runtime settings: ASAN_OPTIONS=halt_on_error=1:abort_on_error=1;
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1.
No explicit leak-sanitizer claim is made.

The independent build directory is build-c2-sanitize. The validator modified
no product/test source and made no commit. Source-start.json, source-end.json
and source-changes.json record source hashes. The final build dry run reported
`ninja: no work to do.`

The initial all-target build exposed an invalid test assignment of `{}` to an
explicit EntityId. Root corrected it to `EntityId{}` and the later complete
build succeeded. build.log.gz retains the original error; build-final.log and
build-post-review.log retain successful rebuilds.

The initial 29-test non-socket run passed 28 tests in 117.12 seconds. The sole
failure was record_store fixture setup assuming SQLite would remove empty WAL
and SHM after close. Diagnosis confirmed the latest generation/payload was
already checkpointed into the main file, with a zero-byte WAL. Root corrected
only disposable fixture setup to check the WAL is empty before deleting its
sidecars and applying mode 0444. The frozen inputs and reader behavior were not
relaxed. Original failure output remains in ctest-sanitizers.log.gz and
record-store-reproduce.log.

After the latest source changes, the final j2 build recompiled record_store,
typed_host and its test. The necessary record_store and typed_host recheck
passed 2/2 in 2.66 seconds. ctest-recheck.log contains complete verbose output.
No AddressSanitizer or UndefinedBehaviorSanitizer diagnostics appeared.

The passing initial set includes legacy_migration, record_application,
operation_registry, core, contracts, allocation_atomicity, parameters,
model_operations, record_query, model, nastran, delta, persistence, query,
sqlite, runtime, geometry_mesh, geometry_mesh_sqlite, record_document,
acceptance_checker, architecture_gate_unit, cpp_format, design, and all generated
contract/schema/profile checks. record_document's 1000 / 10000 / 100000-node
record-layer locality workloads passed all 60 samples under sanitizers.

Excluded socket/end-to-end tests: ipc, m1_ipc, m23_ipc, c2_workflow. Root runs
these separately. This evidence does not claim solver execution, real AI
integration, graphical acceptance, end-to-end locality/performance or complete
P0 acceptance.

C1 read-only source review found no new blocking issue. The migration facade
forwards ownership handlers into the same RecordApplication. The legacy reader
holds an inode lease, verifies main/WAL existence, metadata and exact copied
bytes, and reads a checked SQLite transaction in a disposable snapshot. The
migration preserves full history including redo, operation/host facts, exact
pending-save bytes/tokens, and documented DocId/Epoch behavior. The tests compare
all old engineering collections/references with frozen golden JSON. Current
WAL/no-SHM/mode-0444 cases are covered; a concurrently changing foreign writer
was reviewed in the metadata/byte-check implementation, not independently
stress-tested in this run.

../fixtures.json confirms all six source SHA-256 values equal the immutable
manifest and that no .lock / -wal / -shm exists beside any frozen source.

Final coordinator supplement: after shared legacy/typed quantity normalization, the independent sanitizer build was updated and the six affected tests (core, model_operations, quantity_entry_consistency, sqlite, legacy_migration, typed_host) passed 6/6 in 8.81 seconds. See ctest-post-normalization.log. No ASan/UBSan diagnostics appeared. This recheck does not claim socket or GUI sanitizer coverage.

Compressed .log.gz files preserve the original log bytes; inspect with gzip -dc.
