#!/usr/bin/env python3
"""NEXT C3: real engine/CLI geometry display and view lifecycle checks."""
from __future__ import annotations

import argparse
import json
import subprocess
import tempfile
import time
from pathlib import Path

from c2_workflow_tests import Client


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def integer(value: str) -> int:
    return int(value)


class EngineProcess:
    def __init__(self, binary: str, cli: str | None, endpoint: str, workspace: str,
                 log_file) -> None:
        self.binary = binary
        self.endpoint = endpoint
        self.workspace = workspace
        self.log_file = log_file
        self.client = Client("cli" if cli else "script", cli, endpoint)
        self.process: subprocess.Popen | None = None

    def start(self) -> dict:
        check(self.process is None, "engine is already running")
        self.process = subprocess.Popen(
            [self.binary, "--socket", self.endpoint, "--workspace", self.workspace],
            stdout=self.log_file,
            stderr=self.log_file,
        )
        deadline = time.monotonic() + 20
        last_error: Exception | None = None
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                raise AssertionError(f"engine exited during startup: {self.process.returncode}")
            try:
                return self.client.call("capabilities.list")
            except (OSError, RuntimeError, subprocess.SubprocessError) as error:
                last_error = error
                time.sleep(0.05)
        raise AssertionError(f"engine startup timed out: {last_error}")

    def close(self) -> None:
        if self.process is None:
            return
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=5)
        self.process = None


def select(client: Client, view: dict, context: dict, predicate: dict,
           scope: dict | None = None, expected: str = "success") -> dict:
    parameters = {
        "view_session_id": view["view_session_id"],
        "expected_view_revision": view["view_revision"],
        "predicate": predicate,
        "scope": scope or {"visibility": "through", "include_hidden": True},
    }
    return client.call("selection.evaluate", parameters, context, expected=expected)


def selection_ids(client: Client, selection: dict, context: dict) -> list[str]:
    page = client.call("selection.get", {"selection_handle": selection["selection_handle"]},
                       context)
    return page["entity_ids"]


def render(client: Client, view: dict, context: dict, expected: str = "success") -> dict:
    return client.call("view.render_data",
                       {"view_session_id": view["view_session_id"],
                        "expected_view_revision": view["view_revision"]},
                       context,
                       expected=expected)


def create_view(client: Client, context: dict, hidden_ids: list[str] | None = None) -> dict:
    return client.call("view.create",
                       {"hidden_ids": hidden_ids or [], "camera_fingerprint": "c3-ipc"},
                       context)


def wait_for_mesh(client: Client, context: dict, geometry_id: str) -> dict:
    task = client.call("mesh.generate_line",
                       {"geometry_id": geometry_id, "segments": 1},
                       context,
                       "c3-display-mesh")
    task_id = task["task_id"]
    deadline = time.monotonic() + 20
    while task["state"] not in {"succeeded", "failed", "cancelled", "conflicted"}:
        check(time.monotonic() < deadline, f"mesh task did not finish: {task}")
        time.sleep(0.01)
        task = client.call("task.status", {"task_id": task_id}, client.current())
    check(task["state"] == "succeeded", f"mesh task failed: {task}")
    return task


def rows(client: Client, kind: str, context: dict) -> list[dict]:
    return client.call("entity.query", {"kind": kind}, context)["entities"]


def geometry_lines(render_data: dict, geometry_id: str,
                   expected_start: list[int], expected_end: list[int]) -> None:
    expected = [{"entity_id": geometry_id,
                 "start_mm": expected_start,
                 "end_mm": expected_end}]
    check(render_data.get("geometry_lines") == expected,
          f"geometry display does not match the authoritative line: {render_data}")


def run_scenario(engine: EngineProcess, project_path: Path) -> dict:
    capabilities = engine.start()
    check(capabilities["durable"] is True and capabilities["storage_mode"] == "sqlite",
          f"display test is not using SQLite: {capabilities}")

    created = engine.client.call("project.create", {"name": "C3 geometry display"},
                                 key="c3-display-project")
    line = engine.client.call("geometry.create_line",
                              {"start_mm": [0, 0, 0], "end_mm": [1000, 0, 0]},
                              created,
                              "c3-display-line")
    geometry_id = line["entity_id"]
    before_mesh = engine.client.current()
    check(rows(engine.client, "node", before_mesh) == [],
          "geometry.create_line fabricated mesh nodes")
    check(rows(engine.client, "beam", before_mesh) == [],
          "geometry.create_line fabricated beam elements")

    view = create_view(engine.client, before_mesh)
    initial_render = render(engine.client, view, before_mesh)
    geometry_lines(initial_render, geometry_id, [0, 0, 0], [1000, 0, 0])
    check(initial_render["points"] == [] and initial_render["beams"] == [],
          f"unmeshed geometry must not masquerade as nodes/beams: {initial_render}")

    crossing_box = {"op": "world_box", "minimum": [499, -1, -1],
                    "maximum": [501, 1, 1], "relation": "intersects"}
    picker_scope = {"candidate_ids": [geometry_id], "include_hidden": False,
                    "visibility": "picker_candidates", "invert": False}
    selected = select(engine.client, view, before_mesh, crossing_box, picker_scope)
    check(selected["count"] == 1 and selection_ids(engine.client, selected, before_mesh) ==
          [geometry_id], f"world-box picker omitted the line: {selected}")
    outside_candidates = {**picker_scope, "candidate_ids": []}
    selected = select(engine.client, view, before_mesh, crossing_box, outside_candidates)
    check(selected["count"] == 0 and selection_ids(engine.client, selected, before_mesh) == [],
          "picker selection escaped its explicit candidate set")
    contained_box = {"op": "world_box", "minimum": [100, -1, -1],
                     "maximum": [900, 1, 1], "relation": "contained"}
    selected = select(engine.client, view, before_mesh, contained_box, picker_scope)
    check(selected["count"] == 0,
          "contained world-box selection accepted a line extending outside the box")

    hidden = engine.client.call("view.update",
                                {"view_session_id": view["view_session_id"],
                                 "expected_view_revision": view["view_revision"],
                                 "hidden_ids": [geometry_id],
                                 "camera_fingerprint": "c3-ipc"},
                                before_mesh)
    hidden_selection = select(engine.client, hidden, engine.client.current(), crossing_box,
                              picker_scope)
    check(hidden_selection["count"] == 0,
          "picker selected a hidden geometry line without include_hidden")
    include_hidden = {**picker_scope, "include_hidden": True}
    hidden_selection = select(engine.client, hidden, engine.client.current(), crossing_box,
                              include_hidden)
    check(hidden_selection["count"] == 1,
          "include_hidden failed to select the hidden line in the explicit candidate set")
    hidden_render = render(engine.client, hidden, engine.client.current())
    check(hidden_render.get("geometry_lines") == [],
          f"hidden geometry remained in render data: {hidden_render}")
    stale_view = render(engine.client, view, engine.client.current(), expected="conflict")
    check(stale_view.get("error", {}).get("code") == "REVISION_CONFLICT",
          f"stale view revision was not rejected: {stale_view}")

    shown = engine.client.call("view.update",
                               {"view_session_id": hidden["view_session_id"],
                                "expected_view_revision": hidden["view_revision"],
                                "hidden_ids": [],
                                "camera_fingerprint": "c3-ipc"},
                               engine.client.current())
    shown_context = engine.client.current()
    geometry_lines(render(engine.client, shown, shown_context), geometry_id,
                   [0, 0, 0], [1000, 0, 0])
    shown_selection = select(engine.client, shown, shown_context, crossing_box, picker_scope)
    check(shown_selection["count"] == 1,
          "showing a line did not restore picker selection")

    mesh_task = wait_for_mesh(engine.client, engine.client.current(), geometry_id)
    after_mesh = engine.client.current()
    check(integer(after_mesh["revision"]) == integer(before_mesh["revision"]) + 1,
          "mesh task did not commit exactly one model revision")
    for stale_context in (before_mesh,):
        stale_model = render(engine.client, shown, stale_context, expected="conflict")
        check(stale_model.get("error", {}).get("code") == "REVISION_CONFLICT",
              f"stale document revision was not rejected: {stale_model}")
    stale_model_view = render(engine.client, shown, after_mesh, expected="conflict")
    check(stale_model_view.get("error", {}).get("code") == "REVISION_CONFLICT",
          f"view from an older model revision was not rejected: {stale_model_view}")

    node_ids = sorted(row["entity_id"] for row in rows(engine.client, "node", after_mesh))
    beam_ids = sorted(row["entity_id"] for row in rows(engine.client, "beam", after_mesh))
    check(len(node_ids) == 2 and len(beam_ids) == 1,
          f"one line segment should create two nodes and one beam: {node_ids}, {beam_ids}")
    mesh_view = create_view(engine.client, after_mesh)
    mesh_render = render(engine.client, mesh_view, after_mesh)
    geometry_lines(mesh_render, geometry_id, [0, 0, 0], [1000, 0, 0])
    check({point["entity_id"] for point in mesh_render["points"]} == set(node_ids) and
          {beam["entity_id"] for beam in mesh_render["beams"]} == set(beam_ids),
          f"meshed geometry and FE entities were not displayed together: {mesh_render}")

    before_undo = engine.client.current()
    engine.client.call("history.undo", context=before_undo, key="c3-undo-mesh")
    after_undo = engine.client.current()
    check(integer(after_undo["revision"]) == integer(before_undo["revision"]) + 1,
          "mesh undo did not advance one revision")
    check([row["entity_id"] for row in rows(engine.client, "geometry", after_undo)] ==
          [geometry_id] and rows(engine.client, "node", after_undo) == [] and
          rows(engine.client, "beam", after_undo) == [],
          "undo removed source geometry or retained generated FE entities")
    undo_view = create_view(engine.client, after_undo)
    undo_render = render(engine.client, undo_view, after_undo)
    geometry_lines(undo_render, geometry_id, [0, 0, 0], [1000, 0, 0])
    check(undo_render["points"] == [] and undo_render["beams"] == [],
          f"undo render did not restore geometry-only display: {undo_render}")

    before_redo = engine.client.current()
    engine.client.call("history.redo", context=before_redo, key="c3-redo-mesh")
    after_redo = engine.client.current()
    check(integer(after_redo["revision"]) == integer(before_redo["revision"]) + 1,
          "mesh redo did not advance one revision")
    check({row["entity_id"] for row in rows(engine.client, "node", after_redo)} == set(node_ids) and
          {row["entity_id"] for row in rows(engine.client, "beam", after_redo)} == set(beam_ids),
          "redo did not restore the same generated entity IDs")

    saved_path = str(project_path)
    saved = engine.client.call("project.save", {"path": saved_path}, after_redo,
                               "c3-save-display")
    check(saved["dirty"] is False, f"saved project remains dirty: {saved}")
    engine.client.call("project.close", {"policy": "discard"}, engine.client.current(),
                       "c3-close-display")
    opened = engine.client.call("project.open", {"mode": "normal", "path": saved_path},
                                key="c3-open-display")
    check(opened["document_id"] != created["document_id"],
          "normal open reused the original working document ID")
    check({row["entity_id"] for row in rows(engine.client, "geometry", opened)} ==
          {geometry_id} and
          {row["entity_id"] for row in rows(engine.client, "node", opened)} == set(node_ids) and
          {row["entity_id"] for row in rows(engine.client, "beam", opened)} == set(beam_ids),
          "normal project open changed persisted geometry or generated entity IDs")
    reopened_view = create_view(engine.client, opened)
    reopened_render = render(engine.client, reopened_view, opened)
    geometry_lines(reopened_render, geometry_id, [0, 0, 0], [1000, 0, 0])
    check({point["entity_id"] for point in reopened_render["points"]} == set(node_ids) and
          {beam["entity_id"] for beam in reopened_render["beams"]} == set(beam_ids),
          f"fresh view after normal open lost persisted FE display: {reopened_render}")

    return {"geometry_id": geometry_id, "node_ids": node_ids, "beam_ids": beam_ids,
            "mesh_task_id": mesh_task["task_id"], "hidden_show_verified": True,
            "stale_version_refusal_verified": True, "normal_open_verified": True,
            "requests": len(engine.client.transcript)}


def run_case(engine_binary: str, cli: str | None, evidence_dir: Path) -> dict:
    result = {"case": "c3_display", "binary": engine_binary, "status": "failed"}
    log_path = evidence_dir / "c3-display-engine.log"
    transcript_path = evidence_dir / "c3-display-transcript.json"
    with tempfile.TemporaryDirectory(prefix="qc3-", dir=Path("/tmp").resolve()) as folder:
        root = Path(folder)
        endpoint = str(root / "engine.sock")
        workspace = str(root / "work.sqlite")
        project_path = root / "geometry.qcae"
        with log_path.open("w+", encoding="utf-8") as log_file:
            engine = EngineProcess(engine_binary, cli, endpoint, workspace, log_file)
            try:
                result.update(run_scenario(engine, project_path))
                result["status"] = "passed"
            except Exception as error:  # Persist request/response and server evidence on failure.
                result["error"] = f"{type(error).__name__}: {error}"
            finally:
                engine.close()
                log_file.flush()
                log_file.seek(0)
                transcript_path.write_text(json.dumps(engine.client.transcript, indent=2) + "\n",
                                           encoding="utf-8")
                log_file.flush()
                log_file.seek(0)
                log_text = log_file.read()
        log_path.write_text(log_text, encoding="utf-8")
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True)
    parser.add_argument("--cli", help="Use qcae-cli; omit to use a raw local IPC client")
    parser.add_argument("--evidence-dir", required=True, type=Path)
    args = parser.parse_args()
    engine_binary = str(Path(args.engine).resolve())
    cli = str(Path(args.cli).resolve()) if args.cli else None
    args.evidence_dir.mkdir(parents=True, exist_ok=True)

    result = run_case(engine_binary, cli, args.evidence_dir)
    summary = {"suite": "c3_display_ipc", "passed": int(result["status"] == "passed"),
               "total": 1, "results": [result]}
    (args.evidence_dir / "c3-display-summary.json").write_text(
        json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    if result["status"] != "passed":
        print(json.dumps(result, sort_keys=True))
        return 1
    print("PASS: C3 GeometryLine display, world-box/picker visibility, task mesh, history and reopen")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
