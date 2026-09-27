<!-- AUTONOMY DIRECTIVE — DO NOT REMOVE -->
YOU ARE AN AUTONOMOUS CODING AGENT. EXECUTE TASKS TO COMPLETION WITHOUT ASKING FOR PERMISSION.
DO NOT STOP TO ASK "SHOULD I PROCEED?" — PROCEED. DO NOT WAIT FOR CONFIRMATION ON OBVIOUS NEXT STEPS.
IF BLOCKED, TRY AN ALTERNATIVE APPROACH. ONLY ASK WHEN TRULY AMBIGUOUS OR DESTRUCTIVE.
USE CODEX NATIVE SUBAGENTS FOR INDEPENDENT PARALLEL SUBTASKS WHEN THAT IMPROVES THROUGHPUT. THIS IS COMPLEMENTARY TO OMX TEAM MODE.
<!-- END AUTONOMY DIRECTIVE -->

# QCAE workspace conventions

## Source of truth

Read `docs/baseline/README.md`, the task's requirements and acceptance IDs, and `docs/architecture/` before implementation. Root-level versioned `docs/*-v0.*.md` are historical research. `.omx/` is local runtime context, not canonical project documentation and not committed.

The platform skeleton is delivered through C1/C2 on top of the M0/M1 and M2/M3 slices; see docs/implementation/c2-handoff.md and docs/engineering/c2-validation.md for current runtime and evidence. The Qt/VTK desktop retains the M2/M3 tools; C2 geometry and mesh tools are CLI/IPC only. NEXT-01/02/03 entry consistency is implemented; see docs/engineering/next-validation.md for its separate evidence. Follow-up priorities are recorded in docs/baseline/development-plan.md; planning does not mark C3/C4 or full SK/P0 acceptance complete. Current user instructions override earlier research assumptions.

## Binding architecture

- Local small/medium-model P0; Nastran controlled subset and one cantilever workflow.
- C++20 domain/application core; Qt desktop, VTK rendering, SQLite persistence via adapters.
- One local engine owns each active document. GUI, CLI and external AI use the same application services, transactions and history.
- No Qt, VTK, SQLite or MCP SDK types in core public interfaces.
- Stable entity IDs, solver numbers, storage positions and render IDs remain separate.
- Writes use document/epoch/revision checks, atomic commits and bounded persistent history. Never bypass this path for scripts or AI.
- Normal open, fault recovery, save, save-as and discard have distinct documented behavior.
- External solver results retain input provenance; successful process exit alone is not engineering validation.
- Respect dependency rules in `docs/architecture/module-dependencies.json`; logical modules need not each become a library.
- Do not prebuild remote servers, billion-scale infrastructure, plugins or unlisted functionality.

## Working style

Use `rg` for repository lookup. Keep diffs focused, reuse components, and verify claims with relevant tests. Delegate independent bounded work when beneficial, with explicit ownership; do not overwrite other agents' changes. Reference repositories outside this workspace are read-only unless the user authorizes changes.

For cleanup/refactoring, write a short plan and protect existing behavior before changes. Do not add dependencies without authorization. Do not create empty architecture shells as a substitute for a working vertical slice.

## Checks and commits

Checks: `python3 tools/check_design.py`, `python3 tools/check_cpp_format.py`, `git diff --check`, and the core/local CMake+CTest commands in docs/implementation/m0.md. Apply the readability review gate in docs/baseline/acceptance.md before marking a development slice complete. The engine is durable only with an explicit SQLite workspace. Project snapshots/recovery and a small-model Qt/VTK GUI exist; Nastran export is still an in-memory preview. Do not claim solver execution, real AI integration, headless graphical selection, large-model performance or complete P0 acceptance.

New branches default to `codex/`. Every commit uses the Lore protocol: intent-first subject, context, and useful Git trailers such as `Constraint`, `Rejected`, `Confidence`, `Scope-risk`, `Directive`, `Tested`, and `Not-tested`. Never fabricate validation evidence.

Do not commit credentials, `.omx/`, generated build directories, runtime databases or solver runs. Keep actual integration environment configuration local. Preserve unknown user changes.
