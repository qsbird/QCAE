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
