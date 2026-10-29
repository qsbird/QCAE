#!/usr/bin/env python3
"""Flat-plate workflow through the engine, with the Gmsh mesher.

A 200 x 100 mm steel plate, 2 mm thick, is pulled by 10 kN along its right edge and supported on
the left edge (x) and the bottom edge (y). This test builds that model operation by operation:
geometry, Gmsh mesh, shell section, load case with an edge force and edge constraints. It checks
what each step stored. Later stages extend it to export, solve and compare with the exact answer.
"""
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import subprocess
import tempfile
import time

from analysis_check_ipc_tests import Client

WIDTH, HEIGHT, THICKNESS, FORCE = 200.0, 100.0, 2.0, 10000.0
ELEMENT_SIZE = 10.0
# Outer loop edges: 0 bottom (y = 0), 1 right (x = 200), 2 top (y = 100), 3 left (x = 0).
BOTTOM, RIGHT, LEFT = 0, 1, 3


def quantity(value, unit):
    return {"value": value, "unit": unit}


def finish(client, task):
    deadline = time.monotonic() + 30
    while task["state"] not in {"succeeded", "failed", "cancelled", "conflicted"}:
        assert time.monotonic() < deadline, task
        time.sleep(.02)
        task = client.call("task.status", {"task_id": task["task_id"]}, client.current())
    assert task["state"] == "succeeded", task
    return task


def all_entities(client, kind):
    result, offset = [], 0
    while True:
        page = client.call("entity.query", {"kind": kind, "offset": offset, "limit": 1000}, client.current())
        result += [item["entity_id"] for item in page["entities"]]
        offset += len(page["entities"])
        if offset >= page["total"] or not page["entities"]:
            return result


def build_plate(client):
    """Creates the tension-plate model and returns the identities later steps need."""
    document = client.call("project.create", {"name": "Tension plate"}, key="create")
    outline = [[0, 0, 0], [WIDTH, 0, 0], [WIDTH, HEIGHT, 0], [0, HEIGHT, 0]]
    region = client.call("geometry.create_region",
                         {"outer": {"corners": [{"position_mm": corner} for corner in outline]}},
                         document, "region")["entity_id"]
    task = finish(client, client.call("mesh.generate_region",
                                      {"geometry_id": region, "element_size_mm": ELEMENT_SIZE},
                                      client.current(), "mesh"))
    mesh = task["receipt"]["entity_id"]
    material = client.call("material.create", {"name": "Steel", "young_modulus": quantity(210, "GPa"),
                                                "poisson_ratio": .3}, client.current(), "material")["entity_id"]
    section = client.call("shell_section.create", {"name": "Plate", "material_id": material,
                                                   "thickness_mm": THICKNESS},
                          client.current(), "section")["entity_id"]
    client.call("mesh.assign_shell_section", {"mesh_id": mesh, "section_id": section},
                client.current(), "assign")
    load_case = client.call("load_case.create", {"name": "Tension", "force_ids": [], "constraint_ids": []},
                            client.current(), "load-case")["entity_id"]

    def edge(edge_index):
        return {"load_case_id": load_case, "mesh_id": mesh, "loop": 0, "edge": edge_index}
    client.call("load_case.add_edge_force", {**edge(RIGHT), "x": quantity(FORCE / 1000, "kN"),
                                             "y": quantity(0, "N"), "z": quantity(0, "N")},
                client.current(), "pull")
    client.call("load_case.add_edge_constraint", {**edge(LEFT), "dofs": "1"}, client.current(), "left")
    client.call("load_case.add_edge_constraint", {**edge(BOTTOM), "dofs": "2"}, client.current(), "bottom")
    return {"mesh": mesh, "section": section, "load_case": load_case, "task": task["task_id"]}


def check_model(client, plate):
    positions = {node: client.fields(node)["position"]
                 for node in all_entities(client, "node") if node.startswith(f"node-{plate['task']}-")}
    triangles = [t for t in all_entities(client, "tri3") if t.startswith(f"tri-{plate['task']}-")]
    assert triangles and all(client.fields(t)["section"] == plate["section"] for t in triangles), \
        "every triangle must have the shell section"

    load_case = client.fields(plate["load_case"])
    forces = [client.fields(force) for force in load_case["forces"]]
    right = {node for node, p in positions.items() if math.isclose(p[0], WIDTH, abs_tol=1e-9)}
    assert {force["node"] for force in forces} == right, "the force must act on every right-edge node"
    total = [sum(force["force_n"][axis] for force in forces) for axis in range(3)]
    assert math.isclose(total[0], FORCE, rel_tol=1e-12) and total[1] == 0 and total[2] == 0, total

    constraints = {client.fields(c)["dofs"]: client.fields(c)["nodes"] for c in load_case["constraints"]}
    left = {node for node, p in positions.items() if math.isclose(p[0], 0, abs_tol=1e-9)}
    bottom = {node for node, p in positions.items() if math.isclose(p[1], 0, abs_tol=1e-9)}
    assert set(constraints["1"]) == left and set(constraints["2"]) == bottom, "edge supports"
    return len(positions), len(triangles), len(forces)


def run(engine, cli):
    with tempfile.TemporaryDirectory(prefix="qcae-plate-", dir=Path("/tmp").resolve()) as folder:
        root = Path(folder)
        endpoint, workspace = str(root / "engine.sock"), str(root / "work.sqlite")
        client = Client(engine, cli, endpoint, "script")
        with (root / "engine.log").open("w+") as log:
            process = subprocess.Popen([engine, "--socket", endpoint, "--workspace", workspace],
                                       stdout=log, stderr=log)
            try:
                deadline = time.monotonic() + 15
                while True:
                    try:
                        client.call("capabilities.list")
                        break
                    except (OSError, AssertionError, json.JSONDecodeError):
                        assert process.poll() is None and time.monotonic() < deadline, "engine did not start"
                        time.sleep(.05)
                return check_model(client, build_plate(client))
            finally:
                process.terminate()
                process.wait(timeout=10)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True)
    parser.add_argument("--cli", required=True)
    args = parser.parse_args()
    nodes, triangles, forces = run(str(Path(args.engine).resolve()), str(Path(args.cli).resolve()))
    print(f"PASS: tension plate built through the engine: {nodes} nodes, {triangles} triangles with "
          f"a shell section, {forces} edge forces summing to {FORCE:.0f} N, left and bottom edges supported")


if __name__ == "__main__":
    main()
