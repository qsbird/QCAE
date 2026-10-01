#!/usr/bin/env python3
"""Explicit real AI/MCP diagnostic over synthetic inputs; never a solver substitute."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def cli_call(cli, endpoint, operation, parameters=None, context=None):
    request = {"api_version": "1.1", "request_id": f"diagnostic-{time.time_ns()}",
               "operation": operation, "parameters": parameters or {}}
    if context:
        request.update(document_id=context["document_id"], document_epoch=context["document_epoch"])
    result = subprocess.run([cli, "--socket", str(endpoint), "--no-start"], input=json.dumps(request),
                            capture_output=True, text=True, timeout=15)
    assert result.returncode == 0, (result.stdout, result.stderr)
    return json.loads(result.stdout)


def run(args):
    output = Path(args.evidence_dir).resolve()
    output.mkdir(parents=True, exist_ok=True)
    client, engine_path, cli = map(lambda path: str(Path(path).resolve()), (args.client, args.engine, args.cli))
    with tempfile.TemporaryDirectory(prefix="qcae-real-ai-", dir=Path("/tmp").resolve()) as temporary:
        run_root = Path(temporary)
        endpoint = run_root / "engine.sock"
        prompt = """This is an explicitly authorized QCAE integration test with public synthetic input. Use ONLY the qcae MCP tools for every project/model action. Do not execute shell, read/write files, install anything, access credentials, launch a solver, or use other tools. Do not invent units or document context.
Perform these sequential operations against the existing engine: create a new project named 'Real AI MCP smoke' with idempotency key 'real-ai-project'; read project.current; create ONE material named 'Steel' with Young modulus exactly 210 GPa and Poisson ratio 0.3, using the current document_id/document_epoch/revision and idempotency key 'real-ai-material'; read current and entity fields to verify normalized E = 210000 MPa; undo exactly that material change using the newest context and key 'real-ai-undo'; verify zero materials; redo it with the newest context and key 'real-ai-redo'; verify one material and revision 3. Use engine tool responses as facts. Finish with a short JSON object containing document_id, final_revision, material_count, normalized_modulus_mpa and status. Do not claim complete P0, solver execution or numerical acceptance. If a tool fails, state the actual failure without inventing success.
"""
        (output / "prompt.txt").write_text(prompt)
        metadata = {"schema_version": 1, "purpose": "real_ai_mcp_synthetic_smoke_diagnostic",
                    "formal_m5_acceptance": False, "model": args.model, "reasoning_effort": args.effort,
                    "engine_sha256": digest(engine_path), "cli_sha256": digest(cli),
                    "bridge_sha256": digest(ROOT / "apps/mcp/bridge.py"),
                    "data_destination": "OpenAI model service receives this synthetic prompt and QCAE tool schemas/results; stdio and engine IPC remain local",
                    "workspace": "temporary isolated SQLite; deleted after the run",
                    "source_head": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
                    "source_dirty": bool(subprocess.check_output(["git", "status", "--porcelain"], cwd=ROOT, text=True).strip())}
        with (output / "engine.log").open("w+") as log:
            engine = subprocess.Popen([engine_path, "--socket", str(endpoint), "--workspace", str(run_root / "work.sqlite")],
                                      stdout=log, stderr=log)
            try:
                deadline = time.monotonic() + 15
                while not endpoint.exists() and engine.poll() is None and time.monotonic() < deadline:
                    time.sleep(.025)
                assert endpoint.exists(), "engine did not start"
                config = ["--disable", "plugins", "--disable", "apps", "--disable", "remote_plugin", "--disable", "hooks",
                          "--ask-for-approval", "never", "exec", "--ignore-user-config", "--ephemeral", "--json",
                          "--sandbox", "read-only", "--skip-git-repo-check", "--model", args.model,
                          "-c", f'model_reasoning_effort="{args.effort}"',
                          "-c", f'mcp_servers.qcae.command={json.dumps(sys.executable)}',
                          "-c", 'mcp_servers.qcae.args=' + json.dumps([str(ROOT / "apps/mcp/bridge.py"), "--endpoint", str(endpoint)]),
                          "-c", 'mcp_servers.qcae.required=true',
                          "-c", 'mcp_servers.qcae.default_tools_approval_mode="approve"',
                          "-c", 'mcp_servers.qcae.enabled_tools=' + json.dumps([
                              "capabilities.list", "project.create", "project.current", "material.create",
                              "entity.fields", "history.undo", "history.redo", "model.summary"]),
                          "-C", str(run_root), "-"]
                metadata["client_version"] = subprocess.check_output([client, "--version"], text=True).strip()
                metadata["mcp_policy"] = "per-run approval for explicitly authorized synthetic project tools; eight-tool allowlist; global config unchanged"
                started = time.monotonic()
                with (output / "client.jsonl").open("w") as stdout, (output / "client.stderr.log").open("w") as stderr:
                    completed = subprocess.run([client, *config], input=prompt, text=True, stdout=stdout, stderr=stderr, timeout=600)
                metadata.update(client_exit_code=completed.returncode, elapsed_seconds=time.monotonic() - started)
                if completed.returncode == 0:
                    context = cli_call(cli, endpoint, "project.current")["data"]
                    summary = cli_call(cli, endpoint, "model.summary", context=context)
                    metadata["verified_current"] = context
                    metadata["verified_summary"] = summary
                    assert context["revision"] == "3" and len(summary["data"]["materials"]) == 1, metadata
                    modulus = summary["data"]["materials"][0].get("young_modulus_mpa")
                    assert modulus == 210000, summary
                    items = [json.loads(line) for line in (output / "client.jsonl").read_text().splitlines() if line.strip()]
                    calls = [item for item in items if item.get("item", {}).get("type") == "mcp_tool_call"]
                    metadata["actual_mcp_event_count"] = len(calls)
                    completed_calls = [item["item"] for item in calls if item.get("type") == "item.completed"]
                    metadata["actual_completed_mcp_calls"] = len(completed_calls)
                    assert len(completed_calls) >= 6 and all(item["status"] == "completed" and not item.get("error") for item in completed_calls), "missing or failed actual MCP tool calls"
                    other_actions = [item for item in items if item.get("item", {}).get("type") not in {None, "mcp_tool_call", "agent_message"}]
                    assert not other_actions, "session used actions outside project MCP"
                    metadata["model_usage"] = next(item["usage"] for item in reversed(items) if item.get("type") == "turn.completed")
                    metadata["smoke_passed"] = True
                else:
                    metadata["smoke_passed"] = False
            except Exception as error:
                metadata.update(smoke_passed=False, failure=f"{type(error).__name__}: {error}")
                raise
            finally:
                if engine.poll() is None:
                    engine.terminate()
                engine.wait(timeout=5)
                (output / "facts.json").write_text(json.dumps(metadata, ensure_ascii=False, indent=2) + "\n")
        print(json.dumps({key: metadata.get(key) for key in ("client_exit_code", "smoke_passed", "actual_mcp_event_count", "elapsed_seconds")}))
        return 0 if metadata.get("smoke_passed") else 1


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--client", required=True)
    parser.add_argument("--engine", required=True)
    parser.add_argument("--cli", required=True)
    parser.add_argument("--model", required=True)
    parser.add_argument("--effort", choices=("low", "medium", "high", "xhigh", "max", "ultra"), required=True)
    parser.add_argument("--evidence-dir", required=True)
    raise SystemExit(run(parser.parse_args()))
