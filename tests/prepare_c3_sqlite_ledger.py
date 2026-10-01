#!/usr/bin/env python3
"""Prepare an untracked, same-version SQLite object for byte observation.

The C frontend runs at O0 so aggregate assignments still appear as LLVM copy
intrinsics. Rewrite those and explicit C copies to opaque observer calls BEFORE
optional O2 optimization. The observer prevents the optimizer from discarding or
inlining the observed copy. This is a byte upper-bound build, not a latency run.
"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


def run(command):
    subprocess.run(command, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="official 3.51.0 sqlite3.c amalgamation")
    parser.add_argument("output", type=Path, help="untracked temporary output directory")
    parser.add_argument("--clang", default="clang")
    parser.add_argument("--codegen-optimization", choices=("0", "2"), default="2")
    arguments = parser.parse_args()
    original = arguments.source.read_text()
    if not re.search(r'#define SQLITE_VERSION\s+"3\.51\.0"', original):
        raise RuntimeError("Expected the official SQLite 3.51.0 amalgamation")
    # All C buffer-copy calls in this amalgamation use these two functions.
    # Reject new source variants instead of silently missing another function.
    unsupported = re.findall(
        r"\b(?:strcpy|strncpy|strcat|strncat|strdup|strndup|bcopy|memccpy)\s*\(", original
    )
    if unsupported:
        raise RuntimeError(f"Unaudited C copy routines: {unsupported}")
    marker = "  assert( nByte==(int)(zPayload - (u8*)pOut->z) );"
    if original.count(marker) != 1:
        raise RuntimeError("OP_MakeRecord output boundary changed")
    source = original.replace(marker, marker + "\n  qcae_c3_sqlite_encoded((size_t)nByte);")
    prelude = """#include <stddef.h>
#include <string.h>
#include <stdlib.h>
extern void *qcae_c3_sqlite_memcpy_tagged(void *, const void *, size_t, const char *);
extern void *qcae_c3_sqlite_memmove_tagged(void *, const void *, size_t, const char *);
extern void qcae_c3_sqlite_encoded(size_t);
extern void *qcae_c3_sqlite_realloc(void *, size_t, const char *);
int qcae_c3_sqlite_copy_instrumentation_version(void) { return 3051000; }
#undef memcpy
#undef memmove
#define memcpy(d,s,n) qcae_c3_sqlite_memcpy_tagged(d,s,n,__func__)
#define memmove(d,s,n) qcae_c3_sqlite_memmove_tagged(d,s,n,__func__)
#define realloc(p,n) qcae_c3_sqlite_realloc(p,n,__func__)
"""
    arguments.output.mkdir(parents=True, exist_ok=True)
    prepared = arguments.output / "sqlite3-observed.c"
    frontend = arguments.output / "sqlite3-frontend.ll"
    observed = arguments.output / "sqlite3-observed.ll"
    object_file = arguments.output / "sqlite3-observed.o"
    prepared.write_text(prelude + source)
    # Native 64-bit size_t and opaque-pointer LLVM IR are intentionally required.
    # optnone is disabled so optimization AFTER the observer rewrite is allowed.
    frontend_options = [
        "-O0", "-Xclang", "-disable-O0-optnone", "-fno-builtin",
        "-DSQLITE_THREADSAFE=1", "-DSQLITE_ENABLE_COLUMN_METADATA=1", "-DNDEBUG=1",
        "-S", "-emit-llvm",
    ]
    run([arguments.clang, *frontend_options, str(prepared), "-o", str(frontend)])
    llvm = frontend.read_text()
    if not re.search(r'target datalayout = "[^"\n]*p(?:0)?:64:', llvm):
        # arm64 defaults to 64-bit pointers without an explicit p0 entry.
        if not re.search(r'target triple = "(?:arm64|aarch64|x86_64)-', llvm):
            raise RuntimeError("Only native 64-bit LLVM pointer layouts were audited")
    counts = {"memcpy": 0, "memmove": 0}
    function = "unknown"
    function_names = {}
    copy_call = re.compile(
        r"(?P<prefix>^[ \t]*(?:tail )?call) void @llvm\.(?P<kind>memcpy|memmove)"
        r"(?:\.inline)?\.p0\.p0\.i64\((?P<arguments>.*)\)(?: #\d+)?$", re.M
    )

    def rewrite(match):
        kind = match.group("kind")
        if kind != "memcpy":
            raise RuntimeError("A new aggregate memmove path requires its own bridge")
        counts[kind] += 1
        parts = []
        depth = 0
        start = 0
        for index, character in enumerate(match.group("arguments")):
            depth += character in "(["
            depth -= character in ")]"
            if character == "," and depth == 0:
                parts.append(match.group("arguments")[start:index].strip())
                start = index + 1
        parts.append(match.group("arguments")[start:].strip())
        if len(parts) != 4 or parts[3] not in ("i1 false", "i1 true"):
            raise RuntimeError(f"Unaudited copy argument list: {parts}")
        # Keep nested getelementptr constant expressions, remove only the
        # intrinsic's pointer alignment attributes from the first two operands.
        destination = re.sub(r"^ptr(?: align \d+)? ", "", parts[0])
        source = re.sub(r"^ptr(?: align \d+)? ", "", parts[1])
        if not parts[2].startswith("i64 "):
            raise RuntimeError("Unaudited size_t width in copy intrinsic")
        prefix = re.sub(r"(?:tail )?call$", f"%qcae_copy_{sum(counts.values())} = call", match.group("prefix"))
        name = function_names.setdefault(function, f"qcae_copy_function_{len(function_names)}")
        return (
            f'{prefix} ptr @qcae_c3_sqlite_memcpy_aggregate'
            f'(ptr {destination}, ptr {source}, {parts[2]}, ptr @{name})'
        )

    rewritten = []
    for line in llvm.splitlines():
        definition = re.match(r"^define .* @([-\w.]+)\(", line)
        if definition:
            function = definition.group(1)
        rewritten.append(copy_call.sub(rewrite, line))
    llvm = "\n".join(rewritten) + "\n"
    llvm = re.sub(r"^declare void @llvm\.(?:memcpy|memmove)[^\n]*\n", "", llvm, flags=re.M)
    if re.search(r"@llvm\.(?:memcpy|memmove)|@(?:memcpy|memmove)\b", llvm):
        raise RuntimeError("An unrewritten copy remains in frontend IR")
    llvm += "\ndeclare ptr @qcae_c3_sqlite_memcpy_aggregate(ptr, ptr, i64, ptr)\n"
    for function, symbol in function_names.items():
        if not re.fullmatch(r"[-\w.]+", function):
            raise RuntimeError("Unaudited LLVM symbol string encoding")
        llvm += f'@{symbol} = private unnamed_addr constant [{len(function) + 1} x i8] c"{function}\\00"\n'
    observed.write_text(llvm)
    codegen_options = ["-O" + arguments.codegen_optimization, "-fno-builtin", "-c", "-x", "ir"]
    run([arguments.clang, *codegen_options, str(observed), "-o", str(object_file)])
    # This also verifies that later optimization/code generation introduced no
    # libc copy calls outside the bridge. The bridge itself is a separate TU.
    symbols = subprocess.check_output(["nm", "-u", str(object_file)], text=True)
    if re.search(r"\b_?(?:memcpy|memmove|realloc)\b", symbols):
        raise RuntimeError("Code generation introduced an unobserved libc copy")
    metadata = {
        "sqlite_version": "3.51.0",
        "source_sha256": hashlib.sha256(original.encode()).hexdigest(),
        "source": str(arguments.source.resolve()),
        "object": str(object_file.resolve()),
        "compiler": subprocess.check_output([arguments.clang, "--version"], text=True).splitlines()[0],
        "frontend_options": frontend_options,
        "codegen_options": codegen_options,
        "rewritten_aggregate_copy_intrinsics": counts,
        "unobserved_libc_copy_symbols": False,
        "measurement": "Observed buffer copies; conservative aggregate copies retained before optimization",
        "latency_comparable_to_production": False,
        "driver_internal_includes_transient_bind": True,
    }
    (arguments.output / "instrumentation.json").write_text(json.dumps(metadata, indent=2) + "\n")
    print(json.dumps(metadata, indent=2))


if __name__ == "__main__":
    main()
