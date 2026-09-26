# qcae_application

Existing MemoryApplication lifecycle, transaction/history coordination and private persistent state.

Public API: `qcae/core.hpp`

Direct dependencies: `qcae_document`, `qcae_contracts`, `Threads::Threads`

Boundary: application_state.hpp stays private in src/. This slice retains the existing coordinator and handlers; their later decomposition is not claimed.

Minimal consumer: `#include "qcae/core.hpp"`; link `qcae_application` in CMake.

Validation from the repository root after configuring/building the applicable options:

```sh
ctest --test-dir build-core --output-on-failure -R "^(core|persistence|allocation_atomicity)$"
```

Actual ownership and links: [target manifest](../../modules/targets.json). Generated build directories are not committed.
