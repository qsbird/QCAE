# qcae-desktop

Thin desktop process entry invoking the UI module.

Public API: Executable entry only.

Direct dependencies: `qcae_desktop_ui`

Boundary: No domain/application link.

Minimal consumer: `qcae-desktop --help`; link `qcae-desktop` in CMake.

Validation from the repository root after configuring/building the `QCAE_BUILD_DESKTOP=ON` (desktop also requires IPC/storage):

```sh
ctest --test-dir build-local --output-on-failure -R "^(desktop_smoke)$"
```

Actual ownership and links: [target manifest](../../modules/targets.json). Generated build directories are not committed.
