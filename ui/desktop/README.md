# qcae_desktop_ui

Existing desktop window, editors and request orchestration behind run_desktop.

Public API: `qcae/desktop.hpp`

Direct dependencies: `qcae_desktop_client`, `qcae_vtk_view`

Boundary: Window remains monolithic internally in this bounded migration; view-model extraction is future work. Domain writes cross DesktopClient.

Minimal consumer: `#include "qcae/desktop.hpp"`; link `qcae_desktop_ui` in CMake.

Validation from the repository root after configuring/building the `QCAE_BUILD_DESKTOP=ON` (desktop also requires IPC/storage):

```sh
ctest --test-dir build-local --output-on-failure -R "^(desktop|desktop_smoke)$"
```

Actual ownership and links: [target manifest](../../modules/targets.json). Generated build directories are not committed.
