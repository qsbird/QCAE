# qcae_desktop_ui

Desktop window, editors and request orchestration behind `run_desktop`; `create_desktop_window` exposes the same window with caller-owned lifetime/event loop.

Public API: `qcae/desktop.hpp`

Direct dependencies: `qcae_desktop_client`, `qcae_vtk_view`

Boundary: `ModelingTools` privately owns disposable line input/preview and one mesh task UI. Domain writes cross DesktopClient with document/epoch/revision and an immutable idempotency key. Transport loss suspends the tool; same-context reconnection enables explicit retry of the original intent or resumes task polling. A different document/epoch resets the tool. Mesh generation uses the existing engine task and has no local candidate mesh preview. The window owns the installed render-version fence and selection request generation.

Minimal consumer: `#include "qcae/desktop.hpp"`; link `qcae_desktop_ui` in CMake.

Validation from the repository root after configuring/building the `QCAE_BUILD_DESKTOP=ON` (desktop also requires IPC/storage):

```sh
ctest --test-dir build-local --output-on-failure -R "^(desktop|desktop_smoke|desktop_selection|modeling_tools|c3_desktop_workflow)$"
```

Actual ownership and links: [target manifest](../../modules/targets.json). Generated build directories are not committed.
