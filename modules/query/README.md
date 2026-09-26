# qcae_query

Snapshot queries, disposable selection sessions and render packet production.

Public API: `qcae/query.hpp`

Direct dependencies: `qcae_document`, `qcae_contracts`

Boundary: Depends on read_view.hpp, never the MemoryApplication facade.

Minimal consumer: `#include "qcae/query.hpp"`; link `qcae_query` in CMake.

Validation from the repository root after configuring/building the applicable options:

```sh
ctest --test-dir build-core --output-on-failure -R "^(query)$"
```

Actual ownership and links: [target manifest](../../modules/targets.json). Generated build directories are not committed.
