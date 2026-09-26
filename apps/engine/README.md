# qcae-engine

Engine process setup, socket lifecycle and concrete adapter composition.

Public API: Executable entry only.

Direct dependencies: `qcae_engine_api`, `qcae_transport_local`, `qcae_nastran`

Boundary: Hosts one application; never links desktop rendering. Existing frame handling remains here.

Minimal consumer: `qcae-engine --help`; link `qcae-engine` in CMake.

Validation from the repository root after configuring/building the `QCAE_BUILD_IPC=ON` (desktop also requires IPC/storage):

```sh
ctest --test-dir build-local --output-on-failure -R "^(ipc|m1_ipc|m23_ipc)$"
```

Actual ownership and links: [target manifest](../../modules/targets.json). Generated build directories are not committed.
