#!/usr/bin/env python3
"""Real two-turn AI/MCP unit clarification; a partial TST-I01 diagnostic."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import select
import subprocess
import sys
import tempfile
import time

from real_ai_mcp_session import cli_call, digest

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ["capabilities.list", "project.current", "material.create", "entity.fields", "model.summary"]
POLICY = """You are the external AI client in an explicitly authorized QCAE test over public synthetic data. Use ONLY qcae MCP tools for project/model actions. Do not use shell, files, network searches, other tools, credentials or a solver. The existing local engine owns the model. Do not create a second project. Read its document context; preserve actual revision and idempotency facts. Missing physical units must be requested from the human and never guessed. Stop each turn with one JSON object. Do not claim complete TST-I01, P0, solver or numerical acceptance."""


def engine_fact(call):
    result = call.get("result") or {}
    value = result.get("structuredContent")
    if value is None:
        texts = [item["text"] for item in result.get("content", []) if item.get("type") == "text"]
        if len(texts) == 1:
            value = json.loads(texts[0])
    if not isinstance(value, dict) or "status" not in value:
        raise ValueError("actual MCP response has no structured QCAE status")
    return value


class AppServer:
    def __init__(self, client, endpoint, cwd, output, model, effort):
        args = [str(client), "--disable", "plugins", "--disable", "apps", "--disable", "remote_plugin",
                "--disable", "hooks", "app-server", "--stdio", "-c", "mcp_servers={}",
                "-c", "analytics.enabled=false", "-c", f"model={json.dumps(model)}",
                "-c", f"model_reasoning_effort={json.dumps(effort)}",
                "-c", f"mcp_servers.qcae.command={json.dumps(sys.executable)}",
                "-c", "mcp_servers.qcae.args=" + json.dumps([str(ROOT / "apps/mcp/bridge.py"), "--endpoint", str(endpoint)]),
                "-c", "mcp_servers.qcae.required=true",
                "-c", 'mcp_servers.qcae.default_tools_approval_mode="approve"',
                "-c", "mcp_servers.qcae.enabled_tools=" + json.dumps(TOOLS)]
        self.error_log = (output / "client.stderr.log").open("wb")
        self.events_log = (output / "client.jsonl").open("wb")
        self.requests_log = (output / "client-requests.jsonl").open("wb")
        self.process = subprocess.Popen(args, cwd=cwd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=self.error_log)
        self.buffer, self.sequence, self.events = b"", 0, []
        try:
            self.rpc("initialize", {"clientInfo": {"name": "qcae_synthetic_units", "version": "1"}})
            self.send({"method": "initialized"})
            response = self.rpc("thread/start", {"cwd": str(cwd), "ephemeral": True, "model": model,
                                                  "approvalPolicy": "never", "sandbox": "read-only",
                                                  "developerInstructions": POLICY})
            self.thread = response["thread"]["id"]
            assert response["model"] == model and response["approvalPolicy"] == "never", response
            assert response["thread"]["ephemeral"] is True, response
        except Exception:
            self.close()
            raise

    def send(self, value):
        raw = json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode() + b"\n"
        self.requests_log.write(raw)
        self.requests_log.flush()
        self.process.stdin.write(raw)
        self.process.stdin.flush()

    def receive(self, deadline):
        while b"\n" not in self.buffer:
            remaining = deadline - time.monotonic()
            if remaining <= 0 or not select.select([self.process.stdout], [], [], min(remaining, 10))[0]:
                if self.process.poll() is not None:
                    raise RuntimeError(f"app-server exited {self.process.returncode}")
                if remaining <= 0:
                    raise TimeoutError("bounded app-server response deadline")
                continue
            data = os.read(self.process.stdout.fileno(), 65536)
            if not data:
                raise RuntimeError("app-server stdout closed")
            self.buffer += data
            if len(self.buffer) > 8 * 1024 * 1024:
                raise ValueError("app-server JSONL frame exceeded diagnostic bound")
        raw, self.buffer = self.buffer.split(b"\n", 1)
        self.events_log.write(raw + b"\n")
        self.events_log.flush()
        event = json.loads(raw)
        self.events.append(event)
        if "method" in event and "id" in event:
            self.send({"id": event["id"], "error": {"code": -32601,
                       "message": "This diagnostic does not authorize additional client actions"}})
            raise RuntimeError("unexpected app-server client request: " + event["method"])
        if event.get("method") in {"item/started", "item/completed"}:
            item = event["params"]["item"]
            if item["type"] not in {"userMessage", "agentMessage", "reasoning", "mcpToolCall"}:
                raise RuntimeError("AI used a non-MCP action: " + item["type"])
            if item["type"] == "mcpToolCall" and (item["server"] != "qcae" or item["tool"] not in TOOLS):
                raise RuntimeError("AI called a tool outside the explicit allowlist")
        return event

    def rpc(self, method, params):
        self.sequence += 1
        identity = self.sequence
        self.send({"id": identity, "method": method, "params": params})
        deadline = time.monotonic() + 30
        while True:
            event = self.receive(deadline)
            if event.get("id") == identity and "method" not in event:
                if "error" in event:
                    raise RuntimeError(f"{method}: {event['error']}")
                return event["result"]

    def turn(self, prompt, effort, output_schema):
        start_index = len(self.events)
        response = self.rpc("turn/start", {"threadId": self.thread, "effort": effort,
                                            "input": [{"type": "text", "text": prompt}],
                                            "outputSchema": output_schema})
        identity = response["turn"]["id"]
        deadline = time.monotonic() + 600
        while True:
            event = self.receive(deadline)
            if event.get("method") == "turn/completed" and event["params"]["turn"]["id"] == identity:
                assert event["params"]["turn"]["status"] == "completed", event
                break
        completed = [event["params"]["item"] for event in self.events[start_index:]
                     if event.get("method") == "item/completed"]
        calls = [item for item in completed if item["type"] == "mcpToolCall"]
        failed_calls = [{"tool": item["tool"], "status": item["status"], "error": item.get("error")}
                        for item in calls if item["status"] != "completed" or item.get("error")]
        assert calls and not failed_calls, {"completed_calls": len(calls), "failed_calls": failed_calls}
        messages = [item["text"] for item in completed if item["type"] == "agentMessage"]
        assert messages, "missing real model final message"
        # Require actual structured output; never infer success from prose.
        final = json.loads(messages[-1])
        return {"turn_id": identity, "calls": calls, "final": final}

    def close(self):
        if self.process.poll() is None:
            self.process.stdin.close()
            try:
                self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.process.terminate()
                self.process.wait(timeout=5)
        self.events_log.close()
        self.requests_log.close()
        self.error_log.close()


def one(args, output):
    output.mkdir(parents=True, exist_ok=False)
    facts = {"schema_version": 1, "purpose": "real_same_session_unit_clarification_partial_TST_I01",
             "formal_acceptance": False, "real_solver_run": False, "model": args.model,
             "reasoning_effort": args.effort, "engine_sha256": digest(args.engine),
             "cli_sha256": digest(args.cli), "bridge_sha256": digest(ROOT / "apps/mcp/bridge.py"),
             "harness_sha256": digest(__file__),
             "client_version": subprocess.check_output([args.client, "--version"], text=True).strip(),
             "source_binding": "mutable_tree_diagnostic", "ephemeral_ai_session": True,
             "data_destination": "OpenAI model service receives only synthetic prompts and QCAE schemas/results",
             "limitations": ["material unit cycle only; geometry/force units, GUI and export equivalence pending",
                             "app-server process config overrides; existing auth is used without copying it"]}
    started = time.monotonic()
    with tempfile.TemporaryDirectory(prefix="qcae-ai-clarification-", dir=Path("/tmp").resolve()) as folder:
        root = Path(folder)
        endpoint = root / "engine.sock"
        server = None
        with (output / "engine.log").open("wb") as log:
            engine = subprocess.Popen([args.engine, "--socket", str(endpoint), "--workspace", str(root / "work.sqlite")],
                                      stdout=log, stderr=log)
            try:
                deadline = time.monotonic() + 15
                while not endpoint.exists() and engine.poll() is None and time.monotonic() < deadline:
                    time.sleep(.025)
                assert endpoint.exists(), "engine did not start"
                request = {"api_version": "1.1", "request_id": "human-project", "operation": "project.create",
                           "parameters": {"name": "Synthetic unit clarification"}, "idempotency_key": "human-project"}
                created = subprocess.run([args.cli, "--socket", str(endpoint), "--no-start"], input=json.dumps(request),
                                         capture_output=True, text=True, timeout=15)
                assert created.returncode == 0, created.stdout
                initial = cli_call(args.cli, endpoint, "project.current")["data"]
                server = AppServer(args.client, endpoint, root, output, args.model, args.effort)
                facts["thread_id"] = server.thread
                prompt = "请创建一个 Steel 材料，弹性模量为210，泊松比为0.3。先从现有项目读取上下文；如果必要物理事实不足，请列出缺项并停止，不能自行假定。最后仅输出JSON，包含status和missing数组。"
                (output / "prompt-1.txt").write_text(prompt)
                first_schema = {"type": "object", "additionalProperties": False,
                                "properties": {"status": {"type": "string", "enum": ["needs_input", "success"]},
                                               "missing": {"type": "array", "items": {"type": "string"}}},
                                "required": ["status", "missing"]}
                facts["clarification_output_schema"] = first_schema
                first = server.turn(prompt, args.effort, first_schema)
                after_first = cli_call(args.cli, endpoint, "project.current")["data"]
                summary = cli_call(args.cli, endpoint, "model.summary", context=after_first)
                facts["missing_unit_turn"] = first
                facts["verified_before_units"] = {"context": after_first, "summary": summary}
                assert after_first["revision"] == initial["revision"] and not summary["data"]["materials"], summary
                assert first["final"]["status"] == "needs_input" and first["final"]["missing"], first
                assert not any(call["tool"] == "material.create" and engine_fact(call)["status"] == "success"
                               for call in first["calls"]), first
                prompt = "补充单位：弹性模量210 GPa。材料名Steel，泊松比0.3不变。现在请通过qcae MCP创建这一个材料（幂等键human-unit-material），再读取实体字段核实归一化模量。不要创建其他对象。最后仅输出JSON，包含status、normalized_modulus_mpa和material_count。"
                (output / "prompt-2.txt").write_text(prompt)
                second_schema = {"type": "object", "additionalProperties": False,
                                 "properties": {"status": {"type": "string", "enum": ["needs_input", "success"]},
                                                "normalized_modulus_mpa": {"type": "number"},
                                                "material_count": {"type": "integer"}},
                                 "required": ["status", "normalized_modulus_mpa", "material_count"]}
                facts["units_supplied_output_schema"] = second_schema
                second = server.turn(prompt, args.effort, second_schema)
                facts["units_supplied_turn"] = second
                current = cli_call(args.cli, endpoint, "project.current")["data"]
                summary = cli_call(args.cli, endpoint, "model.summary", context=current)
                assert int(current["revision"]) == int(initial["revision"]) + 1, current
                assert len(summary["data"]["materials"]) == 1, summary
                assert summary["data"]["materials"][0]["young_modulus_mpa"] == 210000, summary
                assert second["final"]["status"] == "success" and second["final"]["normalized_modulus_mpa"] == 210000, second
                assert second["final"]["material_count"] == 1, second
                assert all(engine_fact(call)["status"] == "success" for call in second["calls"]), second
                writes = [call for call in second["calls"] if call["tool"] == "material.create"]
                assert writes and all(call["arguments"]["parameters"]["young_modulus"] == {"value": 210, "unit": "GPa"}
                                      and call["arguments"]["idempotency_key"] == "human-unit-material" for call in writes), writes
                facts.update(units_supplied_turn=second, verified_after_units={"context": current, "summary": summary},
                             actual_completed_mcp_calls=len(first["calls"]) + len(second["calls"]), passed=True)
            except Exception as error:
                facts.update(passed=False, failure=f"{type(error).__name__}: {error}")
                raise
            finally:
                if server is not None:
                    server.close()
                if engine.poll() is None:
                    engine.terminate()
                engine.wait(timeout=5)
                facts["elapsed_seconds"] = time.monotonic() - started
                (output / "facts.json").write_text(json.dumps(facts, ensure_ascii=False, indent=2) + "\n")
    print(json.dumps({key: facts.get(key) for key in ["passed", "actual_completed_mcp_calls", "elapsed_seconds"]}))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("client", "engine", "cli", "model", "effort", "evidence-dir"):
        parser.add_argument("--" + name, required=True)
    arguments = parser.parse_args()
    for name in ("client", "engine", "cli"):
        setattr(arguments, name, str(Path(getattr(arguments, name)).resolve(strict=True)))
    one(arguments, Path(arguments.evidence_dir).resolve())
