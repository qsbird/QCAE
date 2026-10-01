# qcae_nastran

The controlled Nastran codec and its profile provider.

Public API: `qcae/nastran_codec.hpp`

Direct dependencies: `qcae_document`, `qcae_contracts`

Boundary: Links only document/contracts; no application or GUI implementation. Logical manifest names remain stable after file relocation. Version 0.2.0 separates a lexical semantic digest from an exact-source implementation fingerprint; old references are not hot-replaced.

Minimal consumer: `#include "qcae/nastran_codec.hpp"`; link `qcae_nastran` in CMake.

Validation from the repository root after configuring/building the applicable options:

```sh
ctest --test-dir build-core --output-on-failure -R "^(nastran|generated_profile)$"
```

Actual ownership and links: [target manifest](../../modules/targets.json). Generated build directories are not committed.
