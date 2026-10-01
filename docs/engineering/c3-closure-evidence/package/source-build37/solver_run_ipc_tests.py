#!/usr/bin/env python3
"""Production SQLite/task/process integration using explicitly test-only children.

This test never invokes Nastran. Its parsed fields retain test_process provenance,
and numerical engineering validation remains explicitly not_run.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sqlite3
import struct
import subprocess
import sys
import tempfile
import time

from c3_sync_ipc_tests import EngineProcess, check
from nastran_artifact_ipc_tests import RecordReader, owned_facts, wait_task


CHILD = r'''from pathlib import Path
import json
import signal
import sys
import time
mode, source = sys.argv[1:]
if mode == "ignore-term":
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
Path("started").write_text("explicit test-only child; no Nastran")
assert Path(source).is_file() and "BEGIN BULK" in Path(source).read_text()
if mode in {"slow", "restart", "ignore-term"}:
    time.sleep(1.0 if mode == "slow" else 2.0 if mode == "restart" else 5.0)
if mode.startswith("parsed-"):
    # A real filesystem barrier lets the IPC test inspect the unparsed state.
    # It is a test-only child protocol, not a production solver feature.
    deadline = time.monotonic() + 8
    while not Path("allow-output").exists():
        assert time.monotonic() < deadline, "test-only output gate timed out"
        time.sleep(.01)
    manifest = json.loads((Path(source).parent / "manifest.json").read_text())
    numbers = sorted(int(row["number"]) for row in manifest["export_id_map"]
                     if row["namespace"] == "GRID")
    assert len(numbers) == 2
    case = 2 if mode == "parsed-wrongcase" else 1
    tip = "nan" if mode == "parsed-damaged" else "0" if mode == "parsed-wrongphysics" else "-1.904763"
    f06 = (f"1 EXPLICIT SYNTHETIC TEST PROCESS PAGE 1\nSUBCASE {case}\n"
           "D I S P L A C E M E N T V E C T O R\n"
           "POINT ID. TYPE T1 T2 T3 R1 R2 R3\n"
           f"{numbers[0]} G 0 0 0 0 0 0\n{numbers[1]} G 0 {tip} 0 0 0 0\n"
           "F O R C E S O F S I N G L E - P O I N T C O N S T R A I N T\n"
           "POINT ID. TYPE T1 T2 T3 R1 R2 R3\n"
           f"{numbers[0]} G 0 1 0 0 0 1000\n")
    Path("result.f06").write_text(f06)
elif mode != "missing":
    Path("result.f06").write_bytes(b"bad, nonempty output" if mode == "corrupt" else
                                  b"test-only raw output; no engineering result")
print("explicit test-only process completed", flush=True)
'''


class SolverHost(EngineProcess):
    def __init__(self, engine, root, log, config=None):
        super().__init__(engine, str(root / "engine.sock"), str(root / "workspace.sqlite"), log)
        self.config = config

    def start(self):
        check(self.process is None, "Engine already running")
        args = [self.engine, "--socket", self.endpoint, "--workspace", self.workspace]
        if self.config is not None:
            args += ["--solver-config", str(self.config)]
        self.process = subprocess.Popen(args, stdout=self.log_file, stderr=self.log_file)
        deadline = time.monotonic() + 20
        last_error = None
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                self.log_file.flush()
                self.log_file.seek(0)
                raise AssertionError(f"Solver host startup failed: {self.log_file.read()}")
            try:
                return self.client.call("capabilities.list")
            except (OSError, AssertionError, json.JSONDecodeError) as error:
                last_error = error
                time.sleep(.02)
        raise AssertionError(f"Solver host startup timed out: {last_error}")


def configuration(root, mode, *, real_declaration=False, synthetic_version=False, with_reader=False):
    (root / "runs").mkdir()
    child = root / "test_only_child.py"
    child.write_text(CHILD)
    values = {"schema_version": "qcae.local-solver-config.v1",
              "run_config_id": "test-only-config", "executable": str(Path(sys.executable).resolve()),
              "argv": ["-I", str(child), mode, "{input_root}"],
              "solver_family": "Nastran" if real_declaration else "test-only",
              "dialect": "MSC" if real_declaration else "test-only-process",
              "solver_version": "2024.1" if real_declaration else "test-only-1",
              "version_evidence": "test-only declaration; never actual installed version proof",
              "expected_outputs": ["result.f06"], "run_root": str(root / "runs"),
              "test_only": not real_declaration, "max_wall_time_ms": "10000",
              "cancel_grace_ms": "50", "max_output_bytes": "1048576"}
    if with_reader:
        values["schema_version"] = "qcae.local-solver-config.v2"
        values["result_reader"] = {"reader_version": "qcae.nastran.static-f06.v1",
                                   "resource": "result.f06", "subcase": "1",
                                   "unit_system": "mm-N-MPa", "coordinate_basis": "basic"}
    if synthetic_version:
        wrapper = root / "synthetic_version_wrapper"
        wrapper.write_text(f"#!{Path(sys.executable).resolve()}\n"
                           "print('QCAE SYNTHETIC VERSION PROBE')\n"
                           "print('MSC Nastran 2024.1')\n")
        wrapper.chmod(0o700)
        values["executable"] = str(wrapper)
    path = root / "local-config.json"
    path.write_text(json.dumps(values))
    return path


def prepare(host, capabilities, root):
    client = host.client
    profile = capabilities["declared_solver_profiles"][0]["profile_ref"]
    context = client.call("project.create", {"name": "Test-only local execution"}, key="create")
    fixtures = Path(__file__).parent / "fixtures/nastran"
    paths = ("cantilever.bdf", "mesh/nodes.bdf", "mesh/beams.bdf", "properties.bdf")
    resources = [{"path": name, "text": (fixtures / name).read_text()} for name in paths]
    preview = client.call("changes.preview", {"command": "model.import", "root_resource": paths[0],
                          "resources": resources, "source_profile_ref": profile,
                          "unit_system": "mm-N-MPa"}, context)
    client.call("changes.commit", {"preview_id": preview["preview_id"]}, context, "import")
    context = client.current()
    analysis = client.call("entity.query", {"kind": "analysis"}, context)["entities"][0]["entity_id"]
    export = client.call("model.export", {"analysis_id": analysis,
                          "output_directory": str(root / "artifact")}, context, "export",
                         extra={"expected_profile": profile})
    export = wait_task(client, export, context)
    check(export["state"] == "succeeded", export)
    artifact = "artifact-" + export["task_id"]
    check(client.call("artifact.get", {"artifact_id": artifact}, context)["verified"],
          "Existing actual export publication is not verified")
    return context, profile, {"analysis_id": analysis, "artifact_id": artifact,
                              "run_config_id": "test-only-config"}


def admitted(host, parameters, context, profile, key="start"):
    task = host.client.call("analysis.start", parameters, context, key,
                            extra={"expected_profile": profile})
    check(task["state"] in {"queued", "running", "failed"}, task)
    return task, "run-" + task["task_id"]


def wait_started(root, run_id):
    deadline = time.monotonic() + 10
    while not (root / "runs" / run_id / "started").exists():
        check(time.monotonic() < deadline, "Test-only subprocess did not execute")
        time.sleep(.01)


def run_field_spans(project, run_id):
    """Locate exact fields in a saved, low-privilege portable project input."""
    reader = RecordReader(project)
    check(reader.text() == b"QCAE-RECORD-PROJECT" and reader.u64() == 2, "Project format")
    for _ in range(3):
        reader.text()
    for _ in range(reader.u64()):
        reader.text()
    payload_span = None
    for _ in range(reader.u64()):
        space, identity = reader.u64(), reader.text()
        begin, end = reader.text_span()
        if space == 7 and identity == run_id.encode():
            envelope = RecordReader(project[begin:end], begin)
            check(envelope.text() == b"QCAE-OWNED-ROW" and envelope.u64() == 1,
                  "Expected actual owned row envelope")
            check(envelope.text() == b"qcae.solver.run" and envelope.u64() == 1,
                  "Expected actual solver owner")
            payload_span = envelope.text_span()
            check(envelope.offset == end - begin, "Trailing owned row envelope")
    check(reader.offset == len(project) and payload_span is not None, "Missing actual run row")
    begin, end = payload_span
    count = struct.unpack_from("<I", project, begin)[0]
    offset = begin + 4
    spans = []
    for _ in range(count):
        length = struct.unpack_from("<I", project, offset)[0]
        offset += 4
        spans.append((offset, offset + length))
        offset += length
    check(offset == end, "Solver binary field framing")
    argument_count = int(project[slice(*spans[32])])
    output_count = int(project[slice(*spans[33 + argument_count])])
    stages = 34 + argument_count + output_count
    return {"principal": spans[1], "key": spans[2], "signature": spans[3],
            "test_only": spans[5], "manifest_digest": spans[16], "configuration_digest": spans[28],
            "parsing": spans[stages + 2], "numerical": spans[stages + 3]}


def carried_corruption(host, root, source, run_id, parameters, profile):
    with sqlite3.connect(source) as database:
        original = database.execute("SELECT payload FROM project WHERE id=1").fetchone()[0]
    spans = run_field_spans(original, run_id)
    count = 0
    for name in ("unchanged", *spans):
        destination = root / f"carried-{name}.qcae"
        with sqlite3.connect(source) as source_db, sqlite3.connect(destination) as target:
            source_db.backup(target)
            damaged = bytearray(original)
            if name != "unchanged":
                at = spans[name][1] - 1
                damaged[at] = ord("0") if damaged[at] != ord("0") else ord("1")
            target.execute("UPDATE project SET payload=? WHERE id=1", (bytes(damaged),))
        before = owned_facts(host.workspace)
        opened = host.client.call("project.open", {"mode": "normal", "path": str(destination)},
                                  key="open-" + name, expected=None)
        if "status" in opened:
            check(name not in {"unchanged", "principal", "key", "manifest_digest"} and
                  opened["status"] == "failed", f"Unexpected open rejection: {name}: {opened}")
            check(owned_facts(host.workspace) == before, "Malformed portable input partially installed")
        else:
            if name == "unchanged":
                run = host.client.call("analysis.get_run", {"run_id": run_id}, opened)
                check(run["test_only"] and run["source_kind"] == "test_process" and
                      not run["results_available"], "Carried raw observation became real acceptance")
                before_replay = owned_facts(host.workspace)
                host.client.call("analysis.start", parameters, opened, "start",
                                 extra={"expected_profile": profile}, expected="conflict")
                check(owned_facts(host.workspace) == before_replay and
                      len(list((root / "runs").iterdir())) == 1,
                      "Old carried doc/epoch replay changed facts or executed another child")
            else:
                before_get = owned_facts(host.workspace)
                host.client.call("analysis.get_run", {"run_id": run_id}, opened, expected="failed")
                check(owned_facts(host.workspace) == before_get and
                      host.client.current()["revision"] == opened["revision"],
                      f"Rejected {name} cross-row corruption partially committed")
            host.client.call("project.close", {"policy": "discard"}, opened, "close-" + name)
        count += name != "unchanged"
    return count


def owned_payload_span(project, identity, owner, schema):
    reader = RecordReader(project)
    check(reader.text() == b"QCAE-RECORD-PROJECT" and reader.u64() == 2, "Project format")
    for _ in range(3):
        reader.text()
    for _ in range(reader.u64()):
        reader.text()
    found = None
    for _ in range(reader.u64()):
        space, row_id = reader.u64(), reader.text()
        begin, end = reader.text_span()
        if space == 7 and row_id == identity.encode():
            envelope = RecordReader(project[begin:end], begin)
            check(envelope.text() == b"QCAE-OWNED-ROW" and envelope.u64() == 1,
                  "Actual result-owned envelope")
            check(envelope.text() == owner.encode() and envelope.u64() == schema,
                  "Actual result-owned schema")
            found = envelope.text_span()
            check(envelope.offset == end - begin, "Trailing result-owned envelope")
    check(reader.offset == len(project) and found is not None, "Missing actual result-owned row")
    return found


def string_field_spans(project, begin, end):
    count = struct.unpack_from("<I", project, begin)[0]
    check(count <= 16384, "Unexpected result field count")
    offset = begin + 4
    spans = []
    for _ in range(count):
        check(offset + 4 <= end, "Truncated result framing")
        length = struct.unpack_from("<I", project, offset)[0]
        offset += 4
        check(offset + length <= end, "Truncated result field")
        spans.append((offset, offset + length))
        offset += length
    check(offset == end, "Trailing result field framing")
    return spans


def carried_result_corruption(host, root, source, run_id, result_id):
    """Damage portable input, never the production workspace write path."""
    with sqlite3.connect(source) as database:
        original = database.execute("SELECT payload FROM project WHERE id=1").fetchone()[0]
    run_fields = string_field_spans(original, *owned_payload_span(
        original, run_id, "qcae.solver.run", 3))
    argument_count = int(original[slice(*run_fields[32])])
    output_count = int(original[slice(*run_fields[33 + argument_count])])
    stages = 34 + argument_count + output_count
    result_fields = string_field_spans(original, *owned_payload_span(
        original, result_id, "qcae.solver.result", 1))
    parsed_fields = string_field_spans(original, *result_fields[2])
    spans = {"cancellation_observation": run_fields[stages + 5],
             "run_published": run_fields[-1], "result_published": result_fields[1],
             "parsed_component": parsed_fields[-1], "test_only_origin": run_fields[5]}
    negatives = 0
    for name in ("unchanged", *spans):
        destination = root / f"result-carried-{name}.qcae"
        with sqlite3.connect(source) as source_db, sqlite3.connect(destination) as target:
            source_db.backup(target)
            damaged = bytearray(original)
            if name != "unchanged":
                at = spans[name][1] - 1
                damaged[at] = ord("0") if damaged[at] != ord("0") else ord("1")
            target.execute("UPDATE project SET payload=? WHERE id=1", (bytes(damaged),))
        before = owned_facts(host.workspace)
        opened = host.client.call("project.open", {"mode": "normal", "path": str(destination)},
                                  key="result-open-" + name, expected=None)
        if "status" in opened:
            check(name != "unchanged" and opened["status"] == "failed", opened)
            check(owned_facts(host.workspace) == before,
                  f"Rejected {name} portable result partially installed")
        else:
            before_query = owned_facts(host.workspace)
            if name == "unchanged":
                result = host.client.call("analysis.get_result", {"run_id": run_id}, opened)
                check(result["source_kind"] == "test_process" and result["raw_resources_verified"] and
                      result["numerical_validation"] == "not_run", result)
            else:
                host.client.call("analysis.get_result", {"run_id": run_id}, opened, expected="failed")
                negatives += 1
            check(owned_facts(host.workspace) == before_query and
                  host.client.current()["revision"] == opened["revision"],
                  f"Read-only result check committed partial {name} facts")
            host.client.call("project.close", {"policy": "discard"}, opened, "result-close-" + name)
        if name != "unchanged" and "status" in opened:
            negatives += 1
    return negatives


def parsed_scenario(engine, root, mode, transcripts):
    root.mkdir()
    config = configuration(root, mode, with_reader=True)
    with (root / "engine.log").open("w+") as log:
        host = SolverHost(engine, root, log, config)
        try:
            capabilities = host.start()
            client = host.client
            status = client.call("solver.configuration")
            check(status["test_only"] and status["result_reader_available"] and
                  not status["validated"] and not status["numerical_validation_available"], status)
            context, profile, parameters = prepare(host, capabilities, root)
            mapping = [row for row in json.loads((root / "artifact/manifest.json").read_text())[
                "export_id_map"] if row["namespace"] == "GRID"]
            task, run_id = admitted(host, parameters, context, profile)
            wait_started(root, run_id)
            running = client.call("analysis.get_run", {"run_id": run_id}, context)
            check(running["parsing"] == "not_run" and not running["results_available"], running)
            client.call("analysis.get_result", {"run_id": run_id}, context, expected="failed")
            check(not (root / "runs" / run_id / ".qcae-result").exists(),
                  "Pending child published result files")
            if mode == "parsed-stale":
                material = client.call("entity.query", {"kind": "material"}, context)["entities"][0]["entity_id"]
                client.call("material.set_young_modulus", {"entity_id": material,
                            "young_modulus": {"value": 200, "unit": "GPa"}}, context, "edit")
            current = client.current()
            (root / "runs" / run_id / "allow-output").write_text("explicit test-only gate")
            finished = wait_task(client, task, current)
            observed = client.call("analysis.get_run", {"run_id": run_id}, current)
            check(observed["execution"] == "exited" and observed["exit_code"] == "0" and
                  observed["source_kind"] == "test_process" and observed["numerical_validation"] == "not_run",
                  "A parsed test child became a real or numerically accepted solver")
            check(client.current()["revision"] == current["revision"],
                  "Parsed publication committed engineering model changes")
            if mode in {"parsed-wrongcase", "parsed-damaged"}:
                check(finished["state"] == "failed" and observed["parsing"] == "failed" and
                      not observed["results_available"], (finished, observed))
                client.call("analysis.get_result", {"run_id": run_id}, current, expected="failed")
                check(not (root / "runs" / run_id / ".qcae-result").exists() and
                      not any(row[1].startswith("result-") for row in owned_facts(host.workspace)),
                      "Rejected parser output partially published files or result facts")
                return {"mode": mode, "task_state": "failed", "parsing": "failed",
                        "zero_partial_publication": True, "numerical_validation": "not_run"}
            check(finished["state"] == "succeeded" and observed["parsing"] == "parsed" and
                  observed["results_available"], (finished, observed))
            result = client.call("analysis.get_result", {"run_id": run_id}, current)
            result_id = "result-" + task["task_id"]
            check(result["result_id"] == result_id and result["source_kind"] == "test_process" and
                  result["test_only"] and result["parsing"] == "parsed" and
                  result["numerical_validation"] == "not_run" and result["raw_resources_verified"] and
                  result["input_current"] == (mode != "parsed-stale") and
                  result["input_revision"] == context["revision"] and result["subcase"] == "1" and
                  result["unit_system"] == "mm-N-MPa" and result["coordinate_basis"] == "basic", result)
            fields = {field["quantity_id"]: field for field in result["fields"]}
            displacement = fields["displacement"]
            check(displacement["component_names"] == ["T1", "T2", "T3", "R1", "R2", "R3"] and
                  displacement["component_units"] == ["mm", "mm", "mm", "rad", "rad", "rad"] and
                  fields["spc_reaction"]["component_units"] == ["N", "N", "N", "N*mm", "N*mm", "N*mm"],
                  "Parsed publication lost per-component units")
            check({(row["entity_id"], row["solver_number"]) for row in displacement["values"]} ==
                  {(row["entity_id"], str(row["number"])) for row in mapping} and
                  all(len(row["components"]) == 6 for field in result["fields"] for row in field["values"]),
                  "Published fields used a current or incomplete GRID map")
            if mode == "parsed-wrongphysics":
                check(all(row["components"][1] == 0 for row in displacement["values"]),
                      "Test wrong-physics values were replaced by an analytical expectation")
            directory = root / "runs" / run_id / ".qcae-result"
            manifest_bytes = (directory / "manifest.json").read_bytes()
            manifest = json.loads(manifest_bytes)
            check(manifest["source_kind"] == "test_process" and
                  manifest["numerical_validation"] == "not_run" and manifest["artifact_id"] == result_id and
                  len(manifest["files"]) == 4 and len(manifest["origin_run_sha256"]) == 64,
                  "Final manifest lacks complete original process/input provenance")
            for item in manifest["files"]:
                data = (directory / item["path"]).read_bytes()
                check(len(data) == int(item["byte_length"]) and
                      hashlib.sha256(data).hexdigest() == item["sha256"], item)
            check(finished["artifact_receipt"] == {"artifact_id": result_id,
                  "input_revision": context["revision"],
                  "manifest_sha256": hashlib.sha256(manifest_bytes).hexdigest()} and
                  result["result_manifest_sha256"] == hashlib.sha256(manifest_bytes).hexdigest(),
                  "Task, run and result did not publish the same final manifest")
            replay = client.call("analysis.start", parameters, context, "start",
                                 extra={"expected_profile": profile})
            check(replay["task_id"] == task["task_id"] and replay["events"] == finished["events"] and
                  len(list((root / "runs").iterdir())) == 1, "Parsed publication replay executed a second child")
            negatives = 0
            recovered = False
            if mode == "parsed-success":
                original_raw = root / "runs" / run_id / "result.f06"
                raw_bytes = original_raw.read_bytes()
                original_raw.write_bytes(b"x" * len(raw_bytes))
                check(client.call("analysis.get_result", {"run_id": run_id}, current)["raw_resources_verified"],
                      "Post-publication raw run mutation replaced the frozen result artifact")
                original_raw.write_bytes(raw_bytes)
                final_raw = directory / "result.f06"
                final_raw.write_bytes(b"x" * len(raw_bytes))
                damaged = client.call("analysis.get_result", {"run_id": run_id}, current)
                check(not damaged["raw_resources_verified"] and damaged["fields"] == result["fields"] and
                      damaged["numerical_validation"] == "not_run",
                      "Damaged external artifact was declared verified or replaced database facts")
                final_raw.write_bytes(raw_bytes)
                host.process.kill()
                host.process.wait(timeout=5)
                host.process = None
                client.instance_id = None
                check(host.start()["recovery_available"], "Published result had no durable recovery")
                current = client.call("project.open", {"mode": "recover"}, key="recover-result")
                check(client.call("task.status", {"task_id": task["task_id"]}, current)["state"] == "succeeded" and
                      client.call("analysis.get_result", {"run_id": run_id}, current)["fields"] == result["fields"] and
                      len(list((root / "runs").iterdir())) == 1,
                      "Restart lost the published result or re-executed the completed child")
                recovered = True
                carried = root / "parsed-carried.qcae"
                client.call("project.save", {"path": str(carried)}, current, "save-result")
                client.call("project.close", {"policy": "discard"}, current, "close-result")
                negatives = carried_result_corruption(host, root, carried, run_id, result_id)
            return {"mode": mode, "task_state": "succeeded", "parsing": "parsed",
                    "fields": len(result["fields"]), "numerical_validation": "not_run",
                    "cross_row_negative_cases": negatives, "published_recovery": recovered}
        finally:
            transcripts.append(host.client.transcript)
            host.close()


def scenario(engine, root, mode, transcripts):
    root.mkdir()
    config = configuration(root, mode)
    with (root / "engine.log").open("w+") as log:
        host = SolverHost(engine, root, log, config)
        try:
            capabilities = host.start()
            client = host.client
            declared = {item["name"]: item for item in capabilities["operations"]}
            status = client.call("solver.configuration")
            check(declared["analysis.start"]["available"] and status["configured"] and
                  status["process_adapter_ready"] and status["test_only"] and not status["validated"] and
                  not status["version_probe_available"] and status["version_probe"] == {} and
                  not status["result_reader_available"] and not status["numerical_validation_available"],
                  "Test process was certified as actual Nastran")
            context, profile, parameters = prepare(host, capabilities, root)
            originals = {path.name: path.read_bytes() for path in (root / "artifact").rglob("*")
                         if path.is_file()}
            if mode == "success":
                before = owned_facts(host.workspace)
                for name, values, extra, ctx in (
                        ("wrong-config", dict(parameters, run_config_id="not-installed"), profile, context),
                        ("raw-executable", dict(parameters, executable=sys.executable), profile, context),
                        ("wrong-profile", parameters, dict(profile, definition_digest="wrong"), context),
                        ("stale", parameters, profile,
                         dict(context, revision=str(int(context["revision"]) - 1)))):
                    rejected = client.call("analysis.start", values, ctx, name,
                                           extra={"expected_profile": extra}, expected=None)
                    check(rejected.get("status") in {"failed", "conflict"}, rejected)
                    check(owned_facts(host.workspace) == before and not list((root / "runs").iterdir()),
                          f"Rejected {name} admitted task or process side effects")
                manifest = root / "artifact/manifest.json"
                valid_manifest = manifest.read_bytes()
                manifest.write_bytes(b"x" * len(valid_manifest))
                client.call("analysis.start", parameters, context, "bad-artifact",
                            extra={"expected_profile": profile}, expected="failed")
                check(owned_facts(host.workspace) == before and not list((root / "runs").iterdir()),
                      "Bad frozen manifest reached task or startup admission")
                manifest.write_bytes(valid_manifest)
            task, run_id = admitted(host, parameters, context, profile)
            wait_started(root, run_id)
            if mode == "slow":
                material = client.call("entity.query", {"kind": "material"}, context)["entities"][0]["entity_id"]
                client.call("material.set_young_modulus", {"entity_id": material,
                            "young_modulus": {"value": 200, "unit": "GPa"}}, context, "edit")
                current = client.current()
                observed = client.call("analysis.get_run", {"run_id": run_id}, current)
                check(not observed["input_current"] and observed["input_revision"] == context["revision"],
                      "New physical revision replaced running frozen input")
            elif mode == "ignore-term":
                client.call("task.cancel", {"task_id": task["task_id"]}, context)
            elif mode == "restart":
                host.process.kill()
                host.process.wait(timeout=5)
                host.process = None
                client.instance_id = None
                check(host.start()["recovery_available"], "Missing explicit restart recovery")
                recovered = client.call("project.open", {"mode": "recover"}, key="recover")
                restored = client.call("analysis.get_run", {"run_id": run_id}, recovered)
                check(restored["execution"] == "outcome_unknown" and not restored["results_available"],
                      "Restart guessed process outcome or certified results")
                restored_task = client.call("task.status", {"task_id": task["task_id"]}, recovered)
                check(restored_task["state"] in {"interrupted", "outcome_unknown"}, restored_task)
                client.call("analysis.start", parameters, recovered, "start",
                            extra={"expected_profile": profile}, expected="conflict")
                check(len(list((root / "runs").iterdir())) == 1,
                      "Old-epoch retry executed a second process after recovery")
                client.call("project.close", {"policy": "discard"}, recovered, "close-unknown",
                            expected="conflict")
                # The orphan is our explicit bounded test-only child; wait for its natural exit.
                deadline = time.monotonic() + 4
                while not (root / "runs" / run_id / "result.f06").exists():
                    check(time.monotonic() < deadline, "Bounded restart child did not naturally finish")
                    time.sleep(.02)
                return {"mode": mode, "unknown_recovery": True, "rerun": False}
            finished = wait_task(client, task, client.current())
            observed = client.call("analysis.get_run", {"run_id": run_id}, client.current())
            check(observed["parsing"] == "not_run" and observed["numerical_validation"] == "not_run" and
                  not observed["results_available"] and observed["source_kind"] == "test_process",
                  "Exit, corrupt output or cancellation became engineering acceptance")
            if mode == "ignore-term":
                check(finished["state"] == "cancelled" and observed["execution"] == "cancelled" and
                      observed["termination_signal"] == "9" and observed["cancellation_requested"],
                      "Cancellation intent became completed without actual owned group termination")
            else:
                check(finished["state"] == "failed" and observed["execution"] == "exited" and
                      observed["exit_code"] == "0", finished)
                check(len(observed["outputs"]) == (0 if mode == "missing" else 3), observed)
                for item in observed["outputs"]:
                    data = (root / "runs" / run_id / item["path"]).read_bytes()
                    check(int(item["byte_length"]) == len(data) and
                          item["sha256"] == hashlib.sha256(data).hexdigest(), item)
            check(client.current()["revision"] == (current["revision"] if mode == "slow" else context["revision"]),
                  "Process task unexpectedly edited engineering model")
            check(all(path.read_bytes() == originals[path.name]
                      for path in (root / "artifact").rglob("*") if path.is_file()),
                  "Solver subprocess changed original published input")
            replay = client.call("analysis.start", parameters, context, "start",
                                 extra={"expected_profile": profile})
            check(replay["task_id"] == task["task_id"] and replay["events"] == finished["events"] and
                  len(list((root / "runs").iterdir())) == 1, "Idempotent analysis reran a process")
            negatives = 0
            if mode == "success":
                carried = root / "carried.qcae"
                client.call("project.save", {"path": str(carried)}, context, "save")
                client.call("project.close", {"policy": "discard"}, context, "close")
                negatives = carried_corruption(host, root, carried, run_id, parameters, profile)
            return {"mode": mode, "execution": observed["execution"],
                    "task_state": finished["state"], "cross_row_negative_cases": negatives}
        finally:
            transcripts.append(host.client.transcript)
            host.close()


def run(engine, evidence):
    transcripts = []
    with tempfile.TemporaryDirectory(prefix="qcae-solver-ipc-", dir=Path("/tmp").resolve()) as folder:
        root = Path(folder)
        for mode in ("unconfigured", "declared-real", "synthetic-version"):
            scope = root / mode
            scope.mkdir()
            config = configuration(scope, "success", real_declaration=True,
                                   synthetic_version=mode == "synthetic-version") if mode != "unconfigured" else None
            with (scope / "engine.log").open("w+") as log:
                host = SolverHost(engine, scope, log, config)
                try:
                    capabilities = host.start()
                    declared = {item["name"]: item for item in capabilities["operations"]}
                    status = host.client.call("solver.configuration")
                    check(not declared["analysis.start"]["available"] and not status["validated"] and
                          not status["process_adapter_ready"] and
                          status["configured"] == (mode != "unconfigured"),
                          "Configuration declaration falsely proves an actual installed solver")
                    check(status["version_probe_available"] == (mode != "unconfigured"),
                          "Actual fixed information-command observation was misreported")
                    if mode == "declared-real":
                        probe = status["version_probe"]
                        check(probe["reported_version"] == "" and probe["exit_code"] != "0" and
                              not probe["synthetic"] and len(probe["executable_sha256"]) == 64,
                              "Actual Python help failure was replaced by configured version text")
                    elif mode == "synthetic-version":
                        probe = status["version_probe"]
                        check(probe["reported_version"] == "2024.1" and probe["exit_code"] == "0" and
                              probe["synthetic"] and not list((scope / "runs").iterdir()),
                              "A matching synthetic header was promoted to real solver readiness")
                finally:
                    transcripts.append(host.client.transcript)
                    host.close()
        cases = [scenario(engine, root / mode, mode, transcripts)
                 for mode in ("success", "corrupt", "missing", "slow", "ignore-term", "restart")]
        parsed_cases = [parsed_scenario(engine, root / mode, mode, transcripts)
                        for mode in ("parsed-success", "parsed-wrongcase", "parsed-damaged",
                                     "parsed-wrongphysics", "parsed-stale")]
    if evidence:
        evidence.mkdir(parents=True, exist_ok=True)
        (evidence / "solver-run-ipc-transcript.json").write_text(json.dumps(transcripts, indent=2) + "\n")
    return {"passed": True, "scenarios": cases, "parsed_test_process_scenarios": parsed_cases,
            "real_solver": False, "parsing_acceptance": False, "numerical_acceptance": False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True)
    parser.add_argument("--evidence-dir", type=Path)
    args = parser.parse_args()
    print(json.dumps(run(str(Path(args.engine).resolve()), args.evidence_dir), indent=2))


if __name__ == "__main__":
    main()
