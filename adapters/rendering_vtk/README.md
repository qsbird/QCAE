# qcae_vtk_view

VTK rendering and pick handling consuming RenderPacket.

Nodes, beams and committed geometry lines have separate display cells mapped back
to their stable entity IDs. Geometry endpoints are coordinates rather than mesh
nodes. Clicks use intersection hit testing; boxes use the requested contained or
intersecting mode. Visible selection uses VTK hardware visibility; through
selection projects the entities present in the packet. Hidden geometry is omitted
by the packet producer.

`setPreview(RenderPreview)` replaces a separate purple, nonpickable line actor;
`clearPreview()` removes it. Previews contain no entity IDs, do not change the
committed packet or highlighted IDs, and are excluded from both selection paths.

Public API: `qcae/vtk_view.hpp`

Direct dependencies: `qcae_contracts`, `Qt6::Widgets`, `Qt6::OpenGLWidgets`, `${VTK_LIBRARIES}`

Boundary: No domain/application/state storage ownership.

Minimal consumer: `#include "qcae/vtk_view.hpp"`; link `qcae_vtk_view` in CMake.

Validation from the repository root after configuring/building the `QCAE_BUILD_DESKTOP=ON` (desktop also requires IPC/storage):

```sh
ctest --test-dir build-local --output-on-failure -R "^(desktop)$"
```

Actual ownership and links: [target manifest](../../modules/targets.json). Generated build directories are not committed.
