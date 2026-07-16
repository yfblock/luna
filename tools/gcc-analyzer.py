#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Re-run Luna's own C translation units through GCC -fanalyzer."""

import json
import shlex
import subprocess
import sys
import tempfile
from pathlib import Path


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    database = root / "deps/build_lkl/compile_commands.json"
    if not database.exists():
        print("compile database missing; run ./run.sh --build-only", file=sys.stderr)
        return 2
    try:
        entries = json.loads(database.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        print(f"invalid compile database: {error}", file=sys.stderr)
        return 2

    source_root = root / "apps/lkl-root-task/src"
    selected = [
        entry for entry in entries
        if Path(entry.get("file", "")).parent == source_root
    ]
    if not selected:
        print("compile database contains no Luna C translation units", file=sys.stderr)
        return 2

    failed = False
    with tempfile.TemporaryDirectory(prefix="luna-gcc-analyzer-") as output:
        for index, entry in enumerate(selected):
            arguments = shlex.split(entry["command"])
            try:
                output_index = arguments.index("-o") + 1
            except ValueError:
                print(f"compile command lacks -o: {entry['file']}", file=sys.stderr)
                return 2
            arguments[output_index] = str(Path(output) / f"{index}.o")
            arguments.insert(output_index - 1, "-fanalyzer")
            result = subprocess.run(
                arguments, cwd=entry["directory"], stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, text=True, check=False,
            )
            if result.returncode or "warning:" in result.stdout:
                failed = True
                print(f"GCC ANALYZER FAILED: {entry['file']}", file=sys.stderr)
                print(result.stdout, file=sys.stderr)
    if failed:
        return 1
    print(f"GCC ANALYZER PASSED files={len(selected)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
