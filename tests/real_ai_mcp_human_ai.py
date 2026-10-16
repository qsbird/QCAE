#!/usr/bin/env python3
"""Real AI/MCP TST-I10, headless half: AI and a human client share one model and its history.

The AI builds over MCP, the real qcae-cli (a separate client process, standing in for the desktop's
edit) changes the force, the AI undoes the latest change, and the harness saves, closes and reopens
the project before the AI reads it again. Verdicts come from engine facts and the engine responses in
the AI's call log.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import time

from c3_sync_ipc_tests import EngineProcess
import real_ai_claude_transport as claude
from real_ai_mcp_clarification import engine_fact

ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT / "tests/fixtures/nastran-real-benchmark-v3"
RESOURCES = ("cantilever.bdf", "nodes.bdf", "beams.bdf", "properties.bdf")
TOOLS = ["capabilities.list", "project.current", "entity.query", "entity.fields", "model.summary",
         "history.list", "history.undo", "material.create"]
POLICY = """You are the external AI client in an explicitly authorized QCAE test over public synthetic data. Use ONLY the qcae MCP tools. Do not use shell, files, other tools, network searches or credentials. The existing local engine owns the only model; a person edits the same model through another client, and everyone shares one change history. Read the actual document context before every write and use its current document_id, document_epoch and revision. Engine responses are facts: never report a state or value the engine did not report. Stop each turn with the requested JSON object. Do not claim P0 or formal acceptance."""


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def require(value, message):
    if not value:
        raise AssertionError(message)


def schema(properties):
    return {"type": "object", "additionalProperties": False, "properties": properties,
            "required": list(properties)}


def successes(result, tool):
    return [(call, engine_fact(call)) for call in result["calls"]
            if call["tool"] == tool and engine_fact(call)["status"] == "success"]


def model(client):
    current = client.current()
    materials = client.call("model.summary", context=current)["materials"]
    force = client.call("entity.query", {"kind": "force"}, current)["entities"][0]
    return current, materials, force


def human_force(cli, endpoint, context, force_id, y_newton):
    request = {"api_version": "1.1", "request_id": "human-force", "operation": "force.set_vector",
               "parameters": {"force_id": force_id, "x": {"value": 0, "unit": "N"},
                              "y": {"value": y_newton, "unit": "N"}, "z": {"value": 0, "unit": "N"}},
               "document_id": context["document_id"], "document_epoch": context["document_epoch"],
               "expected_revision": str(context["revision"]), "idempotency_key": "human-force"}
    done = subprocess.run([cli, "--socket", endpoint, "--no-start"], input=json.dumps(request),
                          capture_output=True, text=True, timeout=30)
    require(done.returncode == 0, f"qcae-cli failed: {done.stdout} {done.stderr}")
    response = json.loads(done.stdout)
    require(response.get("status") == "success", f"human force edit failed: {response}")
    return response


class Session:
    def __init__(self, host, server, facts, output, cli, root):
        self.host, self.client, self.server = host, host.client, server
        self.facts, self.output, self.cli, self.root = facts, output, cli, root

    def turn(self, name, prompt, output_schema):
        (self.output / f"prompt-{name}.txt").write_text(prompt)
        result = self.server.turn(prompt, "none", output_schema)
        self.facts["turns"][name] = result
        print(json.dumps({"turn": name, "calls": [c["tool"] for c in result["calls"]], "final": result["final"]}),
              flush=True)
        return result

    def run(self):
        client = self.client
        before, materials, force = model(client)
        document = before["document_id"]
        first = self.turn("model", "Create a material named Alu with Young's modulus 70 GPa and Poisson ratio 0.33 in "
                          "the current project. Output JSON: material_id.", schema({"material_id": {"type": "string"}}))
        after, rows, _ = model(client)
        alu = [row for row in rows if row["name"] == "Alu"]
        require(len(rows) == len(materials) + 1 and len(alu) == 1 and alu[0]["young_modulus_mpa"] == 70000,
                "not exactly one new Alu material at 70 GPa")
        require(int(after["revision"]) == int(before["revision"]) + 1 and after["document_id"] == document,
                "the AI's write was not one revision in the existing document")
        require(first["final"]["material_id"] == alu[0]["entity_id"], "AI misreported its material")
        human_force(self.cli, self.host.endpoint, after, force["entity_id"], -2)
        edited, _, moved = model(client)
        require(moved["force_n"] == [0, -2, 0], "the human's force edit did not land")
        history = client.call("history.list", context=edited)
        self.facts["shared_history"] = history
        second = self.turn("undo", "Someone else may have changed the model since your last action. Undo the most "
                           "recent change in the shared document history, whoever made it, and tell me what it "
                           "changed. Output JSON: undone_change (short description), force_y_n_after.",
                           schema({"undone_change": {"type": "string"}, "force_y_n_after": {"type": "number"}}))
        undone, rows, restored = model(client)
        require(successes(second, "history.undo"), "AI made no successful undo")
        require(restored["force_n"] == [0, -1, 0] and any(row["name"] == "Alu" for row in rows),
                "the undo did not revert exactly the human's force change")
        answer = second["final"]
        require("force" in answer["undone_change"].lower() and answer["force_y_n_after"] == -1,
                "AI misreported what it undid")
        saved = self.root / "shared.qcae"
        client.call("project.save", {"path": str(saved)}, undone, "harness-save")
        client.call("project.close", {"policy": "discard"}, client.current(), "harness-close")
        reopened = client.call("project.open", {"mode": "normal", "path": str(saved)}, key="harness-reopen")
        current, rows, force_after = model(client)
        self.facts["reopened"] = {"context": current, "materials": rows, "force": force_after}
        require(current["document_epoch"] != undone["document_epoch"], "reopening did not start a new epoch")
        third = self.turn("reopened", "The project was saved, closed and reopened. Read the reopened document and report "
                          "its material names and the Y component of its force in N. Output JSON: material_names, "
                          "force_y_n.", schema({"material_names": {"type": "array", "items": {"type": "string"}},
                                                "force_y_n": {"type": "number"}}))
        answer = third["final"]
        require(sorted(answer["material_names"]) == sorted(row["name"] for row in rows) and
                answer["force_y_n"] == force_after["force_n"][1], "AI report disagrees with the reopened model")
        reads = [call for call in third["calls"] if call["arguments"].get("document_epoch")]
        require(reads and all(call["arguments"]["document_epoch"] == current["document_epoch"]
                              for call in reads if engine_fact(call)["status"] == "success"),
                "AI's successful reads did not use the reopened epoch")
        self.facts["checked"] = {"document_id": document, "reopened_epoch": current["document_epoch"],
                                 "force_n": force_after["force_n"], "reopen_response": reopened}


def run_one(args, output):
    output.mkdir(parents=True, exist_ok=False)
    facts = {"schema_version": 1, "purpose": "real_ai_human_ai_headless_TST_I10", "formal_acceptance": False,
             "partial": True, "model": args.model, "engine_sha256": digest(args.engine),
             "cli_sha256": digest(args.cli), "bridge_sha256": digest(ROOT / "apps/mcp/bridge.py"),
             "harness_sha256": digest(__file__), "transport_sha256": digest(claude.__file__),
             "commit": subprocess.check_output(["git", "-C", str(ROOT), "rev-parse", "HEAD"], text=True).strip(),
             "data_destination": claude.DATA_DESTINATION, "turns": {}}
    started = time.monotonic()
    server = None
    with tempfile.TemporaryDirectory(prefix="qcae-ai-human-", dir=Path("/tmp").resolve()) as folder:
        root = Path(folder)
        endpoint = str(root / "engine.sock")
        with (output / "engine.log").open("w+") as log:
            host = EngineProcess(args.engine, endpoint, str(root / "work.sqlite"), log)
            try:
                capabilities = host.start()
                client = host.client
                profile = capabilities["declared_solver_profiles"][0]["profile_ref"]
                context = client.call("project.create", {"name": "Synthetic I10 shared model"}, key="fixture-project")
                resources = [{"path": name, "text": (FIXTURES / name).read_text()} for name in RESOURCES]
                preview = client.call("changes.preview", {"command": "model.import", "root_resource": RESOURCES[0],
                                      "resources": resources, "source_profile_ref": profile,
                                      "unit_system": "mm-N-MPa"}, context)
                client.call("changes.commit", {"preview_id": preview["preview_id"]}, context, "fixture-import")
                server = claude.ClaudeSession(args.client, endpoint, root, output, args.model, "none", TOOLS, POLICY)
                facts["session_id"] = server.thread
                Session(host, server, facts, output, args.cli, root).run()
                facts["passed"] = True
            except Exception as error:
                facts.update(passed=False, failure=f"{type(error).__name__}: {error}")
            finally:
                if server is not None:
                    server.close()
                host.close()
                facts["elapsed_seconds"] = time.monotonic() - started
                (output / "facts.json").write_text(json.dumps(facts, ensure_ascii=False, indent=2) + "\n")
    print(json.dumps({"scenario": "human_ai", "passed": facts["passed"], "failure": facts.get("failure"),
                      "elapsed_seconds": facts["elapsed_seconds"]}), flush=True)
    return facts["passed"]


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("client", "engine", "cli", "evidence-dir"):
        parser.add_argument("--" + name, required=True)
    parser.add_argument("--model", default="claude-haiku-4-5-20251001")
    arguments = parser.parse_args()
    for name in ("client", "engine", "cli"):
        setattr(arguments, name, str(Path(getattr(arguments, name)).resolve(strict=True)))
    require(claude.is_claude(arguments.client), "this diagnostic drives Claude Code")
    raise SystemExit(0 if run_one(arguments, Path(arguments.evidence_dir).resolve()) else 1)
