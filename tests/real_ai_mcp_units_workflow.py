#!/usr/bin/env python3
"""Real two-turn AI/MCP dimension/section/material/load unit diagnostic."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import tempfile
import time

import real_ai_mcp_clarification as transport
from real_ai_mcp_session import cli_call, digest

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ["capabilities.list", "project.current", "geometry.create_line", "mesh.generate_line",
         "task.status", "material.create", "section.create", "beam.assign_section",
         "entity.query", "entity.fields", "force.create", "constraint.create", "load_case.create",
         "analysis.create", "analysis.check", "analysis.get_issues", "model.summary",
         "model.export", "artifact.get"]


def verify_physics(rows):
    groups = {kind: [row for row in rows if row["kind"] == kind]
              for kind in ["node", "beam", "geometry", "material", "section", "force", "constraint",
                           "load_case", "analysis"]}
    nodes = {row["entity_id"]: row["position_mm"] for row in groups["node"]}
    assert len(nodes) == 21 and sorted(nodes.values()) == [[index * 50, 0, 0] for index in range(21)]
    assert all(len(groups[kind]) == 1 for kind in groups if kind not in {"node", "beam"})
    section, material, load, support = [groups[kind][0] for kind in ["section", "material", "force", "constraint"]]
    assert material["young_modulus_mpa"] == 210000 and material["poisson_ratio"] == .3
    assert section["material_id"] == material["entity_id"] and section["area_mm2"] == 100
    assert section["i1_mm4"] == section["i2_mm4"] == 833.333 and section["torsion_mm4"] == 1400
    assert len(groups["beam"]) == 20 and all(row["section_id"] == section["entity_id"] for row in groups["beam"])
    assert sorted((nodes[row["nodes"][0]][0], nodes[row["nodes"][1]][0]) for row in groups["beam"]) == [
        (index * 50, (index + 1) * 50) for index in range(20)]
    assert load["force_n"] == [0, -1, 0] and nodes[load["node_id"]] == [1000, 0, 0]
    assert support["dofs"] == "123456" and len(support["nodes"]) == 1 and nodes[support["nodes"][0]] == [0, 0, 0]
    case = groups["load_case"][0]
    assert case["forces"] == [load["entity_id"]] and case["constraints"] == [support["entity_id"]]
    assert groups["analysis"][0]["load_cases"] == [case["entity_id"]]
    line = groups["geometry"][0]
    assert line["start_mm"] == [0, 0, 0] and line["end_mm"] == [1000, 0, 0]
    return {"all_quantities_and_references_verified": True, "node_count": 21, "beam_count": 20}


def run(args):
    output = Path(args.evidence_dir).resolve()
    output.mkdir(parents=True, exist_ok=False)
    facts = {"schema": "qcae.real-ai.units-workflow.v1", "formal_acceptance": False,
             "real_solver_run": False, "model": args.model, "reasoning_effort": args.effort,
             "engine_sha256": digest(args.engine), "cli_sha256": digest(args.cli),
             "harness_sha256": digest(__file__), "transport_sha256": digest(transport.__file__),
             "bridge_sha256": digest(ROOT / "apps/mcp/bridge.py"),
             "source_binding": "mutable-tree diagnostic; engine binary frozen before this run",
             "limitations": ["actual GUI display unit validation and three independent sessions pending",
                              "export is a real file artifact; no solver or numerical acceptance"]}
    facts["source_head"] = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    started = time.monotonic()
    transport.TOOLS = TOOLS
    with tempfile.TemporaryDirectory(prefix="qcae-ai-units-", dir=Path("/tmp").resolve()) as folder:
        root = Path(folder)
        endpoint = root / "engine.sock"
        server = None
        with (output / "engine.log").open("wb") as log:
            engine = subprocess.Popen([args.engine, "--socket", str(endpoint),
                                       "--workspace", str(root / "work.sqlite")], stdout=log, stderr=log)
            try:
                deadline = time.monotonic() + 15
                while not endpoint.exists() and engine.poll() is None and time.monotonic() < deadline:
                    time.sleep(.025)
                assert endpoint.exists(), "engine did not start"
                request = {"api_version": "1.1", "request_id": "units-project", "operation": "project.create",
                           "parameters": {"name": "Synthetic all-units clarification"},
                           "idempotency_key": "units-project"}
                created = subprocess.run([args.cli, "--socket", str(endpoint), "--no-start"],
                                         input=json.dumps(request), capture_output=True, text=True, timeout=15)
                assert created.returncode == 0, created.stdout
                initial = cli_call(args.cli, endpoint, "project.current")["data"]
                catalog = cli_call(args.cli, endpoint, "capabilities.list")["data"]["operations"]
                available = {entry["name"] for entry in catalog if entry["available"]}
                assert set(TOOLS) <= available, {"unavailable_requested_tools": sorted(set(TOOLS) - available)}
                facts["client_version"] = subprocess.check_output([args.client, "--version"], text=True).strip()
                server = transport.AppServer(args.client, endpoint, root, output, args.model, args.effort)
                facts["thread_id"] = server.thread
                first_prompt = """请在现有项目建立直悬臂梁：长度1000，20个均匀梁单元；材料Steel，E=210，泊松比0.3；截面面积100，I1=I2=833.333，扭转常数1400；梁沿全局+X，起点完全固定，终点受全局-Y方向大小1的力。所有有量纲数值均未给单位。请先检查必要信息，不能默认采用工具schema的规范单位；单位不明确时列出缺项并停止，不能提交任何物理对象。最终JSON为status和missing数组。"""
                first_schema = {"type": "object", "additionalProperties": False,
                                "properties": {"status": {"type": "string", "enum": ["needs_input", "success"]},
                                               "missing": {"type": "array", "items": {"type": "string"}}},
                                "required": ["status", "missing"]}
                (output / "prompt-1.txt").write_text(first_prompt)
                first = server.turn(first_prompt, args.effort, first_schema)
                facts["missing_units_turn"] = first
                current = cli_call(args.cli, endpoint, "project.current")["data"]
                summary = cli_call(args.cli, endpoint, "model.summary", context=current)
                facts["verified_before_units"] = {"context": current, "summary": summary}
                assert current["revision"] == initial["revision"] and not summary["data"]["materials"], "write before units"
                assert all(summary["data"][name] == 0 for name in ["node_count", "beam_count", "section_count", "geometry_count"])
                assert first["final"]["status"] == "needs_input" and first["final"]["missing"], "missing clarification"
                assert not any(call["tool"] in {"geometry.create_line", "mesh.generate_line", "material.create",
                                               "section.create", "force.create"} for call in first["calls"]), "attempted premature write"
                second_prompt = """补齐单位与身份：长度1000 mm，E=210 GPa，面积100 mm2，I1=I2=833.333 mm4，J=1400 mm4，力1 N；泊松比0.3无量纲。请用现有QCAE MCP完成这一模型：线从(0,0,0)mm到(1000,0,0)mm，均匀20段；mesh后台任务完成后读取实际Node/Beam身份，给20梁分配同一截面；起点节点全6DOF固定，末端节点施加(0,-1,0)N；建立一个LC1和一个受控Nastran linear_static分析。读取实际上下文和profile，所有写入采用当前版本及唯一幂等键；不猜ID。最后运行场景检查、读取实体字段并使用typed model.export核验规范mm/N/MPa（输出目录OUTPUT_DIRECTORY，等待task.status成功，再artifact.get确认verified）。不得执行求解器或读取文件。只输出JSON：status、length_mm、normalized_modulus_mpa、force_y_n、node_count、beam_count、section_area_mm2、analysis_id、check_id、export_complete、artifact_id。"""
                second_prompt = second_prompt.replace("OUTPUT_DIRECTORY", str(root / "published-export"))
                names = {"status": {"type": "string", "enum": ["needs_input", "success"]},
                         "length_mm": {"type": "number"}, "normalized_modulus_mpa": {"type": "number"},
                         "force_y_n": {"type": "number"}, "node_count": {"type": "integer"},
                         "beam_count": {"type": "integer"}, "section_area_mm2": {"type": "number"},
                         "analysis_id": {"type": "string"}, "check_id": {"type": "string"},
                         "export_complete": {"type": "boolean"}, "artifact_id": {"type": "string"}}
                second_schema = {"type": "object", "additionalProperties": False,
                                 "properties": names, "required": list(names)}
                (output / "prompt-2.txt").write_text(second_prompt)
                facts["output_schemas"] = [first_schema, second_schema]
                second = server.turn(second_prompt, args.effort, second_schema)
                facts["units_supplied_turn"] = second
                facts["actual_completed_mcp_calls"] = len(first["calls"]) + len(second["calls"])
                current = cli_call(args.cli, endpoint, "project.current")["data"]
                summary = cli_call(args.cli, endpoint, "model.summary", context=current)["data"]
                all_rows = cli_call(args.cli, endpoint, "entity.query", context=current)["data"]["entities"]
                caps = cli_call(args.cli, endpoint, "capabilities.list")["data"]
                profile = caps["declared_solver_profiles"][0]["profile_ref"]
                exported = cli_call(args.cli, endpoint, "artifact.get",
                                    {"artifact_id": second["final"]["artifact_id"]}, current)["data"]
                independent_physics = verify_physics(all_rows)
                receipt = cli_call(args.cli, endpoint, "task.status",
                                   {"task_id": exported["task_id"]}, current)["data"]
                facts["independent_model_verification"] = {"context": current, "summary": summary,
                                                             "entities": all_rows, "physics": independent_physics,
                                                             "export_artifact": exported, "task_receipt": receipt}
                manifest_path = Path(exported["manifest_path"])
                manifest = json.loads(manifest_path.read_text())
                assert exported["verified"] and exported["state"] == "published" and manifest["complete"]
                assert manifest["target_profile"] == profile and manifest["unit_system"] == "mm-N-MPa"
                assert manifest["analysis_id"] == second["final"]["analysis_id"]
                artifact_receipt = receipt["artifact_receipt"]
                assert receipt["state"] == "succeeded" and artifact_receipt["artifact_id"] == exported["artifact_id"]
                assert artifact_receipt["input_revision"] == exported["input_revision"] == current["revision"]
                assert digest(manifest_path) == artifact_receipt["manifest_sha256"]
                (output / "artifact-manifest.json").write_text(json.dumps(manifest, indent=2)+"\n")
                for resource in manifest["files"]:
                    relative = resource["path"]
                    target = output / "published-export" / relative
                    target.parent.mkdir(parents=True, exist_ok=True)
                    original = manifest_path.parent / relative
                    assert original.stat().st_size == int(resource["byte_length"]) and digest(original) == resource["sha256"]
                    target.write_bytes(original.read_bytes())
                issues = cli_call(args.cli, endpoint, "analysis.get_issues",
                                  {"check_id": second["final"]["check_id"]}, current)["data"]
                facts["independently_verified"] = {"context": current, "summary": summary,
                                                    "entities": all_rows, "export_artifact": exported, "issues": issues,
                                                    "physics": independent_physics}
                assert summary["node_count"] == 21 and summary["beam_count"] == 20 and summary["section_count"] == 1
                assert len(summary["materials"]) == 1 and summary["materials"][0]["young_modulus_mpa"] == 210000
                assert exported["verified"] and exported["state"] == "published" and manifest["complete"] and not issues["issues"], "incomplete scenario/export"
                final = second["final"]
                assert final["status"] == "success" and final["length_mm"] == 1000 and final["force_y_n"] == -1
                assert final["normalized_modulus_mpa"] == 210000 and final["section_area_mm2"] == 100
                assert final["node_count"] == 21 and final["beam_count"] == 20 and final["export_complete"]
                assert all(transport.engine_fact(call)["status"] == "success" for call in second["calls"]), "tool failure"
                writes = {call["tool"] for call in second["calls"]}
                assert {"material.create", "geometry.create_line", "mesh.generate_line", "section.create",
                        "beam.assign_section", "force.create", "constraint.create", "analysis.create"} <= writes
                if args.snapshot_path:
                    snapshot = Path(args.snapshot_path).resolve()
                    assert snapshot.suffix == ".qcae" and not snapshot.exists(), "snapshot must be a fresh .qcae path"
                    snapshot.parent.mkdir(parents=True, exist_ok=True)
                    request = {"api_version": "1.1", "request_id": "independent-save-ai-model",
                               "operation": "project.save_as", "parameters": {"path": str(snapshot)},
                               "document_id": current["document_id"], "document_epoch": current["document_epoch"],
                               "expected_revision": current["revision"], "idempotency_key": "independent-save-ai-model"}
                    saved = subprocess.run([args.cli, "--socket", str(endpoint), "--no-start"],
                                           input=json.dumps(request), capture_output=True, text=True, timeout=15)
                    response = json.loads(saved.stdout)
                    facts["independent_saved_snapshot"] = {"request": request, "response": response}
                    assert saved.returncode == 0 and response["status"] == "success" and snapshot.is_file(), "snapshot save failed"
                    facts["independent_saved_snapshot"].update(path=str(snapshot), sha256=digest(snapshot),
                                                               byte_length=snapshot.stat().st_size)
                facts.update(passed=True, actual_completed_mcp_calls=len(first["calls"]) + len(second["calls"]))
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
    parser.add_argument("--snapshot-path", help="Optional local .qcae snapshot for a later actual desktop verification")
    arguments = parser.parse_args()
    for name in ("client", "engine", "cli"):
        setattr(arguments, name, str(Path(getattr(arguments, name)).resolve(strict=True)))
    run(arguments)
