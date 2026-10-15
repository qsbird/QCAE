#!/usr/bin/env python3
"""Claude Code headless client for the real AI/MCP diagnostics, with the app-server turn interface.

Each turn runs `claude -p` in one persistent session (the first turn names it, later turns resume
it). Only the QCAE MCP server is configured, built-in tools are disabled, and every QCAE tool
outside the scenario allowlist is removed. The engine responses in the call log are facts; the
final answer is the model's structured output, never parsed from prose.
"""
from __future__ import annotations

import json
from pathlib import Path
import re
import subprocess
import sys
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "apps/mcp"))
import bridge as qcae_bridge  # noqa: E402

DATA_DESTINATION = "Anthropic model service receives only synthetic prompts and QCAE schemas/results"


def tool_name(operation: str) -> str:
    # Claude tool names allow [A-Za-z0-9_-]; Claude Code replaces the dots of MCP tool names.
    return "mcp__qcae__" + operation.replace(".", "_")


def is_claude(client) -> bool:
    version = subprocess.run([str(client), "--version"], capture_output=True, text=True, timeout=30)
    return "Claude Code" in version.stdout


class ClaudeSession:
    data_destination = DATA_DESTINATION

    def __init__(self, client, endpoint, cwd, output, model, effort, tools, policy, bridge_args=None):
        self.client, self.cwd, self.output, self.model = str(client), Path(cwd), Path(output), model
        self.tools, self.policy = list(tools), policy
        self.thread = str(uuid.uuid4())
        self.turns, self.costs = 0, []
        catalog = qcae_bridge.Engine(str(endpoint), 30).call("capabilities.list", {})
        names = [entry["name"] for entry in catalog["data"]["operations"]]
        self.names = {tool_name(name): name for name in names}
        if len(self.names) != len(names):
            raise RuntimeError("QCAE tool names collide after Claude name normalization")
        if set(self.tools) - set(names):
            raise RuntimeError(f"allowlisted tools are not in the engine catalog: {set(self.tools) - set(names)}")
        self.denied = [tool_name(name) for name in names if name not in self.tools]
        server = bridge_args or [str(ROOT / "apps/mcp/bridge.py"), "--endpoint", str(endpoint)]
        self.config = self.output / "mcp-config.json"
        self.config.write_text(json.dumps({"mcpServers": {"qcae": {"command": sys.executable, "args": server}}},
                                          indent=2) + "\n")
        self.version = subprocess.check_output([self.client, "--version"], text=True).strip()

    def turn(self, prompt, effort, output_schema):
        """Runs one turn; effort is ignored (no reasoning-effort control for this model)."""
        self.turns += 1
        arguments = [self.client, "-p", "--model", self.model, "--output-format", "stream-json", "--verbose",
                     "--mcp-config", str(self.config), "--strict-mcp-config", "--tools", "",
                     "--setting-sources", "", "--system-prompt", self.policy,
                     "--json-schema", json.dumps(output_schema),
                     "--allowedTools", *[tool_name(name) for name in self.tools]]
        if self.denied:
            arguments += ["--disallowedTools", *self.denied]
        arguments += ["--session-id", self.thread] if self.turns == 1 else ["--resume", self.thread]
        stem = self.output / f"client-turn-{self.turns}"
        with open(f"{stem}.stderr.log", "wb") as errors:
            completed = subprocess.run(arguments, cwd=self.cwd, input=prompt.encode(), stdout=subprocess.PIPE,
                                       stderr=errors, timeout=900)
        Path(f"{stem}.jsonl").write_bytes(completed.stdout)
        events = [json.loads(line) for line in completed.stdout.decode().splitlines() if line.strip()]
        uses, results, order, structured, final_event = {}, {}, [], [], None
        for event in events:
            if event.get("type") == "assistant":
                for block in event["message"]["content"]:
                    if block.get("type") != "tool_use":
                        continue
                    if block["name"] == "StructuredOutput":
                        # --json-schema delivers the final answer through this synthetic tool.
                        structured.append(block["input"])
                        continue
                    if block["name"] not in self.names:
                        raise RuntimeError("AI used a non-QCAE action: " + block["name"])
                    operation = self.names[block["name"]]
                    if operation not in self.tools:
                        raise RuntimeError("AI called a tool outside the explicit allowlist: " + operation)
                    uses[block["id"]] = {"tool": operation, "arguments": block["input"]}
                    order.append(block["id"])
            elif event.get("type") == "user" and isinstance(event["message"].get("content"), list):
                for block in event["message"]["content"]:
                    if block.get("type") == "tool_result":
                        results[block["tool_use_id"]] = block
            elif event.get("type") == "result":
                final_event = event
        if completed.returncode != 0 or final_event is None or final_event.get("subtype") != "success" \
                or final_event.get("is_error"):
            raise RuntimeError(f"claude turn {self.turns} did not complete: exit {completed.returncode}, "
                               f"{(final_event or {}).get('subtype')}")
        calls = []
        for identity in order:
            call = dict(uses[identity])
            block = results.get(identity)
            if block is None:
                call.update(status="failed", error="no tool result")
            else:
                content = block.get("content")
                text = content if isinstance(content, str) else "".join(
                    item.get("text", "") for item in content or [] if item.get("type") == "text")
                # Claude Code replaces a result over its inline limit with a short preview and a file
                # the model cannot open (it has no file tools); the harness reads that file for facts.
                saved = re.search(r"Full output saved to: (\S+)", text) if "<persisted-output>" in text else None
                if saved:
                    call["persisted_for_model"] = True
                    text = Path(saved.group(1)).read_text()
                try:
                    value = json.loads(text)
                    engine_response = isinstance(value, dict) and "status" in value
                except ValueError:
                    engine_response = False
                # An engine failure is still a completed call; only a missing QCAE response fails it.
                call["result"] = {"content": [{"type": "text", "text": text}]}
                call["status"] = "completed" if engine_response else "failed"
                if not engine_response:
                    call["error"] = text[:2000]
            calls.append(call)
        failed = [{"tool": call["tool"], "error": call.get("error")} for call in calls if call["status"] != "completed"]
        assert calls and not failed, {"completed_calls": len(calls), "failed_calls": failed}
        final = final_event.get("structured_output")
        if final is None and structured:
            final = structured[-1]
        if final is None:
            final = json.loads(final_event["result"])
        cost = {"turn": self.turns, "cost_usd": final_event.get("total_cost_usd"),
                "usage": final_event.get("usage"), "num_turns": final_event.get("num_turns"),
                "duration_ms": final_event.get("duration_ms"),
                "permission_denials": final_event.get("permission_denials")}
        self.costs.append(cost)
        return {"turn_id": f"{self.thread}:{self.turns}", "calls": calls, "final": final, "cost": cost}

    def close(self):
        (self.output / "client-costs.json").write_text(json.dumps(
            {"client_version": self.version, "model": self.model, "session_id": self.thread,
             "turns": self.costs}, indent=2) + "\n")
