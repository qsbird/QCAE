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
from concurrent.futures import ThreadPoolExecutor

from c3_sync_ipc_tests import EngineProcess, IpcClient, check
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
if mode.startswith(("parsed-", "numeric-")):
    # A real filesystem barrier lets the IPC test inspect the unparsed state.
    # It is a test-only child protocol, not a production solver feature.
    deadline = time.monotonic() + 8
    while not Path("allow-output").exists():
        assert time.monotonic() < deadline, "test-only output gate timed out"
        time.sleep(.01)
    manifest = json.loads((Path(source).parent / "manifest.json").read_text())
    numbers = sorted(int(row["number"]) for row in manifest["export_id_map"]
                     if row["namespace"] == "GRID")
    if mode.startswith("numeric-"):
        # The analytic CHILD is explicit synthetic test data. Coordinates come
        # from the frozen BDF, numbers from the original published manifest.
        nodes = {}
        for file in Path(source).parent.rglob("*.bdf"):
            for line in file.read_text().splitlines():
                row = line.strip().split(",")
                if row[0] == "GRID":
                    nodes[int(row[1])] = float(row[3])
        assert len(nodes) == 21 and set(nodes) == set(numbers)
        rows = []
        for number in numbers:
            x = nodes[number]
            uy = -x*x*(3000-x)/(6*210000*833.333)
            rz = -x*(2000-x)/(2*210000*833.333)
            if mode == "numeric-wrongdirection" and x == 500:
                uy = -uy
            if mode == "numeric-zero":
                uy, rz = 0, 0
            z = 1e-5 if mode == "numeric-boundary" and x == 0 else (
                1.00000000001e-5 if mode == "numeric-over-boundary" and x == 0 else 0)
            rows.append(f"{number} G 0 {uy:.17g} {z:.17g} 0 0 {rz:.17g}\n")
        support = next(number for number, x in nodes.items() if x == 0)
        moment = -1000 if mode == "numeric-wrongreaction" else 1000
        f06 = ("1 EXPLICIT SYNTHETIC ANALYTIC CHILD PAGE 1\nSUBCASE 1\n"
               "D I S P L A C E M E N T V E C T O R\n"
               "POINT ID. TYPE T1 T2 T3 R1 R2 R3\n" + "".join(rows) +
               "F O R C E S O F S I N G L E - P O I N T C O N S T R A I N T\n"
               "POINT ID. TYPE T1 T2 T3 R1 R2 R3\n"
               f"{support} G 0 1 0 0 0 {moment}\n")
        Path("result.f06").write_text(f06)
        print("explicit synthetic analytic child completed; not Nastran", flush=True)
        sys.exit(0)
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


def prepare(host, capabilities, root, *, benchmark=False):
    client = host.client
    profile = capabilities["declared_solver_profiles"][0]["profile_ref"]
    context = client.call("project.create", {"name": "Test-only local execution"}, key="create")
    fixtures = Path(__file__).parent / ("fixtures/nastran-real-benchmark-v3" if benchmark else
                                       "fixtures/nastran")
    paths = (("cantilever.bdf", "nodes.bdf", "beams.bdf", "properties.bdf") if benchmark else
             ("cantilever.bdf", "mesh/nodes.bdf", "mesh/beams.bdf", "properties.bdf"))
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
            elif mode == "parsed-stage-failure":
                (root / "runs" / run_id / ".qcae-result").write_text("explicit test-only obstruction")
            current = client.current()
            (root / "runs" / run_id / "allow-output").write_text("explicit test-only gate")
            finished = wait_task(client, task, current)
            observed = client.call("analysis.get_run", {"run_id": run_id}, current)
            check(observed["execution"] == "exited" and observed["exit_code"] == "0" and
                  observed["source_kind"] == "test_process" and observed["numerical_validation"] == "not_run",
                  "A parsed test child became a real or numerically accepted solver")
            check(client.current()["revision"] == current["revision"],
                  "Parsed publication committed engineering model changes")
            if mode == "parsed-stage-failure":
                check(finished["state"] == "failed" and observed["parsing"] == "parsed" and
                      not observed["results_available"], (finished, observed))
                before = owned_facts(host.workspace)
                rejected = client.call("analysis.reconcile_result", {"run_id": run_id}, current,
                                       "failed-reconcile", expected="failed")
                check(rejected["error"]["code"] == "INVALID_INPUT" and
                      "failed/cancelled" in rejected["error"]["message"] and
                      owned_facts(host.workspace) == before and
                      client.call("task.status", {"task_id": task["task_id"]}, current)["state"] == "failed",
                      "A failed staged result task was revived or partially installed")
                return {"mode": mode, "task_state": "failed", "parsing": "parsed",
                        "durable_intent": True, "reconcile_rejected": True,
                        "numerical_validation": "not_run"}
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
            before_numeric = owned_facts(host.workspace)
            unsupported = client.call("analysis.validate_result", {"run_id": run_id}, current,
                                      "unsupported-two-node", extra={"expected_profile": profile},
                                      expected="failed")
            check(unsupported["error"]["code"] == "UNSUPPORTED_CAPABILITY" and
                  owned_facts(host.workspace) == before_numeric,
                  "The old two-node parsed fixture produced a numerical report or partial fact")
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
            reconciled = client.call("analysis.reconcile_result", {"run_id": run_id}, current, "reconcile")
            repeated = client.call("analysis.reconcile_result", {"run_id": run_id}, current, "reconcile")
            check(not reconciled["replayed"] and repeated["replayed"] and
                  repeated["current_files_verified"] and repeated["verified_at_reconcile"] and
                  repeated["source_kind"] == "test_process" and repeated["numerical_validation"] == "not_run" and
                  client.call("task.status", {"task_id": task["task_id"]}, current)["events"] == finished["events"],
                  "Already-completed result reconcile duplicated task events or changed provenance")
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
                historical = client.call("analysis.reconcile_result", {"run_id": run_id}, current, "reconcile")
                before_bad = owned_facts(host.workspace)
                client.call("analysis.reconcile_result", {"run_id": run_id}, current,
                            "corrupt-result-new-key", expected="failed")
                check(historical["replayed"] and historical["verified_at_reconcile"] and
                      not historical["current_files_verified"] and owned_facts(host.workspace) == before_bad,
                      "Historical replay disguised corrupt files or a fresh failure committed facts")
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


def carried_validation_corruption(host, root, source, run_id, report_id):
    """Only mutate a saved low-privilege project, never the live workspace."""
    with sqlite3.connect(source) as database:
        original = database.execute("SELECT payload FROM project WHERE id=1").fetchone()[0]
    fields = string_field_spans(original, *owned_payload_span(
        original, report_id, "qcae.solver.validation", 1))
    report = string_field_spans(original, *fields[15])
    component = string_field_spans(original, *report[3])
    prefix = "validation-" + hashlib.sha256(run_id.encode()).hexdigest()[:32] + "-"
    head = string_field_spans(original, *owned_payload_span(
        original, prefix + "head", "qcae.solver.validation", 2))
    cases = ("test_only", "parsed_digest", "ordinal", "tolerance", "consistent_mismatch", "head", "overflow")
    for name in ("unchanged", *cases):
        destination = root / f"validation-carried-{name}.qcae"
        with sqlite3.connect(source) as source_db, sqlite3.connect(destination) as target:
            source_db.backup(target)
            damaged = bytearray(original)
            if name == "test_only":
                damaged[slice(*fields[14])] = b"\0"
            elif name == "parsed_digest":
                at = fields[9][0]
                damaged[at] = ord("0") if damaged[at] != ord("0") else ord("1")
            elif name == "ordinal":
                damaged[slice(*fields[4])] = b"9"
            elif name == "tolerance":
                old = struct.unpack("<d", original[slice(*component[8])])[0]
                damaged[slice(*component[8])] = struct.pack("<d", old * 2)
            elif name == "consistent_mismatch":
                # All internal error/aggregate relations remain valid. Only the
                # trusted original-source reference recomputation can reject it.
                expected = struct.unpack("<d", original[slice(*component[6])])[0]
                damaged[slice(*component[5])] = struct.pack("<d", expected + 1)
                damaged[slice(*component[7])] = struct.pack("<d", 1)
                damaged[slice(*component[9])] = b"\0"
                damaged[slice(*report[2])] = b"\0"
            elif name == "head":
                at = head[3][1] - 1
                damaged[at] = ord("0") if damaged[at] != ord("0") else ord("1")
            elif name == "overflow":
                # Add a structurally valid unindexed report to low-privilege saved
                # input. The prefix reader must stop at 17 and explicitly report
                # overflow without claiming a latest applicable validation.
                reader = RecordReader(original)
                reader.text()
                reader.u64()
                for _ in range(3):
                    reader.text()
                for _ in range(reader.u64()):
                    reader.text()
                count_at = reader.offset
                count = reader.u64()
                new_key = b"o" * (fields[2][1] - fields[2][0])
                principal = original[slice(*fields[1])]
                parts = (principal, new_key)
                key_wire = struct.pack("<I", 2) + b"".join(struct.pack("<I", len(part)) + part for part in parts)
                new_id = prefix + hashlib.sha256(key_wire).hexdigest()
                payload_begin, payload_end = owned_payload_span(original, report_id, "qcae.solver.validation", 1)
                payload = bytearray(original[payload_begin:payload_end])
                payload[fields[2][0] - payload_begin:fields[2][1] - payload_begin] = new_key
                def text_wire(value):
                    return struct.pack("<Q", len(value)) + value
                envelope = (text_wire(b"QCAE-OWNED-ROW") + struct.pack("<Q", 1) +
                            text_wire(b"qcae.solver.validation") + struct.pack("<Q", 1) + text_wire(payload))
                damaged[count_at:count_at + 8] = struct.pack("<Q", count + 1)
                damaged.extend(struct.pack("<Q", 7) + text_wire(new_id.encode()) + text_wire(envelope))
            target.execute("UPDATE project SET payload=? WHERE id=1", (bytes(damaged),))
        before = owned_facts(host.workspace)
        opened = host.client.call("project.open", {"mode": "normal", "path": str(destination)},
                                  key="open-validation-" + name, expected=None)
        if "status" in opened:
            check(name != "unchanged" and opened["status"] == "failed" and
                  owned_facts(host.workspace) == before, "Malformed report partially opened")
        else:
            before_get = owned_facts(host.workspace)
            got = host.client.call("analysis.get_result", {"run_id": run_id}, opened,
                                   expected="success" if name in {"unchanged", "overflow"} else "failed")
            if name == "overflow":
                checks = got["numerical_checks"]
                check(checks["history_overflow"] and not checks["has_applicable_validation"] and
                      checks["latest_applicable_summary"] == {} and checks["immutable_history"] == [],
                      "Overflow concealed missing index entries or advertised an applicable report")
                overflow_ctx = opened
                profile = host.client.call("capabilities.list")["declared_solver_profiles"][0]["profile_ref"]
                rejected = host.client.call("analysis.validate_result", {"run_id": run_id}, overflow_ctx,
                                           "overflow-reject", extra={"expected_profile": profile}, expected="failed")
                check(rejected["error"]["code"] == "RESOURCE_LIMIT", rejected)
            elif name == "unchanged":
                checks = got["numerical_checks"]
                check(len(checks["immutable_history"]) == 16 and
                      checks["latest_applicable_summary"]["numerical_stage"] == "not_run" and
                      checks["latest_applicable_summary"]["check_kind"] == "synthetic_comparison",
                      "Carried synthetic reports became actual solver acceptance")
            check(owned_facts(host.workspace) == before_get and
                  host.client.current()["revision"] == opened["revision"],
                  "Report query or rejected corruption partially committed")
            host.client.call("project.close", {"policy": "discard"}, opened, "close-validation-" + name)
    return len(cases)


def numerical_scenario(engine, root, mode, transcripts):
    root.mkdir()
    config = configuration(root, mode, with_reader=True)
    with (root / "engine.log").open("w+") as log:
        host = SolverHost(engine, root, log, config)
        try:
            client = host.client
            context, profile, parameters = prepare(host, host.start(), root, benchmark=True)
            task, run_id = admitted(host, parameters, context, profile)
            wait_started(root, run_id)
            before_pending = owned_facts(host.workspace)
            client.call("analysis.validate_result", {"run_id": run_id}, context, "pending",
                        extra={"expected_profile": profile}, expected="failed")
            check(owned_facts(host.workspace) == before_pending,
                  "Pending process numerical admission wrote partial facts")
            if mode == "numeric-stale":
                material = client.call("entity.query", {"kind": "material"}, context)["entities"][0]["entity_id"]
                client.call("material.set_young_modulus", {"entity_id": material,
                            "young_modulus": {"value": 200, "unit": "GPa"}}, context, "stale-edit")
            current = client.current()
            (root / "runs" / run_id / "allow-output").write_text("explicit analytic child gate")
            finished = wait_task(client, task, current)
            check(finished["state"] == "succeeded", finished)
            baseline = owned_facts(host.workspace)
            directory = root / "runs" / run_id / ".qcae-result"
            files = {str(path): path.read_bytes() for path in directory.rglob("*") if path.is_file()}
            before = client.call("analysis.get_result", {"run_id": run_id}, current)
            check(not before["numerical_checks"]["has_applicable_validation"] and
                  before["numerical_checks"]["immutable_history"] == [], before)
            wrong_profile = dict(profile, definition_digest="wrong-registered-definition")
            client.call("analysis.validate_result", {"run_id": run_id}, current, "wrong-profile",
                        extra={"expected_profile": wrong_profile}, expected="failed")
            old = dict(current, revision=str(int(current["revision"]) - 1))
            client.call("analysis.validate_result", {"run_id": run_id}, old, "old-revision",
                        extra={"expected_profile": profile}, expected="conflict")
            check(owned_facts(host.workspace) == baseline,
                  "Profile/revision rejection partially committed a report")
            if mode == "numeric-match":
                prefix = "validation-" + hashlib.sha256(run_id.encode()).hexdigest()[:32] + "-"
                with sqlite3.connect(host.workspace) as database:
                    database.execute("CREATE TRIGGER numerical_fact_rollback BEFORE INSERT ON store_rows "
                                     f"WHEN new.space=7 AND substr(new.identity,1,{len(prefix)})='{prefix}' "
                                     "BEGIN SELECT RAISE(ABORT,'explicit test-only report rollback'); END")
                rollback = client.call("analysis.validate_result", {"run_id": run_id}, current,
                                       "validation", extra={"expected_profile": profile}, expected="failed")
                check(rollback["error"]["code"] == "STORAGE_FAILURE" and
                      owned_facts(host.workspace) == baseline and
                      all(Path(path).read_bytes() == data for path, data in files.items()),
                      "Report-only rollback became storage uncertainty or partial publication")
                with sqlite3.connect(host.workspace) as database:
                    database.execute("DROP TRIGGER numerical_fact_rollback")
            checked = client.call("analysis.validate_result", {"run_id": run_id}, current,
                                  "validation", extra={"expected_profile": profile})
            summary, components = checked["summary"], checked["components"]
            match = mode in {"numeric-match", "numeric-boundary", "numeric-stale"}
            check(summary["comparison"] == ("matched" if match else "mismatch") and
                  summary["check_kind"] == "synthetic_comparison" and summary["numerical_stage"] == "not_run" and
                  summary["source_kind"] == "test_process" and summary["component_count"] == "132" and
                  len(components) == 132 and checked["current_files_verified"] and
                  checked["original_source_state"] == ("stale" if mode == "numeric-stale" else "current"), checked)
            unique = {(row["entity_id"], row["quantity_id"], row["component_id"]) for row in components}
            check(len(unique) == 132 and {row["component_id"] for row in components} ==
                  {"T1", "T2", "T3", "R1", "R2", "R3"} and
                  all(row["absolute_error"] == abs(row["actual"] - row["expected"]) and
                      row["matched"] == (row["absolute_error"] <= row["tolerance"]) for row in components),
                  "Actual IPC omitted or mislabeled component comparisons")
            after = owned_facts(host.workspace)
            check(len(after) == len(baseline) + 2 and
                  all(row in after for row in baseline) and client.current()["revision"] == current["revision"],
                  "Numerical fact CAS changed original task/run/result/model facts")
            replay = client.call("analysis.validate_result", {"run_id": run_id}, current,
                                 "validation", extra={"expected_profile": profile})
            check(replay["replayed"] and replay["components"] == components and
                  owned_facts(host.workspace) == after, "Numerical retry duplicated or changed report facts")
            queried = client.call("analysis.get_result", {"run_id": run_id}, current)
            checks = queried["numerical_checks"]
            check(queried["numerical_validation"] == "not_run" and
                  checks["original_run_validation_stage"] == "not_run" and
                  checks["latest_applicable_summary"] == summary and
                  checks["original_source_state"] == checked["original_source_state"] and
                  checks["current_files_verified"] and not checks["history_overflow"], checks)
            negatives = 0
            if mode == "numeric-match":
                raw = directory / "result.f06"
                data = raw.read_bytes()
                raw.write_bytes(b"x" * len(data))
                historical = client.call("analysis.validate_result", {"run_id": run_id}, current,
                                         "validation", extra={"expected_profile": profile})
                client.call("analysis.validate_result", {"run_id": run_id}, current,
                            "bad-files", extra={"expected_profile": profile}, expected="failed")
                hist_query = client.call("analysis.get_result", {"run_id": run_id}, current)
                check(historical["replayed"] and not historical["current_files_verified"] and
                      not hist_query["numerical_checks"]["current_files_verified"] and
                      hist_query["numerical_checks"]["latest_applicable_summary"] == summary and
                      owned_facts(host.workspace) == after,
                      "Damaged files erased historical facts or admitted a fresh comparison")
                raw.write_bytes(data)
                competitors = [IpcClient(host.endpoint), IpcClient(host.endpoint)]
                def compete(index):
                    return competitors[index].call("analysis.validate_result", {"run_id": run_id}, current,
                                                   "concurrent-" + str(index), extra={"expected_profile": profile})
                with ThreadPoolExecutor(max_workers=2) as pool:
                    responses = list(pool.map(compete, (0, 1)))
                transcripts.extend(other.transcript for other in competitors)
                check({row["summary"]["ordinal"] for row in responses} == {"2", "3"},
                      "Concurrent actual IPC requests claimed the same history ordinal")
                for ordinal in range(4, 17):
                    response = client.call("analysis.validate_result", {"run_id": run_id}, current,
                                           "capacity-" + str(ordinal), extra={"expected_profile": profile})
                    check(response["summary"]["ordinal"] == str(ordinal), "History sequence skipped")
                full = owned_facts(host.workspace)
                overflow = client.call("analysis.validate_result", {"run_id": run_id}, current,
                                       "capacity-17", extra={"expected_profile": profile}, expected="failed")
                client.call("analysis.validate_result", {"run_id": run_id}, current,
                            "validation", extra={"expected_profile": profile})
                check(overflow["error"]["code"] == "RESOURCE_LIMIT" and owned_facts(host.workspace) == full,
                      "Capacity 17 partially committed or blocked an existing-key retry")
                final = client.call("analysis.get_result", {"run_id": run_id}, current)["numerical_checks"]
                check(len(final["immutable_history"]) == 16 and not final["history_overflow"] and
                      final["latest_applicable_summary"]["ordinal"] == "16" and
                      len(json.dumps(final["immutable_history"]).encode()) <= 32768,
                      "History query exceeded its declared count/byte bound")
                host.process.kill()
                host.process.wait(timeout=5)
                host.process = None
                client.instance_id = None
                check(host.start()["recovery_available"], "Numerical reports were not durable")
                recovered = client.call("project.open", {"mode": "recover"}, key="recover-validation")
                restored = client.call("analysis.get_result", {"run_id": run_id}, recovered)
                check(restored["numerical_checks"] == final and len(list((root / "runs").iterdir())) == 1,
                      "Restart changed numerical history or reran a solver")
                carried = root / "validation.qcae"
                client.call("project.save", {"path": str(carried)}, recovered, "save-validation")
                client.call("project.close", {"policy": "discard"}, recovered, "close-validation")
                negatives = carried_validation_corruption(host, root, carried, run_id, summary["validation_id"])
            check(all(Path(path).read_bytes() == data for path, data in files.items()),
                  "Numeric validation/recovery changed published result bytes")
            return {"mode": mode, "components": 132, "comparison": summary["comparison"],
                    "check_kind": "synthetic_comparison", "numerical_stage": "not_run",
                    "portable_negative_cases": negatives, "real_solver": False}
        finally:
            transcripts.append(host.client.transcript)
            host.close()


def publication_rollback_scenario(engine, root, transcripts):
    """The real SQLite adapter rolls back its final publication batch.

    The trigger is scoped to this test's temporary workspace and final receipt;
    there is no production fault opcode or alternative persistence authority.
    """
    root.mkdir()
    config = configuration(root, "parsed-db-rollback", with_reader=True)
    with (root / "engine.log").open("w+") as log:
        host = SolverHost(engine, root, log, config)
        try:
            capabilities = host.start()
            client = host.client
            context, profile, parameters = prepare(host, capabilities, root)
            task, run_id = admitted(host, parameters, context, profile)
            wait_started(root, run_id)
            result_id = "result-" + task["task_id"]
            # SQLite's INSERT ... ON CONFLICT DO UPDATE may enter either trigger.
            # A receipt appears only in the final successful task candidate.
            task_sql = "'" + task["task_id"].replace("'", "''") + "'"
            receipt_hex = result_id.encode().hex()
            with sqlite3.connect(host.workspace) as database:
                for event in ("INSERT", "UPDATE"):
                    database.execute(
                        f"CREATE TRIGGER qcae_test_publication_{event.lower()} BEFORE {event} ON store_rows "
                        f"WHEN NEW.space=6 AND NEW.identity={task_sql} AND "
                        f"instr(NEW.value,X'{receipt_hex}')>0 BEGIN "
                        "SELECT RAISE(ABORT,'QCAE test-only final publication rollback'); END")
            (root / "runs" / run_id / "allow-output").write_text("explicit test-only gate")
            finished = wait_task(client, task, context)
            observed = client.call("analysis.get_run", {"run_id": run_id}, context)
            directory = root / "runs" / run_id / ".qcae-result"
            manifest = json.loads((directory / "manifest.json").read_text())
            for item in manifest["files"]:
                data = (directory / item["path"]).read_bytes()
                check(len(data) == int(item["byte_length"]) and
                      hashlib.sha256(data).hexdigest() == item["sha256"], item)
            snapshot = {"task": finished, "run": observed,
                        "filesystem_published": True,
                        "model_revision_unchanged": client.current()["revision"] == context["revision"],
                        "results_available": observed["results_available"]}
            # Preserve the actual first failure independently of the temp directory.
            (root.parent / "publication-rollback-observation.json").write_text(json.dumps(snapshot, indent=2))
            check(finished["state"] == "outcome_unknown" and
                  finished["diagnostic"]["code"] == "STORAGE_UNCERTAIN" and
                  "STORAGE_FAILURE" in finished["diagnostic"]["message"] and
                  "write record failed" in finished["diagnostic"]["message"] and
                  finished["diagnostic"]["field"] == "storage",
                  f"Published filesystem/database rollback was treated as an ordinary task failure: {snapshot}")
            check(observed["parsing"] == "parsed" and not observed["results_available"] and
                  observed["numerical_validation"] == "not_run" and snapshot["model_revision_unchanged"],
                  "Unconfirmed database publication was installed or changed the engineering model")
            client.call("analysis.get_result", {"run_id": run_id}, context, expected="failed")
            with sqlite3.connect(host.workspace) as database:
                for event in ("insert", "update"):
                    database.execute(f"DROP TRIGGER qcae_test_publication_{event}")
            before_recovery = owned_facts(host.workspace)
            client.call("analysis.reconcile_result", {"run_id": run_id}, context,
                        "before-application-recovery", expected="failed")
            check(owned_facts(host.workspace) == before_recovery,
                  "Unfinished persisted publication bypassed application recovery")
            host.process.kill()
            host.process.wait(timeout=5)
            host.process = None
            client.instance_id = None
            check(host.start()["recovery_available"], "Unknown publication had no durable recovery")
            recovered = client.call("project.open", {"mode": "recover"}, key="recover-publication")
            restored = client.call("task.status", {"task_id": task["task_id"]}, recovered)
            check(restored["state"] in {"interrupted", "outcome_unknown"}, restored)
            before = owned_facts(host.workspace)
            negative_cases = 0
            for name, path in (("missing-manifest", directory / "manifest.json"),
                               ("bad-raw", directory / "result.f06"),
                               ("bad-qcr", directory / "parsed-result.qcr")):
                data = path.read_bytes()
                if name == "missing-manifest":
                    path.unlink()
                else:
                    path.write_bytes(b"x" * len(data))
                client.call("analysis.reconcile_result", {"run_id": run_id}, recovered,
                            name, expected="failed")
                check(owned_facts(host.workspace) == before and
                      client.current()["revision"] == recovered["revision"],
                      f"Rejected {name} recovery partially committed")
                path.write_bytes(data)
                negative_cases += 1
            stale = dict(recovered, revision=str(int(recovered["revision"]) - 1))
            client.call("analysis.reconcile_result", {"run_id": run_id}, stale,
                        "stale-reconcile", expected="conflict")
            bad_epoch = dict(recovered, document_epoch="explicit-old-epoch")
            rejected = client.call("analysis.reconcile_result", {"run_id": run_id}, bad_epoch,
                                   "old-epoch-reconcile", expected=None)
            check(rejected.get("status") in {"failed", "conflict"} and
                  rejected["error"]["code"] == "DOCUMENT_EPOCH_EXPIRED" and
                  owned_facts(host.workspace) == before, rejected)
            negative_cases += 2
            raw_files_before = {item["path"]: (directory / item["path"]).read_bytes()
                                for item in manifest["files"]}
            reconciled = client.call("analysis.reconcile_result", {"run_id": run_id}, recovered,
                                     "repair-publication")
            finished = client.call("task.status", {"task_id": task["task_id"]}, recovered)
            result = client.call("analysis.get_result", {"run_id": run_id}, recovered)
            repeated = client.call("analysis.reconcile_result", {"run_id": run_id}, recovered,
                                   "repair-publication")
            client.call("analysis.reconcile_result", {"run_id": run_id + "-other"}, recovered,
                        "repair-publication", expected="conflict")
            negative_cases += 1
            check(not reconciled["replayed"] and reconciled["current_files_verified"] and
                  repeated["replayed"] and repeated["current_files_verified"] and
                  finished["state"] == "succeeded" and result["raw_resources_verified"] and
                  result["source_kind"] == "test_process" and result["numerical_validation"] == "not_run" and
                  finished["artifact_receipt"]["artifact_id"] == result_id and
                  client.call("task.status", {"task_id": task["task_id"]}, recovered)["events"] == finished["events"] and
                  all((directory / name).read_bytes() == data for name, data in raw_files_before.items()) and
                  len(list((root / "runs").iterdir())) == 1 and
                  client.current()["revision"] == recovered["revision"],
                  "Reconcile re-executed, rewrote original fields, changed numerical provenance or duplicated facts")
            return {"mode": "parsed-db-rollback", "filesystem_published": True,
                    "first_task_state": "outcome_unknown", "reconciled_task_state": "succeeded",
                    "reconcile_negative_cases": negative_cases, "rerun": False}
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
                before_cancelled = owned_facts(host.workspace)
                client.call("analysis.reconcile_result", {"run_id": run_id}, client.current(),
                            "cancelled-reconcile", expected="failed")
                check(owned_facts(host.workspace) == before_cancelled and
                      client.call("task.status", {"task_id": task["task_id"]}, client.current())["state"] == "cancelled",
                      "Reconciliation revived a cancelled process task")
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
                                     "parsed-wrongphysics", "parsed-stale", "parsed-stage-failure")]
        rollback_case = publication_rollback_scenario(engine, root / "publication-rollback", transcripts)
        numerical_cases = [numerical_scenario(engine, root / mode, mode, transcripts)
                           for mode in ("numeric-match", "numeric-wrongdirection", "numeric-wrongreaction",
                                        "numeric-zero", "numeric-boundary", "numeric-over-boundary", "numeric-stale")]
    if evidence:
        evidence.mkdir(parents=True, exist_ok=True)
        (evidence / "solver-run-ipc-transcript.json").write_text(json.dumps(transcripts, indent=2) + "\n")
    return {"passed": True, "scenarios": cases, "parsed_test_process_scenarios": parsed_cases,
            "publication_rollback": rollback_case,
            "synthetic_numerical_comparisons": numerical_cases,
            "real_solver": False, "parsing_acceptance": False, "numerical_acceptance": False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True)
    parser.add_argument("--evidence-dir", type=Path)
    args = parser.parse_args()
    print(json.dumps(run(str(Path(args.engine).resolve()), args.evidence_dir), indent=2))


if __name__ == "__main__":
    main()
