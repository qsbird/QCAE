# qcae-cli

CLI arguments and local JSON request client.

Public API: Executable entry only.

Direct dependencies: `qcae_transport_local`, `qcae_contracts`

Boundary: No domain/application link; uses generated contracts and local transport only. Shared SDK request migration is future work.

Minimal consumer: `qcae-cli --help`; link `qcae-cli` in CMake.

Validation from the repository root after configuring/building the `QCAE_BUILD_IPC=ON` (desktop also requires IPC/storage):

```sh
ctest --test-dir build-local --output-on-failure -R "^(ipc|m1_ipc|m23_ipc)$"
```

Actual ownership and links: [target manifest](../../modules/targets.json). Generated build directories are not committed.
