# qcae_foundation

Stable IDs, units, errors and small value types.

Public API: `qcae/types.hpp`

Direct dependencies: C++20 standard library only.

Boundary: Legacy Material remains in types.hpp to preserve this structural slice and profile identity; moving entity declarations belongs to the schema slice.

Minimal consumer: `#include "qcae/types.hpp"`; link `qcae_foundation` in CMake.

Validation from the repository root after configuring/building the applicable options:

```sh
ctest --test-dir build-core --output-on-failure -R "^(core)$"
```

Actual ownership and links: [target manifest](../../modules/targets.json). Generated build directories are not committed.
