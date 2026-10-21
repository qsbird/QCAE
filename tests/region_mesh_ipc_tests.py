#!/usr/bin/env python3
"""G2: a plate with a hole is created and meshed by the engine's Gmsh mesher, end to end.

The plate is created with geometry.create_region and meshed twice with mesh.generate_region.
Both meshes must be bound to the region and identical node for node; every triangle must turn
counter-clockwise and stay out of the hole. An invalid region and a too fine element size must be
refused with useful diagnostics.
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

PLATE = [[0, 0, 0], [200, 0, 0], [200, 100, 0], [0, 100, 0]]
HOLE_RADIUS = 20
HOLE = [[100 + HOLE_RADIUS * math.cos(2 * math.pi * i / 16), 50 + HOLE_RADIUS * math.sin(2 * math.pi * i / 16), 0]
        for i in range(16)]


def loop(points):
    return {"corners": [{"position_mm": point} for point in points]}


def finish(client, task):
    deadline = time.monotonic() + 30
    while task["state"] not in {"succeeded", "failed", "cancelled", "conflicted"}:
        assert time.monotonic() < deadline, task
        time.sleep(.02)
        task = client.call("task.status", {"task_id": task["task_id"]}, client.current())
    return task


def all_entities(client, kind):
    """Every entity of a kind; entity.query returns pages of at most 1000."""
    result, offset = [], 0
    while True:
        page = client.call("entity.query", {"kind": kind, "offset": offset, "limit": 1000}, client.current())
        result += [item["entity_id"] for item in page["entities"]]
        offset += len(page["entities"])
        if offset >= page["total"] or not page["entities"]:
            return result


def mesh_of(client, task_id):
    """Node positions and triangles (as node indices) of the mesh a task published."""
    def index(entity):
        return int(entity.rsplit("-", 1)[1])
    nodes = sorted((entity for entity in all_entities(client, "node")
                    if entity.startswith(f"node-{task_id}-")), key=index)
    triangles = sorted((entity for entity in all_entities(client, "tri3")
                        if entity.startswith(f"tri-{task_id}-")), key=index)
    positions = [client.fields(node)["position"] for node in nodes]
    connectivity = [[index(node) for node in client.fields(triangle)["nodes"]] for triangle in triangles]
    return positions, connectivity


def check_triangles(positions, connectivity):
    for corners in connectivity:
        a, b, c = (positions[i] for i in corners)
        twice_area = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])
        assert twice_area > 0, ("triangle is not counter-clockwise", corners)
        centre = [(a[k] + b[k] + c[k]) / 3 for k in range(2)]
        inscribed = HOLE_RADIUS * math.cos(math.pi / 16)
        assert math.dist(centre, [100, 50]) > inscribed, ("triangle lies in the hole", corners)


def run(engine, cli):
    with tempfile.TemporaryDirectory(prefix="qcae-region-mesh-", dir=Path("/tmp").resolve()) as folder:
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
                        capabilities = client.call("capabilities.list")
                        break
                    except (OSError, AssertionError, json.JSONDecodeError):
                        assert process.poll() is None and time.monotonic() < deadline, "engine did not start"
                        time.sleep(.05)
                operations = {item["name"]: item for item in capabilities["operations"]}
                assert operations["geometry.create_region"]["available"]
                assert operations["mesh.generate_region"]["available"], operations["mesh.generate_region"]

                document = client.call("project.create", {"name": "Plate with a hole"}, key="create")
                refused = client.call("geometry.create_region",
                                      {"outer": loop(PLATE), "holes": [loop([[190, 10, 0], [220, 10, 0], [220, 30, 0]])]},
                                      document, "crossing", expected="failed")
                assert refused["error"]["field"] == "holes[0]", refused

                region = client.call("geometry.create_region", {"outer": loop(PLATE), "holes": [loop(HOLE)]},
                                     client.current(), "plate")["entity_id"]
                meshes = []
                for attempt in ("first", "second"):
                    task = finish(client, client.call("mesh.generate_region",
                                                      {"geometry_id": region, "element_size_mm": 10},
                                                      client.current(), attempt))
                    assert task["state"] == "succeeded", task
                    mesh = client.fields(task["receipt"]["entity_id"])
                    assert mesh["origin"] == "geometry" and mesh["geometry"] == region, mesh
                    meshes.append(mesh_of(client, task["task_id"]))
                positions, connectivity = meshes[0]
                assert len(positions) > 100 and len(connectivity) > 150, (len(positions), len(connectivity))
                assert meshes[0] == meshes[1], "the same region meshed twice gave different meshes"
                check_triangles(positions, connectivity)

                too_fine = finish(client, client.call("mesh.generate_region",
                                                      {"geometry_id": region, "element_size_mm": 0.5},
                                                      client.current(), "too-fine"))
                assert too_fine["state"] == "failed", too_fine
                assert "exceed the limit of 20000" in too_fine["diagnostic"]["message"], too_fine
                return len(positions), len(connectivity)
            finally:
                process.terminate()
                process.wait(timeout=10)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True)
    parser.add_argument("--cli", required=True)
    args = parser.parse_args()
    nodes, triangles = run(str(Path(args.engine).resolve()), str(Path(args.cli).resolve()))
    print(f"PASS: plate with a hole meshed by Gmsh through the engine, twice identically "
          f"({nodes} nodes, {triangles} triangles); invalid region and too fine size refused")


if __name__ == "__main__":
    main()
