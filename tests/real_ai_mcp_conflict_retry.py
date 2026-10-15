#!/usr/bin/env python3
"""Real AI/MCP TST-I04 (stale preview conflict) and TST-I05 (retry after a lost response).

A second IPC client stands in for the desktop in I04, so that case is partial. In I05 the bridge
loses the response of the first changes.commit after the engine executed it. Verdicts come from
engine facts read by the harness and the engine responses in the AI's call log.
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
SCENARIOS = ("version_conflict", "lost_response")
TOOLS = ["capabilities.list", "project.current", "entity.query", "entity.fields", "model.summary",
         "history.list", "operations.get", "changes.preview", "changes.commit", "material.create"]
POLICY = """You are the external AI client in an explicitly authorized QCAE test over public synthetic data. Use ONLY the qcae MCP tools. Do not use shell, files, other tools, network searches or credentials. The existing local engine owns the only model; other people may change it at any time. Read the actual document context before writes and use its current document_id, document_epoch and revision. Engine responses are facts: never claim an outcome the engine did not report. If an outcome is unknown, find out what actually happened before acting again, and never create duplicates. Stop each turn with the requested JSON object. Do not claim P0 or formal acceptance."""


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def require(value, message):
    if not value:
        raise AssertionError(message)


def schema(properties):
    return {"type": "object", "additionalProperties": False, "properties": properties,
            "required": list(properties)}


def calls(result, tool):
    return [call for call in result["calls"] if call["tool"] == tool]


def materials(client):
    current = client.current()
    return current, client.call("model.summary", context=current)["materials"]


def turn(server, facts, output, name, prompt, output_schema):
    (output / f"prompt-{name}.txt").write_text(prompt)
    result = server.turn(prompt, "none", output_schema)
    facts["turns"][name] = result
    print(json.dumps({"turn": name, "calls": [c["tool"] for c in result["calls"]], "final": result["final"]}),
          flush=True)
    return result


def version_conflict(client, server, facts, output):
    client.call("project.create", {"name": "Synthetic I04 conflict"}, key="fixture-project")
    client.call("material.create", {"name": "Steel", "young_modulus": {"value": 210, "unit": "GPa"}},
                client.current(), "fixture-material")
    base, rows = materials(client)
    steel = rows[0]["entity_id"]
    first = turn(server, facts, output, "preview",
                 "Read the current project. Prepare, but do not commit, a change of the Steel material's Young's "
                 "modulus to 200 GPa with changes.preview. Output JSON: status, preview_id, base_revision.",
                 schema({"status": {"type": "string"}, "preview_id": {"type": "string"},
                         "base_revision": {"type": "string"}}))
    previews = [engine_fact(call) for call in calls(first, "changes.preview")]
    require(any(p["status"] == "success" for p in previews), "AI made no successful preview")
    require(not calls(first, "changes.commit") and client.current()["revision"] == base["revision"],
            "AI wrote while asked only to preview")
    # The other actor changes the same material, moving the document one revision on.
    preview = client.call("changes.preview", {"command": "material.set_young_modulus", "entity_id": steel,
                                              "young_modulus": {"value": 205, "unit": "GPa"}}, client.current())
    client.call("changes.commit", {"preview_id": preview["preview_id"]}, client.current(), "other-actor-commit")
    moved, rows = materials(client)
    require(int(moved["revision"]) == int(base["revision"]) + 1, "other actor's commit did not land")
    second = turn(server, facts, output, "stale_commit",
                  "Now commit the preview you prepared earlier. If the engine refuses it, do not change values or "
                  "retry differently; report what the engine said and stop. Output JSON: status (committed, "
                  "conflict or failed), engine_code, current_revision.",
                  schema({"status": {"type": "string", "enum": ["committed", "conflict", "failed"]},
                          "engine_code": {"type": "string"}, "current_revision": {"type": "string"}}))
    commits = [engine_fact(call) for call in calls(second, "changes.commit")]
    after, rows = materials(client)
    facts["stale_commit_responses"] = commits
    require(commits and all(c["status"] != "success" for c in commits), "a stale preview commit succeeded")
    require(after["revision"] == moved["revision"] and rows[0]["young_modulus_mpa"] == 205000,
            "the other actor's revision or value did not survive")
    require(second["final"]["status"] == "conflict", "AI did not report the conflict")
    third = turn(server, facts, output, "fresh_preview",
                 "Re-read the current state and prepare a new preview of the same change (Young's modulus 200 GPa) "
                 "against it. Do not commit. Output JSON: status, preview_id, base_revision.",
                 schema({"status": {"type": "string"}, "preview_id": {"type": "string"},
                         "base_revision": {"type": "string"}}))
    fresh = [engine_fact(call) for call in calls(third, "changes.preview")]
    require(any(p["status"] == "success" and p["data"]["revision"] == moved["revision"] for p in fresh),
            "no fresh preview against the current revision")
    require(not any(engine_fact(c)["status"] == "success" for c in calls(third, "changes.commit")) and
            client.current()["revision"] == moved["revision"], "AI committed while asked only to preview")
    facts["checked"] = {"base_revision": base["revision"], "other_actor_revision": moved["revision"],
                        "final_modulus_mpa": rows[0]["young_modulus_mpa"]}


def lost_response(client, server, facts, output, marker):
    client.call("project.create", {"name": "Synthetic I05 retry"}, key="fixture-project")
    base = client.current()
    history = client.call("history.list", context=base)
    result = turn(server, facts, output, "retry",
                  "Create one material named Aluminium with Young's modulus 70 GPa: preview it with changes.preview "
                  "and commit it with changes.commit using idempotency key 'ai-retry-material'. Output JSON: "
                  "status, material_count, committed_revision.",
                  schema({"status": {"type": "string"}, "material_count": {"type": "integer"},
                          "committed_revision": {"type": "string"}}))
    require(marker.exists(), "the injected response loss did not happen")
    after, rows = materials(client)
    after_history = client.call("history.list", context=after)
    commits = [(call["arguments"], engine_fact(call)) for call in calls(result, "changes.commit")]
    facts["commits"] = commits
    require(len(rows) == 1 and rows[0]["name"] == "Aluminium" and rows[0]["young_modulus_mpa"] == 70000,
            "not exactly one Aluminium material at 70 GPa")
    require(int(after["revision"]) == int(base["revision"]) + 1, "revision did not advance exactly once")
    require(len(after_history["items"]) == len(history["items"]) + 1, "history did not gain exactly one entry")
    for arguments, response in commits[1:]:
        require(arguments.get("idempotency_key") == "ai-retry-material",
                "a retry used a different idempotency key")
        if response["status"] == "success":
            require(response["data"].get("replayed") is True, "a retry was not the original transaction")
    require(not any(engine_fact(c)["status"] == "success" for c in calls(result, "material.create")),
            "AI created the material a second way")
    require(result["final"]["material_count"] == 1, "AI misreported the material count")
    # Engine property, checked by the harness: the same key with other parameters is refused.
    other = client.call("changes.preview", {"command": "material.create", "name": "Other",
                                            "young_modulus": {"value": 1, "unit": "GPa"}}, after)
    reuse = client.call("changes.commit", {"preview_id": other["preview_id"]}, after, "ai-retry-material",
                        expected=None)
    facts["same_key_other_parameters"] = reuse
    require(reuse.get("status") != "success", "same key with other parameters was not refused")
    require(len(materials(client)[1]) == 1, "same key with other parameters created a material")
    facts["checked"] = {"base_revision": base["revision"], "final_revision": after["revision"],
                        "commit_attempts": len(commits)}


def run_one(args, scenario, output):
    output.mkdir(parents=True, exist_ok=False)
    facts = {"schema_version": 1, "purpose": f"real_ai_{scenario}", "formal_acceptance": False,
             "partial": scenario == "version_conflict", "model": args.model,
             "engine_sha256": digest(args.engine), "bridge_sha256": digest(ROOT / "apps/mcp/bridge.py"),
             "lossy_bridge_sha256": digest(ROOT / "tests/real_ai_lossy_bridge.py"),
             "harness_sha256": digest(__file__), "transport_sha256": digest(claude.__file__),
             "commit": subprocess.check_output(["git", "-C", str(ROOT), "rev-parse", "HEAD"], text=True).strip(),
             "data_destination": claude.DATA_DESTINATION, "turns": {}}
    started = time.monotonic()
    server = None
    with tempfile.TemporaryDirectory(prefix=f"qcae-ai-{scenario}-", dir=Path("/tmp").resolve()) as folder:
        root = Path(folder)
        endpoint = str(root / "engine.sock")
        marker = root / "dropped-commit"
        with (output / "engine.log").open("w+") as log:
            host = EngineProcess(args.engine, endpoint, str(root / "work.sqlite"), log)
            try:
                host.start()
                bridge_args = None
                if scenario == "lost_response":
                    bridge_args = [str(ROOT / "tests/real_ai_lossy_bridge.py"), "--endpoint", endpoint,
                                   "--drop-marker", str(marker)]
                server = claude.ClaudeSession(args.client, endpoint, root, output, args.model, "none", TOOLS,
                                              POLICY, bridge_args)
                facts["session_id"] = server.thread
                if scenario == "version_conflict":
                    version_conflict(host.client, server, facts, output)
                else:
                    lost_response(host.client, server, facts, output, marker)
                facts["passed"] = True
            except Exception as error:
                facts.update(passed=False, failure=f"{type(error).__name__}: {error}")
            finally:
                if server is not None:
                    server.close()
                host.close()
                facts["elapsed_seconds"] = time.monotonic() - started
                (output / "facts.json").write_text(json.dumps(facts, ensure_ascii=False, indent=2) + "\n")
    print(json.dumps({"scenario": scenario, "passed": facts["passed"], "failure": facts.get("failure"),
                      "elapsed_seconds": facts["elapsed_seconds"]}), flush=True)
    return facts["passed"]


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("client", "engine", "evidence-dir"):
        parser.add_argument("--" + name, required=True)
    parser.add_argument("--model", default="claude-haiku-4-5-20251001")
    parser.add_argument("--scenario", choices=SCENARIOS, required=True)
    arguments = parser.parse_args()
    for name in ("client", "engine"):
        setattr(arguments, name, str(Path(getattr(arguments, name)).resolve(strict=True)))
    require(claude.is_claude(arguments.client), "this diagnostic drives Claude Code")
    raise SystemExit(0 if run_one(arguments, arguments.scenario, Path(arguments.evidence_dir).resolve()) else 1)
