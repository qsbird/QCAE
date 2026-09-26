# qcae_desktop_client

Existing asynchronous DesktopClient connection, handshake, timeout and reconnect behavior.

Public API: `qcae/desktop_client.hpp`

Direct dependencies: `qcae_transport_local`, `qcae_contracts`

Boundary: A shared client module with no domain/application dependency. CLI unification into this asynchronous client is later work, not implied by directory placement.

Minimal consumer: `#include "qcae/desktop_client.hpp"`; link `qcae_desktop_client` in CMake.

Validation from the repository root after configuring/building the `QCAE_BUILD_IPC=ON` (desktop also requires IPC/storage):

```sh
ctest --test-dir build-local --output-on-failure -R "^(desktop)$"
```

Actual ownership and links: [target manifest](../../modules/targets.json). Generated build directories are not committed.
