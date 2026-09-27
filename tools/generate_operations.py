#!/usr/bin/env python3
"""Generate a dependency-free C++ descriptor catalog from the design source."""

import argparse
import json
from pathlib import Path


def generate(source):
    catalog = json.loads(source.read_text(encoding="utf-8"))
    rows = catalog["operations"]
    if len({row["name"] for row in rows}) != len(rows):
        raise ValueError("Duplicate operation names")
    if catalog["operation_count"] != len(rows):
        raise ValueError("Operation count does not match")
    def quote(value):
        return json.dumps(value, ensure_ascii=True)
    lines = ["// Generated from docs/contracts/operations.json; do not edit.", "#pragma once",
             "#include <array>", "#include <string_view>", "namespace qcae {",
             "struct OperationDescriptor {",
             "  std::string_view name, effect, description, input_type, output_type, target_context;",
             "  bool requires_document, requires_epoch, requires_revision, requires_idempotency_key, requires_profile_match;",
             "};",
             f"inline constexpr std::string_view api_version = {quote(catalog['api_version'])};",
             f"inline constexpr std::array<OperationDescriptor, {len(rows)}> operation_catalog = {{{{"]
    for row in rows:
        strings = [quote(row[key]) for key in ("name", "effect", "description", "input_type", "output_type", "target_context")]
        flags = []
        for key in ("requires_document", "requires_epoch", "requires_revision", "requires_idempotency_key", "requires_profile_match"):
            if type(row[key]) is not bool:
                raise ValueError(f"{row['name']}.{key} must be a boolean")
            flags.append("true" if row[key] else "false")
        lines.append("  {" + ", ".join(strings + flags) + "},")
    lines += ["}};", "inline constexpr const OperationDescriptor* find_operation(std::string_view name) {",
              "  for (const auto& operation : operation_catalog) if (operation.name == name) return &operation;",
              "  return nullptr;", "}", "} // namespace qcae", ""]
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    expected = generate(args.source)
    if args.check:
        if not args.output.is_file() or args.output.read_text(encoding="utf-8") != expected:
            raise SystemExit("Generated descriptor catalog is stale")
        print("Generated descriptor catalog matches its source")
    else:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(expected, encoding="utf-8")


if __name__ == "__main__":
    main()
