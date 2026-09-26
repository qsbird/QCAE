# qcae_engine_api

Server-side JSON adaptation and dispatch into application, codec ports and selection services.

Public API: `qcae/ipc_api.hpp`, `qcae/ipc_model.hpp`, `qcae/ipc_selection.hpp`

Direct dependencies: `qcae_application`, `qcae_query`, `qcae_contracts`, `Qt6::Core`

Boundary: Engine-only dependency. Clients must not include these headers or link this target.

Minimal consumer: `#include "qcae/ipc_api.hpp"`; link `qcae_engine_api` in CMake.

Validation from the repository root after configuring/building the `QCAE_BUILD_IPC=ON` (desktop also requires IPC/storage):

```sh
ctest --test-dir build-local --output-on-failure -R "^(ipc|m1_ipc|m23_ipc)$"
```

Actual ownership and links: [target manifest](../../modules/targets.json). Generated build directories are not committed.
