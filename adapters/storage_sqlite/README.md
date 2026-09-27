# qcae_sqlite

SQLite workspace transactions, recovery payloads, project leases and snapshot publication.

Public API: `qcae/sqlite_store.hpp`

Direct dependencies: `qcae_contracts`, `${QCAE_SQLITE_TARGET}`

Boundary: Implements IWorkspaceStore; no application implementation dependency.

Minimal consumer: `#include "qcae/sqlite_store.hpp"`; link `qcae_sqlite` in CMake.

Validation from the repository root after configuring/building the `QCAE_BUILD_STORAGE=ON` (desktop also requires IPC/storage):

```sh
ctest --test-dir build-local --output-on-failure -R "^(sqlite)$"
```

Actual ownership and links: [target manifest](../../modules/targets.json). Generated build directories are not committed.

`read_legacy_workspace_readonly` stages a detached main database and every
existing WAL file in a private temporary directory. It holds the source inode
lease and checks main/WAL existence, inode, size, nanosecond modification/change
times and exact copied bytes before SQLite consumes the snapshot. A source that
changes during copying is rejected; a foreign SQLite writer does not have to
honor QCAE's lease for this check. SQLite reconstructs SHM and any other recovery
sidecars only beside the disposable copy, then validates `quick_check` and reads
one transaction. The original main, WAL and SHM bytes remain untouched, including
when the WAL contains a committed change newer than the main database. Existing
SHM is deliberately rebuilt from WAL instead of copied.

This is the explicit full legacy migration/read boundary. It requires a
quiescent source and temporary disk space for its main database plus WAL; it is
not a live backup service or the incremental record commit path. WAL-mode
sources with missing sidecars work through the same isolated snapshot.

`record_store` requires success for a committed hot-WAL read, verifies unchanged
main/WAL/SHM bytes, and repeats after removing only the disposable test source's
SHM. `legacy_migration` additionally compares all six frozen old datasets to
complete expected entity/reference values through the new application.
