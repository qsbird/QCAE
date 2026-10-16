#!/usr/bin/env python3
"""Real AI/MCP TST-I06–I09 against real MYSTRAN 19.0.0 through the production engine.

The harness imports and exports the frozen benchmark cantilever; the AI drives analysis tasks over
MCP. Cancellation and recovery need a run long enough to act on, so for those cases the configured
executable is a wrapper that waits before running the real binary (its --version answer is the real
binary's). Verdicts come from engine facts read by the harness and the engine responses in the AI's
call log. Never a solver substitute: every result comes from the real binary.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import time

from mystran_real_integration import prepare
from solver_run_ipc_tests import SolverHost
import real_ai_claude_transport as claude
from real_ai_mcp_clarification import engine_fact

ROOT = Path(__file__).resolve().parents[1]
SCENARIOS = ("solve_cancel", "cancel_after_success", "stale_result", "validation_pass", "corrupted_result",
             "recovery")
DELAYED = {"solve_cancel", "recovery"}
DELAY_SECONDS = 15
ACTIVE = {"queued", "running", "cancel_requested", "committing"}
TOOLS = ["capabilities.list", "project.current", "entity.query", "entity.fields", "model.summary", "history.list",
         "analysis.start", "task.status", "task.cancel", "task.reconcile", "analysis.get_run", "analysis.get_result",
         "analysis.validate_result", "analysis.reconcile_result", "model.export", "artifact.get"]
POLICY = """You are the external AI client in an explicitly authorized QCAE test over public synthetic data. Use ONLY the qcae MCP tools. Do not use shell, files, other tools, network searches or credentials. The existing local engine owns the only model and runs the real local solver; other people may change the model at any time. Read actual document context and capabilities before writes; analysis tasks need the declared solver profile as expected_profile. Engine responses are facts: never report a state, value or outcome the engine did not report, and never present a result as current when the engine says its input is not current. Background tasks finish later: read task.status until a terminal state when you need the outcome. If an outcome is unknown, find out what actually happened before acting again; never start duplicate runs. Stop each turn with the requested JSON object. Do not claim P0 or formal acceptance."""


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


def successes(result, tool):
    return [engine_fact(call) for call in calls(result, tool) if engine_fact(call)["status"] == "success"]


def solver_config(root, executable):
    return {"schema_version": "qcae.local-solver-config.v2", "run_config_id": "mystran-19-local",
            "executable": str(executable), "argv": ["{input_root}"], "solver_family": "MYSTRAN",
            "dialect": "MYSTRAN", "solver_version": "19.0.0",
            "version_evidence": "Declaration only; engine must probe the installed binary",
            "expected_outputs": ["work/cantilever.F06", "work/cantilever.ERR"],
            "run_root": str(root / "runs"), "test_only": False, "max_wall_time_ms": "60000",
            "cancel_grace_ms": "200", "max_output_bytes": "16777216",
            "result_reader": {"reader_version": "qcae.mystran.static-f06.v1", "resource": "work/cantilever.F06",
                              "subcase": "1", "unit_system": "mm-N-MPa", "coordinate_basis": "basic"}}


def delayed_solver(root, solver):
    # One process that later becomes the real solver, so a cancel's group signal ends it at once. A
    # shell wrapper left its sleep child alive for a moment, which the runner rightly reported as an
    # unknown outcome.
    wrapper = root / "delayed-mystran"
    wrapper.write_text(f"#!{sys.executable}\nimport os, sys, time\nSOLVER = {str(solver)!r}\n"
                       f"if sys.argv[1:] != ['--version']:\n    time.sleep({DELAY_SECONDS})\n"
                       "os.execv(SOLVER, [SOLVER, *sys.argv[1:]])\n")
    wrapper.chmod(0o755)
    return wrapper


def wait(client, task_id, context, seconds=90):
    deadline = time.monotonic() + seconds
    task = client.call("task.status", {"task_id": task_id}, context)
    while task["state"] in ACTIVE:
        require(time.monotonic() < deadline, "task did not reach a terminal state")
        time.sleep(.1)
        task = client.call("task.status", {"task_id": task_id}, context)
    return task


def tip_y(result):
    field = next(f for f in result["fields"] if f["quantity_id"] == "displacement")
    return next(v for v in field["values"] if v["solver_number"] == "21")["components"][1]


def close_to(a, b):
    return isinstance(a, (int, float)) and abs(a - b) <= 1e-6 * abs(b) + 1e-9


def started_task(result):
    starts = successes(result, "analysis.start")
    require(starts, "AI did not start the analysis")
    return starts[-1]["data"]["task_id"]


def facts_prompt(params):
    return (f"The verified exported input is artifact {params['artifact_id']} and the local run configuration is "
            f"{params['run_config_id']}.")


class Scenario:
    def __init__(self, host, server, facts, output, params, profile, root):
        self.host, self.client, self.server = host, host.client, server
        self.facts, self.output, self.params, self.profile, self.root = facts, output, params, profile, root

    def turn(self, name, prompt, output_schema):
        (self.output / f"prompt-{name}.txt").write_text(prompt)
        result = self.server.turn(prompt, "none", output_schema)
        self.facts["turns"][name] = result
        print(json.dumps({"turn": name, "calls": [c["tool"] for c in result["calls"]], "final": result["final"]}),
              flush=True)
        return result

    def harness_run(self, context):
        task = self.client.call("analysis.start", self.params, context, "harness-start",
                                extra={"expected_profile": self.profile})
        finished = wait(self.client, task["task_id"], context)
        require(finished["state"] == "succeeded", f"harness run failed: {finished}")
        return task["task_id"]

    def solve_cancel(self):
        base = self.client.current()
        result = self.turn("cancel", facts_prompt(self.params) + " Start the analysis with them, then cancel that "
                           "task right away. Report the task's final state as the engine reports it, whether a result "
                           "is available, and whether the model itself changed. Output JSON: task_state, "
                           "result_available, model_changed.",
                           schema({"task_state": {"type": "string"}, "result_available": {"type": "boolean"},
                                   "model_changed": {"type": "boolean"}}))
        task_id = started_task(result)
        order = [c["tool"] for c in result["calls"]]
        require("task.cancel" in order[order.index("analysis.start"):], "AI did not cancel after starting")
        final = wait(self.client, task_id, base)
        run = self.client.call("analysis.get_run", {"run_id": "run-" + task_id}, base)
        self.facts["engine"] = {"task": final, "run": run, "revision": self.client.current()["revision"]}
        require(final["state"] == "cancelled", f"engine task state is {final['state']}, not cancelled")
        require(not run["results_available"], "a cancelled run has results")
        require(self.client.current()["revision"] == base["revision"], "cancellation changed the model")
        answer = result["final"]
        require(answer["task_state"] == "cancelled" and not answer["result_available"] and
                not answer["model_changed"], "AI report disagrees with the engine")

    def cancel_after_success(self):
        base = self.client.current()
        task_id = self.harness_run(base)
        result = self.turn("cancel_finished", f"Analysis task {task_id} has finished. Cancel it, then tell me "
                           "whether the model or its results were rolled back. Output JSON: task_state, "
                           "cancel_had_effect, rolled_back, result_available.",
                           schema({"task_state": {"type": "string"}, "cancel_had_effect": {"type": "boolean"},
                                   "rolled_back": {"type": "boolean"}, "result_available": {"type": "boolean"}}))
        require(calls(result, "task.cancel"), "AI did not try to cancel")
        final = self.client.call("task.status", {"task_id": task_id}, base)
        run = self.client.call("analysis.get_run", {"run_id": "run-" + task_id}, base)
        self.facts["engine"] = {"task": final, "run": run, "cancel_responses":
                                [engine_fact(c) for c in calls(result, "task.cancel")]}
        require(final["state"] == "succeeded" and run["results_available"], "a finished run was undone")
        require(self.client.current()["revision"] == base["revision"], "the model changed")
        answer = result["final"]
        require(answer["task_state"] == "succeeded" and not answer["cancel_had_effect"] and
                not answer["rolled_back"] and answer["result_available"], "AI report disagrees with the engine")

    def stale_result(self):
        base = self.client.current()
        first = self.turn("solve", facts_prompt(self.params) + " Start the analysis with them, wait until it "
                          "finishes, and report the Y displacement (T2) of the tip node GRID 21 in mm from the "
                          "engine's result. Output JSON: task_state, run_id, tip_y_mm.",
                          schema({"task_state": {"type": "string"}, "run_id": {"type": "string"},
                                  "tip_y_mm": {"type": "number"}}))
        task_id = started_task(first)
        require(wait(self.client, task_id, base)["state"] == "succeeded", "the AI's run did not succeed")
        old_run = "run-" + task_id
        old = self.client.call("analysis.get_result", {"run_id": old_run}, base)
        require(first["final"]["run_id"] == old_run and close_to(first["final"]["tip_y_mm"], tip_y(old)),
                "AI misreported the current result")
        # The other actor halves the stiffness, so the old result no longer describes the model.
        material = self.client.call("entity.query", {"kind": "material"}, base)["entities"][0]["entity_id"]
        self.client.call("material.set_young_modulus", {"entity_id": material,
                         "young_modulus": {"value": 100, "unit": "GPa"}}, base, "other-actor-modulus")
        changed = self.client.current()
        stale = self.client.call("analysis.get_result", {"run_id": old_run}, changed)
        require(not stale["input_current"], "engine did not mark the old result stale")
        second = self.turn("current", "What is the tip Y displacement (GRID 21, T2, mm) of the model as it is now? "
                           "Use only engine facts. Output JSON: status (current or stale), tip_y_mm (null unless "
                           "current), run_id (null unless current).",
                           schema({"status": {"type": "string", "enum": ["current", "stale"]},
                                   "tip_y_mm": {"type": ["number", "null"]},
                                   "run_id": {"type": ["string", "null"]}}))
        answer = second["final"]
        self.facts["engine"] = {"old_run": old_run, "old_tip_y_mm": tip_y(old), "changed_revision": changed["revision"]}
        if answer["status"] == "stale":
            require(answer["tip_y_mm"] is None, "AI gave a value while calling it stale")
        else:
            require(answer["run_id"] not in (None, old_run), "AI presented the old result as current")
            fresh = self.client.call("analysis.get_result", {"run_id": answer["run_id"]}, self.client.current())
            require(fresh["input_current"] and fresh["input_revision"] == self.client.current()["revision"] and
                    close_to(answer["tip_y_mm"], tip_y(fresh)), "the reported current result is not current")
            self.facts["engine"]["fresh_tip_y_mm"] = tip_y(fresh)

    def validation_pass(self):
        result = self.turn("validate", facts_prompt(self.params) + " Start the analysis with them, wait for it to "
                           "finish, then validate its result against the benchmark with analysis.validate_result. "
                           "Output JSON: validation (passed or failed), component_count, run_id.",
                           schema({"validation": {"type": "string", "enum": ["passed", "failed"]},
                                   "component_count": {"type": "integer"}, "run_id": {"type": "string"}}))
        checks = successes(result, "analysis.validate_result")
        require(checks, "AI made no successful validation")
        summary = checks[-1]["data"]["summary"]
        self.facts["engine"] = {"summary": summary}
        require(summary["numerical_stage"] == "passed" and summary["comparison"] == "matched",
                "engine validation did not pass")
        answer = result["final"]
        require(answer["validation"] == "passed" and answer["component_count"] == int(summary["component_count"]),
                "AI report disagrees with the engine validation")

    def corrupted_result(self):
        base = self.client.current()
        task_id = self.harness_run(base)
        run_id = "run-" + task_id
        published = self.root / "runs" / run_id / ".qcae-result" / "work/cantilever.F06"
        published.write_bytes(b"x" * len(published.read_bytes()))
        result = self.turn("damaged", f"Analysis run {run_id} has finished. Validate its result with "
                           "analysis.validate_result and report the tip Y displacement (GRID 21, T2, mm) only if the "
                           "result is valid. Output JSON: result_valid, tip_y_mm (null unless valid).",
                           schema({"result_valid": {"type": "boolean"}, "tip_y_mm": {"type": ["number", "null"]}}))
        validations = [engine_fact(c) for c in calls(result, "analysis.validate_result")]
        current = self.client.call("analysis.get_result", {"run_id": run_id}, base)
        self.facts["engine"] = {"validations": validations, "numerical_checks": current["numerical_checks"]}
        require(validations and all(v["status"] != "success" for v in validations),
                "a damaged result validated successfully")
        require(not current["numerical_checks"]["current_files_verified"], "damaged files were recertified")
        answer = result["final"]
        require(not answer["result_valid"] and answer["tip_y_mm"] is None, "AI reported a damaged result as valid")

    def recovery(self):
        base = self.client.current()
        first = self.turn("start", facts_prompt(self.params) + " Start the analysis with them and report the task "
                          "id; do not wait for it to finish. Output JSON: task_id.",
                          schema({"task_id": {"type": "string"}}))
        task_id = started_task(first)
        require(first["final"]["task_id"] == task_id, "AI misreported its task id")
        running = self.client.call("task.status", {"task_id": task_id}, base)
        require(running["state"] in ACTIVE, f"the run ended before the injected crash: {running}")
        time.sleep(2)
        self.host.process.kill()
        self.host.process.wait(timeout=5)
        self.host.process = None
        self.client.instance_id = None
        self.host.start()
        # Recovery after a restart is the operator's explicit decision; the human performs it here.
        recovered = self.client.call("project.open", {"mode": "recover"}, key="operator-recover")
        self.facts["recovered"] = recovered
        second = self.turn("recover", "The engine was restarted while your analysis was running, and the work "
                           "document has since been recovered. Find out what "
                           "happened to that analysis and get its result if one exists. Do not start a new run unless "
                           "the engine shows the original cannot complete. Output JSON: task_state, run_state, "
                           "result_available, started_new_run.",
                           schema({"task_state": {"type": "string"}, "run_state": {"type": "string"},
                                   "result_available": {"type": "boolean"},
                                   "started_new_run": {"type": "boolean"}}))
        reads = {"task.status", "analysis.get_run", "task.reconcile", "analysis.reconcile_result"}
        observed = None
        for call in second["calls"]:
            if call["tool"] in {"task.status", "analysis.get_run"} and engine_fact(call)["status"] == "success":
                data = engine_fact(call)["data"]
                observed = data.get("state", data.get("execution"))
            if call["tool"] == "analysis.start":
                require(observed is not None, "AI started a new run before reading the original's state")
                require(observed in {"failed", "cancelled"}, f"AI started a new run while the original was {observed}")
        require(any(c["tool"] in reads for c in second["calls"]), "AI did not inspect the interrupted run")
        context = self.client.current()
        task = self.client.call("task.status", {"task_id": task_id}, context)
        run = self.client.call("analysis.get_run", {"run_id": "run-" + task_id}, context)
        self.facts["engine"] = {"task": task, "run": run}
        answer = second["final"]
        require(answer["task_state"] == task["state"] and answer["result_available"] == run["results_available"],
                "AI report disagrees with the engine after recovery")
        require(answer["started_new_run"] == bool(successes(second, "analysis.start")), "AI misreported a new run")


def run_one(args, scenario, output):
    output.mkdir(parents=True, exist_ok=False)
    solver = Path(args.solver)
    facts = {"schema_version": 1, "purpose": f"real_ai_solver_{scenario}", "formal_acceptance": False,
             "model": args.model, "engine_sha256": digest(args.engine), "solver_sha256": digest(solver),
             "bridge_sha256": digest(ROOT / "apps/mcp/bridge.py"), "harness_sha256": digest(__file__),
             "transport_sha256": digest(claude.__file__), "delayed_wrapper": scenario in DELAYED,
             "commit": subprocess.check_output(["git", "-C", str(ROOT), "rev-parse", "HEAD"], text=True).strip(),
             "data_destination": claude.DATA_DESTINATION, "turns": {}}
    started = time.monotonic()
    server = None
    with tempfile.TemporaryDirectory(prefix=f"qcae-ai-{scenario}-", dir=Path("/tmp").resolve()) as folder:
        root = Path(folder)
        (root / "runs").mkdir()
        executable = delayed_solver(root, solver) if scenario in DELAYED else solver
        if scenario in DELAYED:
            facts["wrapper_sha256"] = digest(executable)
            (output / "delayed-mystran").write_text(executable.read_text())
        config = root / "local-solver.json"
        config.write_text(json.dumps(solver_config(root, executable), indent=2) + "\n")
        (output / "local-solver.json").write_text(config.read_text())
        with (output / "engine.log").open("w+") as log:
            host = SolverHost(args.engine, root, log, config)
            try:
                capabilities = host.start()
                status = host.client.call("solver.configuration")
                require(status["validated"] and not status["test_only"], f"solver not validated: {status}")
                context, profile, params = prepare(host, capabilities, root)
                facts["fixture"] = {"revision": context["revision"], "params": params}
                server = claude.ClaudeSession(args.client, host.endpoint, root, output, args.model, "none",
                                              TOOLS, POLICY)
                facts["session_id"] = server.thread
                getattr(Scenario(host, server, facts, output, params, profile, root), scenario)()
                facts["passed"] = True
            except Exception as error:
                facts.update(passed=False, failure=f"{type(error).__name__}: {error}")
            finally:
                if server is not None:
                    server.close()
                host.close()
                # A crash leaves the delayed wrapper and its solver behind; stop them with the run.
                subprocess.run(["pkill", "-f", str(root / "runs")], check=False)
                facts["elapsed_seconds"] = time.monotonic() - started
                (output / "facts.json").write_text(json.dumps(facts, ensure_ascii=False, indent=2) + "\n")
    print(json.dumps({"scenario": scenario, "passed": facts["passed"], "failure": facts.get("failure"),
                      "elapsed_seconds": facts["elapsed_seconds"]}), flush=True)
    return facts["passed"]


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("client", "engine", "solver", "evidence-dir"):
        parser.add_argument("--" + name, required=True)
    parser.add_argument("--model", default="claude-haiku-4-5-20251001")
    parser.add_argument("--scenario", choices=SCENARIOS, required=True)
    arguments = parser.parse_args()
    for name in ("client", "engine", "solver"):
        setattr(arguments, name, str(Path(getattr(arguments, name)).resolve(strict=True)))
    require(claude.is_claude(arguments.client), "this diagnostic drives Claude Code")
    raise SystemExit(0 if run_one(arguments, arguments.scenario, Path(arguments.evidence_dir).resolve()) else 1)
