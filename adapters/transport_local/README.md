# qcae_transport_local

Shared local endpoint conventions and Qt transport dependency boundary.

Public API: `qcae/local_endpoint.hpp`

Direct dependencies: `qcae_contracts`, `Qt6::Core`, `Qt6::Network`

Boundary: No engine dispatch, application or domain access. Framing still resides in the current client/host implementations; this is not a new transport abstraction.

Minimal consumer: `#include "qcae/local_endpoint.hpp"`; link `qcae_transport_local` in CMake.

Validation from the repository root after configuring/building the `QCAE_BUILD_IPC=ON` (desktop also requires IPC/storage):

```sh
ctest --test-dir build-local --output-on-failure -R "^(ipc)$"
```

Actual ownership and links: [target manifest](../../modules/targets.json). Generated build directories are not committed.
