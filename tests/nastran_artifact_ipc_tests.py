#!/usr/bin/env python3
"""Real SQLite host publication, provenance, idempotency and recovery checks."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sqlite3
import struct
import tempfile
import time

from c3_sync_ipc_tests import EngineProcess, check, decode_packet, fetch_resource


def wait_task(client, task, context):
    deadline = time.monotonic() + 30
    while task["state"] in {"queued", "running", "cancel_requested", "committing"}:
        check(time.monotonic() < deadline, "Export task timed out")
        time.sleep(.01)
        task = client.call("task.status", {"task_id": task["task_id"]}, context)
    return task


class RecordReader:
    """Locate fields in a real saved record without re-encoding the project."""

    def __init__(self, data, base=0):
        self.data, self.base, self.offset = data, base, 0

    def u64(self):
        check(self.offset + 8 <= len(self.data), "Truncated persisted integer")
        value = struct.unpack_from("<Q", self.data, self.offset)[0]
        self.offset += 8
        return value

    def text_span(self):
        size = self.u64()
        start = self.offset
        check(start + size <= len(self.data), "Truncated persisted string")
        self.offset += size
        return self.base + start, self.base + self.offset

    def text(self):
        start, end = self.text_span()
        return self.data[start - self.base:end - self.base]


def task_field_spans(project, task_id):
    reader = RecordReader(project)
    check(reader.text() == b"QCAE-RECORD-PROJECT" and reader.u64() == 2,
          "Expected a real current saved project")
    for _ in range(3):  # Project ID, name and content state.
        reader.text()
    for _ in range(reader.u64()):
        reader.text()  # Physical entity record.
    task_span = None
    for _ in range(reader.u64()):
        space, identity = reader.u64(), reader.text()
        start, end = reader.text_span()
        if space == 6 and identity == task_id.encode():
            envelope = RecordReader(project[start:end], start)
            check(envelope.text() == b"QCAE-OWNED-ROW" and envelope.u64() == 1,
                  "Expected an actual owned-row envelope")
            check(envelope.text() == b"qcae.runtime.task" and envelope.u64() == 1,
                  "Expected an actual runtime task owner")
            task_span = envelope.text_span()
            check(envelope.offset == end - start, "Trailing owned-row bytes")
    check(reader.offset == len(project) and task_span is not None, "Saved export task missing")
    start, end = task_span
    reader = RecordReader(project[start:end], start)
    check(reader.text() == b"QCAE-TASK" and reader.u64() == 3,
          "Expected an artifact-receipt task")
    spans = {}
    for name in ("task_id", "principal", "key", "operation", "signature", "document_id", "epoch"):
        spans[name] = reader.text_span()
    spans["revision"] = (start + reader.offset, start + reader.offset + 8)
    reader.u64()
    for name in ("profile_id", "profile_version", "profile_digest"):
        spans[name] = reader.text_span()
    check(reader.u64() == 4, "Saved export task is not succeeded")
    reader.u64()  # Progress double bits.
    for _ in range(reader.u64()):
        for _ in range(3):
            reader.u64()  # Sequence, state and progress bits.
    check(reader.u64() == 0 and reader.u64() == 0 and reader.u64() == 1,
          "Expected only an artifact receipt, with no failure diagnostic")
    spans["artifact_id"] = reader.text_span()
    spans["receipt_revision"] = (start + reader.offset, start + reader.offset + 8)
    reader.u64()
    spans["manifest_sha256"] = reader.text_span()
    check(reader.offset == end - start, "Trailing task bytes")
    return spans


def owned_facts(workspace):
    with sqlite3.connect(workspace) as database:
        return database.execute(
            "SELECT space,identity,value FROM store_rows WHERE space IN (6,7) ORDER BY space,identity"
        ).fetchall()


def frozen_revision_span(project):
    marker = b"QCAE-FROZEN-STATIC-1"
    check(project.count(marker) == 1, "Expected one actual frozen artifact input")
    offset = project.index(marker) + len(marker)
    # Frozen input uses record_wire's four-byte length-prefixed fields.
    for _ in range(3):  # Original document ID, epoch and decimal revision.
        check(offset + 4 <= len(project), "Truncated frozen input")
        length = struct.unpack_from("<I", project, offset)[0]
        start, offset = offset + 4, offset + 4 + length
        check(offset <= len(project), "Truncated frozen input field")
    return start, offset


def carried_task_corruption(engine, root, source, task_id, artifact_id, fixture):
    """A carried project may contain structurally valid but inconsistent facts."""
    with sqlite3.connect(source) as database:
        original = database.execute("SELECT payload FROM project WHERE id=1").fetchone()[0]
    spans = task_field_spans(original, task_id)
    cases = ("principal", "operation", "document_id", "epoch", "revision",
             "profile_id", "profile_version", "profile_digest", "artifact_id", "manifest_sha256",
             "frozen_and_task_revision")
    with (root / "carried-engine.log").open("w+") as log:
        workspace = root / "carried.sqlite"
        host = EngineProcess(engine, str(root / "carried.sock"), str(workspace), log)
        try:
            host.start()
            for case in ("unchanged", *cases):
                destination = root / f"carried-{case}.qcae"
                with sqlite3.connect(source) as source_db, sqlite3.connect(destination) as target:
                    source_db.backup(target)
                    damaged = bytearray(original)
                    if case in {"revision", "frozen_and_task_revision"}:
                        revision = struct.unpack_from("<Q", original, spans["revision"][0])[0] + 1
                        # Keep the task's own structural receipt check valid. The
                        # mismatch is between the task and the frozen artifact.
                        for name in ("revision", "receipt_revision"):
                            struct.pack_into("<Q", damaged, spans[name][0], revision)
                        if case == "frozen_and_task_revision":
                            start, end = frozen_revision_span(original)
                            replacement = str(revision).encode()
                            check(len(replacement) == end - start, "Fixture revision changed field width")
                            damaged[start:end] = replacement
                    elif case != "unchanged":
                        at = spans[case][1] - 1
                        damaged[at] = ord("0") if damaged[at] != ord("0") else ord("1")
                    target.execute("UPDATE project SET payload=? WHERE id=1", (bytes(damaged),))
                context = host.client.call("project.open", {"mode": "normal", "path": str(destination)},
                                           key=f"open-{case}")
                if case == "unchanged":
                    check(host.client.call("artifact.get", {"artifact_id": artifact_id}, context)["verified"],
                          "Normal carried artifact compared against the new active epoch")
                    result = host.client.call("results.read_fixture", {
                        "artifact_id": artifact_id, "fixture_json": json.dumps(fixture)}, context, "carried-fixture")
                    check(result["source_kind"] == "fixture", "Carried fixture was labeled solver output")
                    host.client.call("artifact.reconcile", {"artifact_id": artifact_id}, context, "carried-reconcile")
                else:
                    before = owned_facts(workspace)
                    host.client.call("artifact.get", {"artifact_id": artifact_id}, context, expected="failed")
                    host.client.call("results.read_fixture", {
                        "artifact_id": artifact_id, "fixture_json": json.dumps(fixture)}, context,
                        f"fixture-{case}", expected="failed")
                    for attempt in range(2):
                        host.client.call("artifact.reconcile", {"artifact_id": artifact_id}, context,
                                         f"reconcile-{case}-{attempt}", expected="failed")
                    check(owned_facts(workspace) == before,
                          f"Rejected {case} mismatch partially published facts or task events")
                    check(host.client.current()["revision"] == context["revision"],
                          f"Rejected {case} mismatch edited the model")
                host.client.call("project.close", {"policy": "discard"}, context, f"close-{case}")
            return len(cases)
        finally:
            host.close()


def run(engine, evidence):
    with tempfile.TemporaryDirectory(prefix="qcae-nastran-artifact-", dir=Path("/tmp").resolve()) as folder:
        root = Path(folder)
        with (root / "engine.log").open("w+") as log:
            host = EngineProcess(engine, str(root / "engine.sock"), str(root / "workspace.sqlite"), log)
            try:
                capabilities = host.start()
                client = host.client
                profile = capabilities["declared_solver_profiles"][0]["profile_ref"]
                operations = {item["name"]: item for item in capabilities["operations"]}
                check(sum(item["name"] == "model.export" for item in capabilities["operations"]) == 1,
                      "Capability catalog contains duplicate model.export entries")
                for name in ("model.export", "artifact.get", "artifact.reconcile", "nastran.ui", "project.migrate_profile"):
                    check(operations[name]["available"], f"Missing production operation {name}")
                check(operations["project.migrate_profile"]["effect"] == "document_write", "Migration effect")
                ui = client.call("nastran.ui")
                check(ui["operation"] == "model.export" and ui["profile"] == profile and ui["units"] == "mm-N-MPa",
                      "UI contribution does not call installed export contract")
                document = client.call("project.create", {"name": "Nastran artifact"}, key="create")
                fixtures = Path(__file__).parent / "fixtures" / "nastran"
                paths = ("cantilever.bdf", "mesh/nodes.bdf", "mesh/beams.bdf", "properties.bdf")
                resources = [{"path": path, "text": (fixtures / path).read_text()} for path in paths]
                preview = client.call("changes.preview", {"command": "model.import", "root_resource": "cantilever.bdf",
                    "resources": resources, "source_profile_ref": profile, "unit_system": "mm-N-MPa"}, document)
                client.call("changes.commit", {"preview_id": preview["preview_id"]}, document, "import")
                original = client.current()
                analysis = client.call("entity.query", {"kind": "analysis"}, original)["entities"][0]["entity_id"]
                view = client.call("view.create", {"hidden_ids": [], "camera_fingerprint": "package-test"}, original)
                render = client.call("view.render_resource", {"view_session_id": view["view_session_id"],
                    "expected_view_revision": view["view_revision"]}, original, extra={"requested_version": 1})
                packet = decode_packet(fetch_resource(client, render["manifest"])[0])
                check(len(packet["points"]) == 2 and len(packet["beams"]) == 1, "Package render contribution not used")
                bad_profile = dict(profile, definition_digest=profile["definition_digest"] + "-bad")
                client.call("model.export", {"analysis_id": analysis, "output_directory": str(root / "wrong-profile")},
                    original, "wrong-profile", extra={"expected_profile": bad_profile}, expected="failed")
                check(not (root / "wrong-profile").exists(), "Invalid profile created files")
                output = root / "published"
                parameters = {"analysis_id": analysis, "output_directory": str(output)}
                task = client.call("model.export", parameters, original, "export", extra={"expected_profile": profile})
                task = wait_task(client, task, original)
                check(task["state"] == "succeeded", task)
                check(client.current()["revision"] == original["revision"], "Artifact publication edited model revision")
                artifact_id = "artifact-" + task["task_id"]
                artifact = client.call("artifact.get", {"artifact_id": artifact_id}, original)
                check(artifact["verified"] and artifact["state"] == "published", artifact)
                manifest = json.loads((output / "manifest.json").read_text())
                check(manifest["complete"] and manifest["artifact_id"] == artifact_id and manifest["task_id"] == task["task_id"], manifest)
                check(manifest["analysis_id"] == analysis and manifest["document_id"] == original["document_id"] and
                      manifest["document_epoch"] == original["document_epoch"] and manifest["revision"] == original["revision"], manifest)
                check(manifest["target_profile"] == profile and len(manifest["export_id_map"]) == 7, manifest)
                check(manifest["input_sha256"] == hashlib.sha256(bytes.fromhex(manifest["physical_signature_hex"])).hexdigest(),
                      "Input SHA256 does not cover the exact physical signature")
                for item in manifest["files"]:
                    data = (output / item["path"]).read_bytes()
                    check(len(data) == int(item["byte_length"]) and hashlib.sha256(data).hexdigest() == item["sha256"], item)
                replayed = client.call("model.export", parameters, original, "export", extra={"expected_profile": profile})
                check(replayed["task_id"] == task["task_id"] and replayed["events"] == task["events"], "Export replay reran work")
                client.call("model.export", dict(parameters, output_directory=str(root / "other")), original,
                    "export", extra={"expected_profile": profile}, expected="conflict")
                reconciled = client.call("artifact.reconcile", {"artifact_id": artifact_id}, original, "reconcile")
                check(not reconciled["replayed"] and reconciled["current_files_verified"], reconciled)
                repeated = client.call("artifact.reconcile", {"artifact_id": artifact_id}, original, "reconcile")
                check(repeated["replayed"] and repeated["current_files_verified"], repeated)
                unchanged_task = client.call("task.status", {"task_id": task["task_id"]}, original)
                check(unchanged_task["events"] == task["events"], "Reconcile appended a duplicate success event")
                client.call("artifact.reconcile", {"artifact_id": artifact_id + "-other"}, original,
                    "reconcile", expected="conflict")
                old = dict(original, revision=str(int(original["revision"]) - 1))
                client.call("artifact.reconcile", {"artifact_id": artifact_id}, old, "stale-reconcile", expected="conflict")
                protected = output / manifest["files"][0]["path"]
                contents = protected.read_bytes()
                protected.write_bytes(b"x" * len(contents))
                historical = client.call("artifact.reconcile", {"artifact_id": artifact_id}, original, "reconcile")
                check(historical["replayed"] and historical["verified_at_reconcile"] and not historical["current_files_verified"],
                      "Replay disguised corrupted current files as verified")
                client.call("artifact.get", {"artifact_id": artifact_id}, original, expected="failed")
                client.call("artifact.reconcile", {"artifact_id": artifact_id}, original, "corrupt-new-key", expected="failed")
                protected.write_bytes(contents)
                host.process.kill()
                host.process.wait(timeout=5)
                host.process = None
                host.client.instance_id = None
                check(host.start()["recovery_available"], "Restart omitted explicit recovery")
                recovered = client.call("project.open", {"mode": "recover"}, key="recover")
                check(client.call("artifact.get", {"artifact_id": artifact_id}, recovered)["verified"], "Recovery lost artifact fact")
                check(client.call("task.status", {"task_id": task["task_id"]}, recovered)["state"] == "succeeded", "Recovery reran completed export")
                carried = root / "carried.qcae"
                client.call("project.save", {"path": str(carried)}, recovered, "save-carried")
                fixture = {"input_fingerprint": manifest["physical_signature_hex"],
                           "identities": manifest["export_id_map"], "quantity": "displacement", "unit": "mm",
                           "components": ["X", "Y", "Z"], "location": "node", "coordinate_basis": "global",
                           "case": manifest["case_label"], "frame": 0, "source_kind": "fixture",
                           "values": [{"solver_number": item["number"], "value": [0, 0, 0]}
                                      for item in manifest["export_id_map"] if item["namespace"] == "GRID"]}
                negative_cases = carried_task_corruption(engine, root, carried, task["task_id"], artifact_id, fixture)
                return {"passed": True, "files": len(manifest["files"]), "export_ids": len(manifest["export_id_map"]),
                        "published": True,
                        "reconcile_idempotency": True, "recovery": True,
                        "cross_row_negative_cases": negative_cases, "real_solver": False}
            finally:
                host.close()
                if evidence:
                    evidence.mkdir(parents=True, exist_ok=True)
                    (evidence / "nastran-artifact-ipc-transcript.json").write_text(json.dumps(host.client.transcript, indent=2) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True)
    parser.add_argument("--evidence-dir", type=Path)
    args = parser.parse_args()
    print(json.dumps(run(str(Path(args.engine).resolve()), args.evidence_dir), sort_keys=True))


if __name__ == "__main__":
    main()
