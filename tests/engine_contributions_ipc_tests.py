#!/usr/bin/env python3
"""Exercise a test-only record contribution through real engine IPC and SQLite."""
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


def revision(info: dict) -> int:
    return int(info["revision"])


def relation_rows(client: Client, context: dict | None = None) -> list[dict]:
    data = client.call("entity.query", {"kind": "test_relation"}, context or client.current())
    return data["entities"]


def fields(client: Client, identity: str) -> dict:
    data = client.call("entity.fields", {"entity_id": identity}, client.current())
    return data["fields"]


def relation_create(client: Client, context: dict, key: str, from_id: str, to_id: str,
                    name: str, expected: str = "success") -> dict:
    return client.call("test.relation.create",
                       {"from_id": from_id, "to_id": to_id, "name": name},
                       context, key, expected)


class EngineProcess:
    def __init__(self, binary: str, cli: str, endpoint: str, workspace: str,
                 log_file) -> None:
        self.binary = binary
        self.cli = cli
        self.endpoint = endpoint
        self.workspace = workspace
        self.log_file = log_file
        self.client = Client("cli", cli, endpoint)
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

    def kill(self) -> None:
        check(self.process is not None, "engine is not running")
        self.process.kill()
        self.process.wait(timeout=10)
        self.process = None

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


def wait_for_mesh(client: Client, context: dict, geometry_id: str) -> list[str]:
    started = client.call("mesh.generate_line",
                          {"geometry_id": geometry_id, "segments": 1},
                          context, "contribution-endpoint-mesh")
    task_id = started["task_id"]
    deadline = time.monotonic() + 20
    task = started
    while task["state"] not in {"succeeded", "failed", "cancelled", "conflicted"}:
        check(time.monotonic() < deadline, f"mesh task did not finish: {task}")
        time.sleep(0.01)
        task = client.call("task.status", {"task_id": task_id}, client.current())
    check(task["state"] == "succeeded", f"mesh task failed: {task}")

    nodes = client.call("entity.query", {"kind": "node"}, client.current())["entities"]
    check(len(nodes) == 2, f"one line segment should create two nodes, got {nodes}")
    by_x = sorted((fields(client, node["entity_id"])["position"][0], node["entity_id"])
                  for node in nodes)
    check(by_x[0][0] < by_x[1][0], "line endpoints did not have increasing x positions")
    return [by_x[0][1], by_x[1][1]]


def assert_history_unchanged(client: Client, before_revision: str, before_history: dict,
                             label: str) -> None:
    after_info = client.current()
    after_history = client.call("history.list", context=after_info)
    check(after_info["revision"] == before_revision,
          f"{label} advanced the document revision")
    check(after_history["items"] == before_history["items"] and
          after_history["cursor"] == before_history["cursor"],
          f"{label} changed persistent history")


def assert_relation(client: Client, identity: str, from_id: str, to_id: str,
                    name: str) -> None:
    actual = fields(client, identity)
    check(actual == {"from": from_id, "to": to_id, "name": name},
          f"relation fields differ for {identity}: {actual}")


def test_contribution(engine: EngineProcess, project_path: Path) -> dict:
    caps = engine.start()
    check(caps["durable"] is True and caps["storage_mode"] == "sqlite",
          f"contribution engine is not backed by SQLite: {caps}")
    capabilities = {item["name"]: item for item in caps["operations"]}
    relation_capability = capabilities.get("test.relation.create")
    check(relation_capability is not None and relation_capability["available"] is True,
          "test relation handler is not present in engine capabilities")
    check(relation_capability["version"] == 1,
          f"unexpected relation contract version: {relation_capability}")

    created = engine.client.call("project.create", {"name": "Contribution IPC"},
                                 key="contribution-project")
    endpoint = engine.client.call(
        "geometry.create_line",
        {"start_mm": [0, 0, 0], "end_mm": [1000, 0, 0]},
        created,
        "contribution-endpoint-line",
    )
    nodes = wait_for_mesh(engine.client, engine.client.current(), endpoint["entity_id"])
    left, right = nodes

    before_info = engine.client.current()
    before_history = engine.client.call("history.list", context=before_info)
    invalid_requests = [
        ("reversed", right, left, "reversed relation"),
        ("self", left, left, "self relation"),
        ("missing", left, "missing-node", "missing endpoint"),
    ]
    for suffix, from_id, to_id, name in invalid_requests:
        rejected = relation_create(engine.client, engine.client.current(),
                                   f"invalid-{suffix}", from_id, to_id, name, "failed")
        check(rejected.get("error", {}).get("code") not in {None, ""},
              f"{suffix} relation rejection lacks a diagnostic: {rejected}")
        assert_history_unchanged(engine.client, before_info["revision"], before_history,
                                 f"{suffix} relation rejection")

    relation_name = "saved test relation"
    create_context = engine.client.current()
    created_relation = relation_create(engine.client, create_context,
                                       "saved-relation", left, right, relation_name)
    relation_id = created_relation["entity_id"]
    check(bool(relation_id), f"typed relation handler returned no entity ID: {created_relation}")
    check(revision(engine.client.current()) == revision(create_context) + 1,
          "valid relation creation did not advance exactly one revision")
    replay = relation_create(engine.client, create_context, "saved-relation",
                             left, right, relation_name)
    check(replay["entity_id"] == relation_id and
          replay["transaction_id"] == created_relation["transaction_id"] and
          replay["replayed"] is True,
          f"idempotent retry did not return the original relation fact: {replay}")

    all_entities = engine.client.call("entity.query", {}, engine.client.current())["entities"]
    all_matches = [item for item in all_entities if item["entity_id"] == relation_id]
    check(len(all_matches) == 1, "all-entity enumeration omitted or duplicated the relation")
    summary = all_matches[0]
    check(summary.get("kind") == "test_relation" and summary.get("name") == relation_name and
          summary.get("from_id") == left and summary.get("to_id") == right,
          f"generic relation JSON summary is incomplete: {summary}")

    by_kind = relation_rows(engine.client)
    check([item["entity_id"] for item in by_kind] == [relation_id],
          f"kind query returned the wrong relation set: {by_kind}")
    by_id = engine.client.call("entity.query", {"ids": [relation_id]},
                               engine.client.current())["entities"]
    check([item["entity_id"] for item in by_id] == [relation_id],
          f"ID query returned the wrong relation set: {by_id}")
    by_name = engine.client.call("entity.query", {"name_contains": relation_name},
                                 engine.client.current())["entities"]
    check([item["entity_id"] for item in by_name] == [relation_id],
          f"name query returned the wrong relation set: {by_name}")
    assert_relation(engine.client, relation_id, left, right, relation_name)

    outgoing = engine.client.call("entity.references",
                                  {"entity_id": relation_id, "direction": "outgoing"},
                                  engine.client.current())["references"]
    check({(item["from"], item["to"]) for item in outgoing} ==
          {(relation_id, left), (relation_id, right)},
          f"outgoing relation references are wrong: {outgoing}")
    incoming = engine.client.call("entity.references",
                                  {"entity_id": left, "direction": "incoming"},
                                  engine.client.current())["references"]
    check(any(item["from"] == relation_id and item["to"] == left for item in incoming),
          f"incoming node references omit the relation: {incoming}")

    before_undo = engine.client.current()
    engine.client.call("history.undo", context=before_undo, key="undo-saved-relation")
    after_undo = engine.client.current()
    check(revision(after_undo) == revision(before_undo) + 1,
          "undo did not advance exactly one revision")
    missing = engine.client.call("entity.fields", {"entity_id": relation_id}, after_undo,
                                 expected="failed")
    check(missing.get("error", {}).get("code") == "ENTITY_NOT_FOUND",
          f"undo left the relation visible: {missing}")
    before_redo = engine.client.current()
    engine.client.call("history.redo", context=before_redo, key="redo-saved-relation")
    after_redo = engine.client.current()
    check(revision(after_redo) == revision(before_redo) + 1,
          "redo did not advance exactly one revision")
    assert_relation(engine.client, relation_id, left, right, relation_name)

    saved = engine.client.call("project.save", {"path": str(project_path)},
                               engine.client.current(), "save-relation-project")
    check(saved["dirty"] is False, f"saved project remains dirty: {saved}")

    unsaved_name = "unsaved test relation"
    unsaved_created = relation_create(engine.client, engine.client.current(),
                                      "unsaved-relation", left, right, unsaved_name)
    unsaved_id = unsaved_created["entity_id"]
    before_crash = engine.client.current()
    engine.kill()

    recovered_caps = engine.start()
    check(recovered_caps.get("recovery_available") is True,
          f"SQLite restart did not expose recovery: {recovered_caps}")
    recovered = engine.client.call("project.open", {"mode": "recover"}, key="recover-relation")
    check(recovered["document_id"] == before_crash["document_id"] and
          recovered["document_epoch"] != before_crash["document_epoch"],
          f"recovery did not retain document identity and advance epoch: {recovered}")
    check(recovered["revision"] == before_crash["revision"],
          "recovery changed the persisted revision")
    assert_relation(engine.client, relation_id, left, right, relation_name)
    assert_relation(engine.client, unsaved_id, left, right, unsaved_name)

    before_recovery_undo = engine.client.current()
    engine.client.call("history.undo", context=before_recovery_undo,
                       key="undo-unsaved-after-recovery")
    after_recovery_undo = engine.client.current()
    check(revision(after_recovery_undo) == revision(before_recovery_undo) + 1,
          "post-recovery undo did not advance exactly one revision")
    absent_unsaved = engine.client.call("entity.fields", {"entity_id": unsaved_id},
                                        after_recovery_undo, expected="failed")
    check(absent_unsaved.get("error", {}).get("code") == "ENTITY_NOT_FOUND",
          f"post-recovery undo did not remove the unsaved relation: {absent_unsaved}")
    assert_relation(engine.client, relation_id, left, right, relation_name)
    before_recovery_redo = engine.client.current()
    engine.client.call("history.redo", context=before_recovery_redo,
                       key="redo-unsaved-after-recovery")
    after_recovery_redo = engine.client.current()
    check(revision(after_recovery_redo) == revision(before_recovery_redo) + 1,
          "post-recovery redo did not advance exactly one revision")
    assert_relation(engine.client, unsaved_id, left, right, unsaved_name)

    engine.client.call("project.close", {"policy": "discard"},
                       engine.client.current(), "discard-recovered-relation-project")
    opened = engine.client.call("project.open", {"mode": "normal", "path": str(project_path)},
                                key="open-saved-relation-project")
    check(opened["document_id"] != recovered["document_id"],
          "normal open reused the recovered document identity")
    saved_relations = relation_rows(engine.client, opened)
    check({item["entity_id"] for item in saved_relations} == {relation_id},
          f"normal open did not restore the saved relation state: {saved_relations}")
    assert_relation(engine.client, relation_id, left, right, relation_name)
    absent_after_open = engine.client.call("entity.fields", {"entity_id": unsaved_id}, opened,
                                           expected="failed")
    check(absent_after_open.get("error", {}).get("code") == "ENTITY_NOT_FOUND",
          f"normal open included an unsaved relation: {absent_after_open}")

    return {"relation_id": relation_id, "unsaved_relation_id": unsaved_id,
            "endpoint_ids": nodes, "recovery_verified": True,
            "normal_open_verified": True, "requests": len(engine.client.transcript)}


def test_production_binary(engine: EngineProcess) -> dict:
    caps = engine.start()
    check(caps["durable"] is True and caps["storage_mode"] == "sqlite",
          f"production comparison engine is not backed by SQLite: {caps}")
    names = {item["name"] for item in caps["operations"]}
    check("test.relation.create" not in names,
          "production engine advertises the test-only relation operation")
    unsupported = engine.client.call(
        "test.relation.create",
        {"from_id": "from", "to_id": "to", "name": "must stay unavailable"},
        expected="failed",
    )
    check(unsupported.get("error", {}).get("code") == "UNSUPPORTED_CAPABILITY",
          f"production engine did not reject the test-only operation: {unsupported}")
    created = engine.client.call("project.create", {"name": "Production composition"},
                                 key="production-project")
    check(bool(created.get("document_id")) and bool(created.get("document_epoch")),
          f"production project creation failed after unsupported operation: {created}")
    return {"test_operation_absent": True, "project_created": True,
            "requests": len(engine.client.transcript)}


def run_case(name: str, binary: str, cli: str, root: Path, evidence_dir: Path,
             scenario, project_path: Path | None = None) -> dict:
    case_dir = root / name
    case_dir.mkdir(parents=True)
    endpoint = str(case_dir / "engine.sock")
    workspace = str(case_dir / "work.sqlite")
    log_path = evidence_dir / f"{name}-engine.log"
    transcript_path = evidence_dir / f"{name}-transcript.json"
    result = {"case": name, "binary": binary, "status": "failed"}
    with log_path.open("w+", encoding="utf-8") as log_file:
        engine = EngineProcess(binary, cli, endpoint, workspace, log_file)
        try:
            detail = scenario(engine, project_path) if project_path else scenario(engine)
            result.update(detail)
            result["status"] = "passed"
        except Exception as error:  # Evidence is persisted before the failure is reported.
            result["error"] = f"{type(error).__name__}: {error}"
        finally:
            engine.close()
            log_file.flush()
            log_file.seek(0)
            transcript_path.write_text(json.dumps(engine.client.transcript, indent=2) + "\n",
                                       encoding="utf-8")
            (evidence_dir / f"{name}-engine.log").write_text(log_file.read(), encoding="utf-8")
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True,
                        help="test-only engine binary with the relation contribution")
    parser.add_argument("--production-engine", required=True,
                        help="normal engine binary without the test contribution")
    parser.add_argument("--cli", required=True)
    parser.add_argument("--evidence-dir", required=True, type=Path)
    args = parser.parse_args()

    evidence_dir = args.evidence_dir
    evidence_dir.mkdir(parents=True, exist_ok=True)
    results: list[dict] = []
    try:
        with tempfile.TemporaryDirectory(prefix="qei-", dir=Path("/tmp").resolve()) as temp:
            root = Path(temp)
            saved_project = root / "saved-relation.qcae"
            results.append(run_case("contribution", str(Path(args.engine).resolve()),
                                    str(Path(args.cli).resolve()), root, evidence_dir,
                                    test_contribution, saved_project))
            write_summary(evidence_dir, results)
            results.append(run_case("production", str(Path(args.production_engine).resolve()),
                                    str(Path(args.cli).resolve()), root, evidence_dir,
                                    test_production_binary))
    except Exception as error:
        results.append({"case": "harness", "status": "failed",
                        "error": f"{type(error).__name__}: {error}"})
    write_summary(evidence_dir, results)
    passed = len(results) == 2 and all(result.get("status") == "passed" for result in results)
    if not passed:
        for result in results:
            if result.get("status") != "passed":
                print(json.dumps(result, sort_keys=True))
        return 1
    print("PASS: test-only relation contribution over real engine IPC/SQLite; production capability absent")
    return 0


def write_summary(evidence_dir: Path, results: list[dict]) -> None:
    summary = {"suite": "engine_contributions_ipc", "passed": sum(
        result.get("status") == "passed" for result in results),
        "total": 2, "results": results}
    (evidence_dir / "engine-contributions-summary.json").write_text(
        json.dumps(summary, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    raise SystemExit(main())
