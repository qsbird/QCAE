#!/usr/bin/env python3
"""SK-11/BP-10: discovered Nastran registrations used by a real SQLite host.

This exercises existing IPC operations, not a generic contribution callback API.
It does not claim solver execution, GUI interaction, or a direct core-rule negative.
"""
from __future__ import annotations

import argparse
from contextlib import closing
import hashlib
import json
from pathlib import Path
import sqlite3
import tempfile

from c3_sync_ipc_tests import EngineProcess, check, decode_packet, fetch_resource
from nastran_artifact_ipc_tests import wait_task


PHYSICAL_KINDS = ("node", "beam", "material", "section", "force", "constraint", "analysis")
# Stable public RecordTraits IDs; the projections are callable through view.render_resource.
RENDER_TYPES = {4098: "point", 4100: "line2", 4109: "geometry_line"}


def discover(capabilities):
    check(capabilities["durable"] and capabilities["storage_mode"] == "sqlite",
          "Package test requires the explicit SQLite host")
    check(capabilities["package_contributions_version"] == 1, "Package catalog version")
    contributions = capabilities["package_contributions"]
    owners = {entry["contribution_id"]: entry for entry in contributions}
    check(len(owners) == len(contributions), "Duplicate package contribution owner")
    operations = {item["name"]: item for item in capabilities["operations"]}
    check(len(operations) == len(capabilities["operations"]), "Duplicate global operation")
    registered_names = set()
    record_types = set()
    for entry in contributions:
        for category in ("core", "operations", "codecs", "validation", "ui", "render"):
            check(isinstance(entry[category], list), f"Invalid {category} catalog array")
        for item in entry["core"]:
            if item["kind"] == "record":
                check(isinstance(item["name"], str) and item["name"] and
                      type(item["version"]) is int and item["version"] > 0,
                      "Registered record definition lost its name/version")
                check(item["record_type"] not in record_types, "Duplicate record owner")
                record_types.add(item["record_type"])
            else:
                check(item["kind"] == "rule" and isinstance(item["id"], str) and item["id"],
                      "Invalid registered core rule")
        for item in entry["operations"]:
            check(item["name"] not in registered_names, "Duplicate operation owner")
            registered_names.add(item["name"])
            actual = operations[item["name"]]
            check(all(item[field] == actual[field]
                      for field in ("name", "version", "schema_id", "available")),
                  f"Package operation differs from actual host catalog: {item}")
    nastran = owners["qcae.nastran"]
    check(all(nastran[category] for category in
              ("core", "operations", "codecs", "validation", "ui", "render")),
          "Nastran did not register every required contribution category")
    check(nastran["core"] == [{"kind": "rule", "id": "qcae.nastran.controlled_subset"}],
          "Controlled-subset rule is not attributed to the selected package")
    profiles = capabilities["declared_solver_profiles"]
    check(len(profiles) == 1, "Expected one controlled declared profile")
    profile = profiles[0]["profile_ref"]
    check(nastran["codecs"] == [{"profile_ref": profile}], "Selected codec/profile mismatch")
    check(nastran["validation"] == [{"id": "qcae.nastran.export", "version": 1}],
          "Selected export validator mismatch")
    check(nastran["ui"] == [{"operation": "nastran.ui", "version": 1}], "UI registration mismatch")
    package_operations = {item["name"]: item for item in nastran["operations"]}
    for name in ("nastran.ui", "model.export", "artifact.get"):
        check(package_operations[name]["available"], f"Missing callable package operation {name}")
    check(operations["nastran.ui"]["effect"] == "read_only", "UI discovery writes the document")
    projections = {int(item["record_type"]): item["topology"] for item in nastran["render"]}
    check(len(projections) == len(nastran["render"]) and projections == RENDER_TYPES,
          "Selected render entries differ from the controlled package")
    check(set(projections) <= record_types, "Render entries reference unregistered record types")
    check("model.import" in operations["changes.preview"]["supported_commands"],
          "Selected codec is absent from the actual import entry")
    return profile


def sqlite_rows(workspace):
    """Read every logical table/column, including payloads and store generation."""
    with closing(sqlite3.connect(workspace.as_uri() + "?mode=ro", uri=True)) as database:
        database.execute("BEGIN")
        names = database.execute(
            "SELECT name FROM sqlite_master WHERE type='table' ORDER BY name").fetchall()
        result = {}
        for (name,) in names:
            quoted = '"' + name.replace('"', '""') + '"'
            cursor = database.execute(f"SELECT * FROM {quoted}")
            columns = [item[0] for item in cursor.description]
            order = ",".join(str(index + 1) for index in range(len(columns)))
            rows = database.execute(f"SELECT * FROM {quoted} ORDER BY {order}").fetchall()
            result[name] = {"columns": columns, "rows": [
                [{"blob_hex": value.hex()} if isinstance(value, bytes) else value for value in row]
                for row in rows]}
        return result


def unchanged_state(client, workspace):
    document = client.current()
    return {"document": document, "history": client.call("history.list", context=document),
            "sqlite": sqlite_rows(workspace)}


def physical_fields(client):
    """Compare all imported physical fields/references after allocating fresh IDs."""
    context = client.current()
    rows, identities = {}, {}
    for kind in PHYSICAL_KINDS:
        entities = client.call("entity.query", {"kind": kind}, context)["entities"]
        fields = [(entity["entity_id"], client.call("entity.fields", {
            "entity_id": entity["entity_id"]}, context)["fields"]) for entity in entities]
        if kind == "node":
            fields.sort(key=lambda item: item[1]["position"])
            check(len(fields) == 2, "Real cantilever node count")
        else:
            check(len(fields) == 1, f"Real cantilever {kind} count")
        rows[kind] = fields
        identities.update((identity, f"{kind}:{index}")
                          for index, (identity, _) in enumerate(fields))

    def canonical(value):
        if isinstance(value, str):
            return identities.get(value, value)
        if isinstance(value, list):
            return [canonical(item) for item in value]
        if isinstance(value, dict):
            return {key: canonical(item) for key, item in value.items()}
        return value

    return {kind: [canonical(fields) for _, fields in values] for kind, values in rows.items()}


def import_resources(client, context, profile, root_resource, resources, key):
    preview = client.call("changes.preview", {
        "command": "model.import", "root_resource": root_resource, "resources": resources,
        "source_profile_ref": profile, "unit_system": "mm-N-MPa"}, context)
    check(preview["import_report"]["complete"], "Real codec import report incomplete")
    client.call("changes.commit", {"preview_id": preview["preview_id"]}, context, key)


def run(args):
    if args.evidence_dir:
        args.evidence_dir.mkdir(parents=True, exist_ok=True)
        args.evidence_dir = Path(tempfile.mkdtemp(prefix="run-", dir=args.evidence_dir))
    observations = {"scope": "real IPC/SQLite contribution calls; no GUI or solver execution"}
    with tempfile.TemporaryDirectory(prefix="qcae-package-", dir=Path("/tmp").resolve()) as folder:
        root = Path(folder)
        workspace = root / "workspace.sqlite"
        with (root / "engine.log").open("w+") as log:
            host = EngineProcess(args.engine, str(root / "engine.sock"), str(workspace), log)
            try:
                capabilities = host.start()
                profile = discover(capabilities)
                observations["capabilities"] = capabilities
                client = host.client
                document = client.call("project.create", {"name": "Package contribution calls"}, key="create")
                before_ui = unchanged_state(client, workspace)
                ui = client.call("nastran.ui", extra={"requested_version": 1})
                check(ui["operation"] == "model.export" and ui["profile"] == profile and
                      ui["units"] == "mm-N-MPa", "Actual UI contribution targets a different codec/export")
                after_ui = unchanged_state(client, workspace)
                observations["ui"] = {"response": ui, "before": before_ui, "after": after_ui}
                check(before_ui == after_ui, "Read-only UI call changed persistent state/history/context")
                fixtures = Path(__file__).parent / "fixtures" / "nastran"
                paths = ("cantilever.bdf", "mesh/nodes.bdf", "mesh/beams.bdf", "properties.bdf")
                resources = [{"path": path, "text": (fixtures / path).read_text()} for path in paths]
                import_resources(client, document, profile, "cantilever.bdf", resources, "import")
                line = client.call("geometry.create_line", {
                    "start_mm": [0, 10, 0], "end_mm": [100, 10, 0]}, client.current(), "common-line")
                context = client.current()
                physics = physical_fields(client)
                check([node["position"] for node in physics["node"]] == [[0, 0, 0], [100, 0, 0]],
                      "Real BDF import node positions")
                check(physics["beam"][0]["nodes"] == ["node:0", "node:1"] and
                      physics["beam"][0]["section"] == "section:0" and
                      physics["section"][0]["material"] == "material:0",
                      "Real BDF import physical references")
                check(physics["material"][0]["young_modulus_mpa"] == 210000 and
                      physics["force"][0]["force_n"] == [0, -100, 0], "Real BDF import physics")
                view = client.call("view.create", {
                    "hidden_ids": [], "camera_fingerprint": "package-contributions"}, context)
                rendered = client.call("view.render_resource", {
                    "view_session_id": view["view_session_id"],
                    "expected_view_revision": view["view_revision"]}, context,
                    extra={"requested_version": 1})
                packet = decode_packet(fetch_resource(client, rendered["manifest"])[0])
                nodes = client.call("entity.query", {"kind": "node"}, context)["entities"]
                beams = client.call("entity.query", {"kind": "beam"}, context)["entities"]
                check({point["entity_id"] for point in packet["points"]} ==
                      {node["entity_id"] for node in nodes} and len(packet["points"]) == 2,
                      "Discovered point projection did not render the imported nodes")
                check({point["entity_id"]: point["position_mm"] for point in packet["points"]} ==
                      {node["entity_id"]: client.call("entity.fields", {
                          "entity_id": node["entity_id"]}, context)["fields"]["position"] for node in nodes},
                      "Discovered point projection changed physical coordinates")
                check(len(packet["beams"]) == 1 and packet["beams"][0]["entity_id"] == beams[0]["entity_id"] and
                      [packet["points"][index]["entity_id"] for index in packet["beams"][0]["points"]] ==
                      client.call("entity.fields", {"entity_id": beams[0]["entity_id"]}, context)["fields"]["nodes"],
                      "Discovered line2 projection changed beam connectivity")
                check(len(packet["geometry_lines"]) == 1 and packet["geometry_lines"][0]["entity_id"] == line["entity_id"] and
                      packet["geometry_lines"][0]["start_mm"] == [0, 10, 0] and
                      packet["geometry_lines"][0]["end_mm"] == [100, 10, 0],
                      "Discovered geometry projection did not render the common-service line")
                check(packet["document_id"] == context["document_id"] and
                      packet["document_epoch"] == context["document_epoch"] and
                      packet["revision"] == int(context["revision"]), "Rendered resource has stale authority")
                observations["render"] = {"manifest": rendered["manifest"], "packet": packet}
                before_rejection = unchanged_state(client, workspace)
                rejected = client.call("model.export", {
                    "analysis_id": nodes[0]["entity_id"], "output_directory": str(root / "invalid-export")},
                    context, "invalid-analysis", extra={"requested_version": 1, "expected_profile": profile},
                    expected="failed")
                check(rejected["error"]["code"] == "INVALID_INPUT" and
                      rejected["error"]["message"].startswith("Controlled Nastran export validation failed") and
                      "analysis_missing" in rejected["error"]["message"],
                      "Request did not reach the selected export validator")
                after_rejection = unchanged_state(client, workspace)
                observations["validation"] = {"response": rejected, "before": before_rejection, "after": after_rejection}
                check(before_rejection == after_rejection and not (root / "invalid-export").exists(),
                      "Rejected export validator published rows, tasks, model changes, history or files")
                analysis = client.call("entity.query", {"kind": "analysis"}, context)["entities"][0]["entity_id"]
                output = root / "published"
                task = wait_task(client, client.call("model.export", {
                    "analysis_id": analysis, "output_directory": str(output)}, context, "export",
                    extra={"requested_version": 1, "expected_profile": profile}), context)
                check(task["state"] == "succeeded", f"Actual export failed: {task}")
                check(client.current()["revision"] == context["revision"], "Export edited the model revision")
                artifact_id = "artifact-" + task["task_id"]
                artifact = client.call("artifact.get", {"artifact_id": artifact_id}, context,
                                       extra={"requested_version": 1})
                check(artifact["verified"] and artifact["state"] == "published", "Actual artifact is not verified")
                manifest_bytes = (output / "manifest.json").read_bytes()
                manifest = json.loads(manifest_bytes)
                check(manifest["complete"] and manifest["artifact_id"] == artifact_id and
                      manifest["task_id"] == task["task_id"] and manifest["analysis_id"] == analysis and
                      manifest["document_id"] == context["document_id"] and
                      manifest["document_epoch"] == context["document_epoch"] and
                      manifest["revision"] == context["revision"] and manifest["target_profile"] == profile,
                      "Actual publication provenance differs from the selected call")
                check(hashlib.sha256(manifest_bytes).hexdigest() == task["artifact_receipt"]["manifest_sha256"] and
                      manifest["input_sha256"] == hashlib.sha256(bytes.fromhex(manifest["physical_signature_hex"])).hexdigest(),
                      "Actual publication/input digest mismatch")
                published_resources = []
                for item in manifest["files"]:
                    path = Path(item["path"])
                    check(not path.is_absolute() and ".." not in path.parts, "Unsafe published resource path")
                    raw = (output / path).read_bytes()
                    check(len(raw) == int(item["byte_length"]) and hashlib.sha256(raw).hexdigest() == item["sha256"],
                          "Actual resource bytes differ from the published manifest")
                    published_resources.append({"path": item["path"], "text": raw.decode("utf-8")})
                observations["export"] = {"task": task, "artifact": artifact, "manifest": manifest,
                                          "resources": published_resources, "physical_fields": physics}
                # model.import requires an empty model; normal sequential lifecycle keeps one authority.
                client.call("project.close", {"policy": "discard"}, context, "close-exported")
                reopened = client.call("project.create", {"name": "Actual BDF semantic readback"}, key="readback-create")
                import_resources(client, reopened, profile, manifest["root_resource"], published_resources, "readback-import")
                readback = physical_fields(client)
                observations["semantic_readback"] = readback
                check(readback == physics, "Actual published BDF reimport changed physical fields/references")
                check(client.call("capabilities.list")["package_contributions"] == capabilities["package_contributions"],
                      "Document lifecycle changed the selected startup registrations")
                observations["completed"] = True
                print("PASS: six-category package discovery joined to real UI/import/validation/render/export/readback calls")
            finally:
                process = host.process
                host.close()
                observations["engine_returncode"] = process.returncode if process else None
                if args.evidence_dir:
                    log.flush()
                    log.seek(0)
                    (args.evidence_dir / "engine.log").write_text(log.read())
                    (args.evidence_dir / "transcript.json").write_text(json.dumps(host.client.transcript, indent=2) + "\n")
                    (args.evidence_dir / "observations.json").write_text(json.dumps(observations, indent=2) + "\n")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--engine", required=True)
    parser.add_argument("--evidence-dir", type=Path)
    run(parser.parse_args())


if __name__ == "__main__":
    main()
