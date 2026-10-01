#!/usr/bin/env python3
"""Record the UTF-8 output actually returned by an EXT-01 command."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys

evidence = Path(__file__).resolve().parent
name, kind, *command = sys.argv[1:]
result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
output = result.stdout.decode("utf-8")
(evidence / (name + ".log")).write_bytes(result.stdout)
delivered = output
if len(result.stdout) > 16000:
    marker = "\n--- Output clipped by evidence runner; complete disk log is not delivered ---\n"
    delivered = result.stdout[:8000].decode("utf-8", errors="ignore") + marker
    delivered += result.stdout[-8000:].decode("utf-8", errors="ignore")
with (evidence / "commands.jsonl").open("a") as log:
    log.write(json.dumps({"name": name, "kind": kind, "command": command,
                          "exit_code": result.returncode, "read_count": 1,
                          "disk_utf8_bytes": len(result.stdout),
                          "delivered_utf8_bytes": len(delivered.encode("utf-8")),
                          "output_sha256": hashlib.sha256(result.stdout).hexdigest(),
                          "delivered_complete_output": delivered == output,
                          "delivery_range": "complete" if delivered == output else "first and last 8000 raw bytes",
                          "tokens": None}) + "\n")
sys.stdout.write(delivered)
sys.exit(result.returncode)
