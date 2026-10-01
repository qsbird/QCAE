#!/usr/bin/env python3
"""Compare actual installed and isolated HarfBuzz shaping without a GUI.

The observer manifest, API entries and bulk/growth facts are verified. Typed
glyph/cache and native ownership gaps remain explicit; this is not a whole
pipeline coverage assertion or a change to the frozen Qt prefix.
"""

from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
from pathlib import Path


class GlyphInfo(ctypes.Structure):
    _fields_ = [(name, ctypes.c_uint32) for name in
                ("codepoint", "mask", "cluster", "var1", "var2")]


class GlyphPosition(ctypes.Structure):
    _fields_ = [(name, ctypes.c_int32) for name in
                ("x_advance", "y_advance", "x_offset", "y_offset", "var")]


CALLBACK = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_char_p,
                           ctypes.c_uint, ctypes.c_uint64)


def library(path: Path) -> ctypes.CDLL:
    api = ctypes.CDLL(str(path))
    signatures = {
        "hb_version_string": ([], ctypes.c_char_p),
        "hb_shape_list_shapers": ([], ctypes.POINTER(ctypes.c_char_p)),
        "hb_blob_create_from_file_or_fail": ([ctypes.c_char_p], ctypes.c_void_p),
        "hb_blob_destroy": ([ctypes.c_void_p], None),
        "hb_face_create": ([ctypes.c_void_p, ctypes.c_uint], ctypes.c_void_p),
        "hb_face_get_upem": ([ctypes.c_void_p], ctypes.c_uint),
        "hb_face_destroy": ([ctypes.c_void_p], None),
        "hb_font_create": ([ctypes.c_void_p], ctypes.c_void_p),
        "hb_font_destroy": ([ctypes.c_void_p], None),
        "hb_ot_font_set_funcs": ([ctypes.c_void_p], None),
        "hb_font_set_scale": ([ctypes.c_void_p, ctypes.c_int, ctypes.c_int], None),
        "hb_buffer_create": ([], ctypes.c_void_p),
        "hb_buffer_destroy": ([ctypes.c_void_p], None),
        "hb_buffer_add_utf8": ([ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int,
                                ctypes.c_uint, ctypes.c_int], None),
        "hb_buffer_guess_segment_properties": ([ctypes.c_void_p], None),
        "hb_shape_full": ([ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p,
                           ctypes.c_uint, ctypes.POINTER(ctypes.c_char_p)], ctypes.c_int),
        "hb_buffer_get_glyph_infos": ([ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint)],
                                      ctypes.POINTER(GlyphInfo)),
        "hb_buffer_get_glyph_positions": ([ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint)],
                                          ctypes.POINTER(GlyphPosition)),
    }
    for name, (arguments, result) in signatures.items():
        function = getattr(api, name)
        function.argtypes = arguments
        function.restype = result
    return api


def shapers(api: ctypes.CDLL) -> list[str]:
    values = api.hb_shape_list_shapers()
    result = []
    index = 0
    while values[index]:
        result.append(values[index].decode())
        index += 1
    return result


def shape(api: ctypes.CDLL, font_path: Path, text: str, shaper: str | None) -> dict:
    blob = api.hb_blob_create_from_file_or_fail(str(font_path).encode())
    if not blob:
        raise RuntimeError("The actual font input could not be opened")
    face = api.hb_face_create(blob, 0)
    font = api.hb_font_create(face)
    buffer = api.hb_buffer_create()
    try:
        api.hb_ot_font_set_funcs(font)
        scale = api.hb_face_get_upem(face)
        api.hb_font_set_scale(font, scale, scale)
        encoded = text.encode()
        api.hb_buffer_add_utf8(buffer, encoded, len(encoded), 0, len(encoded))
        api.hb_buffer_guess_segment_properties(buffer)
        requested = ((ctypes.c_char_p * 2)(shaper.encode(), None) if shaper else None)
        success = bool(api.hb_shape_full(font, buffer, None, 0, requested))
        info_count = ctypes.c_uint()
        position_count = ctypes.c_uint()
        infos = api.hb_buffer_get_glyph_infos(buffer, ctypes.byref(info_count))
        positions = api.hb_buffer_get_glyph_positions(buffer, ctypes.byref(position_count))
        if info_count.value != position_count.value:
            raise RuntimeError("The real glyph output arrays have different lengths")
        return {"success": success, "glyphs": [
            {"glyph_id": infos[index].codepoint, "cluster": infos[index].cluster,
             "mask": infos[index].mask, "advance": [positions[index].x_advance,
                                                       positions[index].y_advance],
             "offset": [positions[index].x_offset, positions[index].y_offset]}
            for index in range(info_count.value)]}
    finally:
        api.hb_buffer_destroy(buffer)
        api.hb_font_destroy(font)
        api.hb_face_destroy(face)
        api.hb_blob_destroy(blob)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--installed", type=Path,
                        default=Path("/opt/homebrew/opt/harfbuzz/lib/libharfbuzz.dylib"))
    parser.add_argument("--observed", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--font", type=Path, default=Path("/System/Library/Fonts/SFNS.ttf"))
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    report = {"schema": 1, "passed": False, "coverage_complete": False,
              "installed_library": str(args.installed.resolve()),
              "observed_library": str(args.observed.resolve()),
              "observed_sha256": hashlib.sha256(args.observed.read_bytes()).hexdigest(),
              "font": str(args.font),
              "font_sha256": hashlib.sha256(args.font.read_bytes()).hexdigest(),
              "cases": [], "sites": {}, "callback_error": None}
    manifest_sites = set()

    @CALLBACK
    def observe(_context: int, site: bytes, kind: int, size: int) -> None:
        try:
            name = site.decode()
            if kind > 3 or name not in manifest_sites:
                raise RuntimeError("An observed source site or kind is outside the exact manifest")
            key = str(kind) + ":" + name
            if key not in report["sites"]:
                if len(report["sites"]) >= 4096:
                    raise RuntimeError("The source observer site limit was exceeded")
                report["sites"][key] = {"site": name, "kind": kind, "calls": 0, "bytes": 0}
            value = report["sites"][key]
            value["calls"] += 1
            value["bytes"] += size
        except Exception as error:
            report["callback_error"] = str(error)

    observed = None
    try:
        installed = library(args.installed)
        observed = library(args.observed)
        observed.qcae_hb_sdk_observer_manifest.argtypes = []
        observed.qcae_hb_sdk_observer_manifest.restype = ctypes.c_char_p
        observed.qcae_hb_sdk_observer_install.argtypes = [CALLBACK, ctypes.c_void_p]
        observed.qcae_hb_sdk_observer_install.restype = None
        actual = json.loads(observed.qcae_hb_sdk_observer_manifest().decode())
        expected = json.loads(args.manifest.read_text())
        if actual != expected:
            raise RuntimeError("The actual loaded source manifest differs from the requested manifest")
        report["manifest_sha256"] = hashlib.sha256(args.manifest.read_bytes()).hexdigest()
        report["remaining"] = actual["remaining"]
        manifest_sites.update(item["site"] for item in actual["sites"])
        report["version"] = observed.hb_version_string().decode()
        report["installed_shapers"] = shapers(installed)
        report["observed_shapers"] = shapers(observed)
        if (report["version"] != installed.hb_version_string().decode() or
                report["observed_shapers"] != report["installed_shapers"]):
            raise RuntimeError("The actual installed and observed shaping capabilities differ")
        observed.qcae_hb_sdk_observer_install(observe, None)
        for shaper in (None, "ot", "coretext"):
            for text in ("Steel E = 200000 MPa", "梁 αβ", "العربية", "देवनागरी", "🙂"):
                original = shape(installed, args.font, text, shaper)
                measured = shape(observed, args.font, text, shaper)
                case = {"text": text, "shaper": shaper or "default", "original": original,
                        "observed": measured, "equal": original == measured}
                report["cases"].append(case)
                if not case["equal"] or not measured["success"]:
                    raise RuntimeError("Original/observed shaping failed or changed actual glyph output")
        entries = sum(item["calls"] for item in report["sites"].values() if item["kind"] == 0)
        copied = sum(item["bytes"] for item in report["sites"].values() if item["kind"] in (1, 2))
        unsupported = sum(item["calls"] for item in report["sites"].values() if item["kind"] == 3)
        report["entry_calls"] = entries
        report["known_copy_write_upper_bound"] = copied
        report["unsupported_calls"] = unsupported
        if report["callback_error"] or not entries or not copied or not unsupported:
            raise RuntimeError("Actual source calls, byte observations or remaining gap evidence is missing")
        report["passed"] = True
        print("PASS: exact loaded marker/shapers, 15 original glyph outputs, actual source facts; coverage remains unknown")
        return 0
    except Exception as error:
        report["error"] = str(error)
        print("FAIL:", error)
        return 1
    finally:
        if observed is not None and hasattr(observed, "qcae_hb_sdk_observer_install"):
            observed.qcae_hb_sdk_observer_install(CALLBACK(), None)
        args.report.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n")


if __name__ == "__main__":
    raise SystemExit(main())
