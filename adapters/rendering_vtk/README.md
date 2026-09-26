# qcae_vtk_view

VTK rendering and pick handling consuming RenderPacket.

Public API: `qcae/vtk_view.hpp`

Direct dependencies: `qcae_contracts`, `Qt6::Widgets`, `Qt6::OpenGLWidgets`, `${VTK_LIBRARIES}`

Boundary: No domain/application/state storage ownership.

Minimal consumer: `#include "qcae/vtk_view.hpp"`; link `qcae_vtk_view` in CMake.

Validation from the repository root after configuring/building the `QCAE_BUILD_DESKTOP=ON` (desktop also requires IPC/storage):

```sh
ctest --test-dir build-local --output-on-failure -R "^(desktop)$"
```

Actual ownership and links: [target manifest](../../modules/targets.json). Generated build directories are not committed.
