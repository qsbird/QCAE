# qcae_contracts

Dependency-free read/write DTOs, render packets, profile metadata and persistence ports; generated operation descriptions.

Public API: `qcae/application_types.hpp`, `qcae/document_info.hpp`, `qcae/profile_provider.hpp`, `qcae/render_packet.hpp`, `qcae/workspace_store.hpp`

Direct dependencies: `qcae_foundation`

Boundary: No state ownership, domain implementation or Qt/VTK/SQLite SDK types.

Minimal consumer: `#include "qcae/application_types.hpp"`; link `qcae_contracts` in CMake.

Validation from the repository root after configuring/building the applicable options:

```sh
ctest --test-dir build-core --output-on-failure -R "^(contracts)$"
```

Actual ownership and links: [target manifest](../../modules/targets.json). Generated build directories are not committed.
