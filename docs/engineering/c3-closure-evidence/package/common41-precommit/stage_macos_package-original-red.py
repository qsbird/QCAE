#!/usr/bin/env python3
"""Create a new local app staging copy and verify its actual Mach-O dependency closure."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
SYSTEM_ROOTS = ("/System/Library/", "/usr/lib/")


def command(*arguments):
    try:
        return subprocess.check_output([str(value) for value in arguments], stderr=subprocess.STDOUT, text=True)
    except subprocess.CalledProcessError as error:
        sys.stderr.write(error.output)
        raise


def dependencies(path):
    own = command("otool", "-D", path).splitlines()[1:]
    identity = own[0].strip() if own else None
    return [value for line in command("otool", "-L", path).splitlines()[1:]
            if (value := line.strip().split(" (compatibility version", 1)[0]) != identity]


def rpaths(path):
    lines = command("otool", "-l", path).splitlines()
    return [lines[index + 2].strip().split("path ", 1)[1].rsplit(" (offset", 1)[0]
            for index, line in enumerate(lines) if line.strip() == "cmd LC_RPATH"]


def macho(path):
    if not path.is_file() or path.is_symlink():
        return False
    with path.open("rb") as stream:
        return stream.read(4) in {b"\xcf\xfa\xed\xfe", b"\xfe\xed\xfa\xcf", b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca"}


def image_uuids(path):
    return sorted(tuple(line.split()[:3]) for line in command("dwarfdump", "--uuid", path).splitlines())


def expand(value, binary, executable):
    value = value.replace("@loader_path", str(binary.parent)).replace("@executable_path", str(executable.parent))
    return Path(value)


def resolve(dependency, binary, executable, paths, extra):
    if dependency.startswith("@rpath/"):
        relative = dependency[len("@rpath/"):]
        candidates = [expand(path, binary, executable) / relative for path in paths]
        candidates += [Path(path) / relative for path in extra]
    elif dependency.startswith("@"):
        candidates = [expand(dependency, binary, executable)]
    else:
        candidates = [Path(dependency)]
    for candidate in candidates:
        if candidate.is_file():
            return candidate.resolve()
    raise ValueError(f"unresolved dependency {dependency!r} of {binary}")


def stage(args):
    if sys.platform != "darwin":
        raise ValueError("this staging tool requires macOS; other systems are unverified")
    build, bundle = Path(args.build_dir).resolve(), Path(args.bundle).absolute()
    allowed = ROOT / "out"
    if not bundle.is_relative_to(allowed) or bundle.suffix != ".app" or bundle.exists():
        raise ValueError("bundle must be a new .app directory below this workspace's ignored out/")
    cache = (build / "CMakeCache.txt").read_text()
    if "CMAKE_BUILD_TYPE:STRING=Release" not in cache or "QCAE_C3_SQLITE_OBSERVED_OBJECT:FILEPATH=\n" not in cache:
        raise ValueError("only a regular Release build with no test SQLite observer can be packaged")
    macos, frameworks, resources = (bundle / "Contents" / part for part in ("MacOS", "Frameworks", "Resources"))
    macos.mkdir(parents=True)
    frameworks.mkdir()
    resources.mkdir()
    executables = [macos / name for name in ("qcae-desktop", "qcae-engine", "qcae-cli", "qcae-mcp")]
    for target in executables:
        shutil.copy2(build / target.name, target)
    shutil.copy2(build / "qcae_mcp_bridge.py", resources / "qcae_mcp_bridge.py")
    plist = {"CFBundleName": "QCAE", "CFBundleDisplayName": "QCAE", "CFBundleIdentifier": "org.qcae.desktop",
             "CFBundleExecutable": "qcae-desktop", "CFBundlePackageType": "APPL",
             "CFBundleVersion": "0.0.1", "CFBundleShortVersionString": "0.0.1",
             "NSHighResolutionCapable": True, "LSMinimumSystemVersion": "26.0"}
    (bundle / "Contents/Info.plist").write_bytes(plistlib.dumps(plist))
    (resources / "DEPLOYMENT.txt").write_text("Local arm64 staging diagnostic, not a completed P0 release.\nMCP requires an existing Python 3.10+ interpreter (--python). No solver, credentials or workspaces included.\n")
    deployment = command(args.macdeployqt, bundle, "-no-strip", "-no-codesign", "-verbose=1",
                         *[f"-executable={path}" for path in executables[1:]],
                         *[f"-libpath={Path(path).resolve()}" for path in args.libpath])
    (resources / "qt-deployment.log").write_text(deployment)
    mapping = {}
    pending = [(path, path) for path in bundle.rglob("*") if macho(path)]
    seen = set()
    while pending:
        source, target = pending.pop()
        if target in seen:
            continue
        seen.add(target)
        paths = rpaths(source)
        for dependency in dependencies(source):
            if dependency.startswith(SYSTEM_ROOTS):
                continue
            resolved = resolve(dependency, source, executables[0], paths,
                               [frameworks, *args.libpath])
            if resolved == source.resolve():
                continue  # dylib's own LC_ID_DYLIB, not a runtime dependency
            if resolved.is_relative_to(bundle):
                destination = resolved
            else:
                if ".framework" in str(resolved):
                    origin = next(parent for parent in resolved.parents if parent.suffix == ".framework")
                    framework = frameworks / origin.name
                    destination = framework / resolved.relative_to(origin)
                    if not framework.exists():
                        shutil.copytree(origin, framework, symlinks=True)
                        for binary in framework.rglob("*"):
                            if macho(binary):
                                original = origin / binary.relative_to(framework)
                                pending.append((original, binary))
                                command("install_name_tool", "-id", "@rpath/" + str(binary.relative_to(frameworks)), binary)
                    if not destination.is_file():
                        raise ValueError(f"framework deployment did not preserve {resolved}")
                else:
                    destination = mapping.get(resolved, frameworks / resolved.name)
                if resolved not in mapping and ".framework" not in str(resolved):
                    if destination.exists():
                        if not macho(destination) or image_uuids(resolved) != image_uuids(destination):
                            raise ValueError(f"library basename collision: {resolved}")
                    else:
                        shutil.copy2(resolved, destination)
                    mapping[resolved] = destination
                    pending.append((resolved, destination))
                    command("install_name_tool", "-id", "@rpath/" + destination.name, destination)
            relative = "@loader_path/" + os.path.relpath(destination, target.parent)
            if dependency != relative:
                command("install_name_tool", "-change", dependency, relative, target)
        for path in rpaths(target):
            if path.startswith("/") and not path.startswith(SYSTEM_ROOTS):
                command("install_name_tool", "-delete_rpath", path, target)
    binaries = sorted(path for path in bundle.rglob("*") if macho(path))
    verified = []
    for binary in binaries:
        paths = rpaths(binary)
        for dependency in dependencies(binary):
            if dependency.startswith(SYSTEM_ROOTS):
                continue
            resolved = resolve(dependency, binary, executables[0], paths, [frameworks])
            if not resolved.is_relative_to(bundle):
                raise ValueError(f"external runtime dependency remains: {resolved}")
        if binary != executables[0]:
            command("codesign", "--force", "--sign", "-", "--timestamp=none", binary)
        verified.append({"path": str(binary.relative_to(bundle)), "dependencies": dependencies(binary)})
    licenses = resources / "licenses"
    licenses.mkdir()
    vtk_licenses = ROOT / "build-vtk-deps/install/share/licenses/VTK"
    if vtk_licenses.is_dir():
        shutil.copytree(vtk_licenses, licenses / "VTK")
    qt_licenses = Path(args.qt_license_dir).resolve()
    if not qt_licenses.is_dir():
        raise ValueError("the matching Qt source LICENSES directory is required")
    shutil.copytree(qt_licenses, licenses / "Qt")
    # Seal after all resources are in place; this is local launch, not public distribution.
    command("codesign", "--force", "--deep", "--sign", "-", "--timestamp=none", bundle)
    signature = command("codesign", "--verify", "--deep", "--strict", bundle)
    manifest = {"schema_version": 1, "status": "local_staging_dependency_closure_verified",
                "complete_p0_release": False, "public_notarization": False,
                "architecture": "arm64", "minimum_deployment_target": "26.0_unverified_on_older_systems",
                "python_runtime": "existing Python >=3.10 required; not bundled",
                "source_head": command("git", "-C", ROOT, "rev-parse", "HEAD").strip(),
                "source_dirty": bool(command("git", "-C", ROOT, "status", "--porcelain").strip()),
                "binaries": verified, "local_signature_verification": signature.strip(),
                "files": [{"path": str(path.relative_to(bundle)), "size": path.stat().st_size,
                           "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
                          for path in sorted(bundle.rglob("*")) if path.is_file() and not path.is_symlink()]}
    (bundle.parent / (bundle.stem + "-manifest.json")).write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps({"bundle": str(bundle), "macho_files": len(binaries), "files": len(manifest["files"]), "complete_p0_release": False}))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True)
    parser.add_argument("--bundle", required=True)
    parser.add_argument("--macdeployqt", default="macdeployqt")
    parser.add_argument("--libpath", action="append", default=[])
    parser.add_argument("--qt-license-dir", required=True)
    stage(parser.parse_args())
