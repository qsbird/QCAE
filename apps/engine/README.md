# qcae-engine

Engine process setup, socket lifecycle and concrete adapter composition.

Public API: `qcae_run_engine` in `qcae/engine_host.hpp` runs the actual server with static contributions; `main.cpp` selects production defaults.

Targets: `qcae-engine` links the concrete `qcae_engine_host` library, which links `qcae_engine_api`, `qcae_transport_local`, `qcae_nastran`, and optional SQLite.

Boundary: Hosts one application; never links desktop rendering. Existing frame handling remains here.

Minimal consumer: `qcae-engine --help`; a custom static entry links `qcae_engine_host`. The test contribution executable uses this same server without installing test types in production.

Validation from the repository root after configuring/building the `QCAE_BUILD_IPC=ON` (desktop also requires IPC/storage):

```sh
ctest --test-dir build-local --output-on-failure -R "^(engine_assembly|entity_query_ipc|engine_contributions_ipc|ipc|m1_ipc|m23_ipc)$"
```

Actual ownership and links: [target manifest](../../modules/targets.json). Generated build directories are not committed.

C2 registers typed geometry, material, node/section editing and background line-mesh operations
on the same authoritative record application. Use `--workspace` for durable task rows and
history; normal startup still requires explicit `project.open` with `mode: "recover"` when a
workspace is recoverable. The wire table is in `adapters/engine_api/README.md`.

For a legacy workspace, `--migrate-from /absolute/source.sqlite --workspace /absolute/new.sqlite`
reads the quiescent source without opening a writer and publishes converted rows only into the
new target. The target must not exist. The engine then awaits explicit recovery; it does not
rewrite or delete the source.
