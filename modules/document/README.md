# qcae_document

Typed model, validation/reference traversal, record codec and model deltas; read-only ModelSnapshot.

Public API: `qcae/model.hpp`, `qcae/model_codec.hpp`, `qcae/model_delta.hpp`, `qcae/state_codec.hpp`, `qcae/read_view.hpp`

Direct dependencies: `qcae_foundation`, `qcae_contracts`

Boundary: No application lifecycle, GUI, storage adapter or concrete solver dependency.

Minimal consumer: `#include "qcae/model.hpp"`; link `qcae_document` in CMake.

Validation from the repository root after configuring/building the applicable options:

```sh
ctest --test-dir build-core --output-on-failure -R "^(model|delta)$"
```

Actual ownership and links: [target manifest](../../modules/targets.json). Generated build directories are not committed.
