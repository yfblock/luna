#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
[[ -x "$ROOT/deps/build_lkl/simulate" ]] || {
    echo "build missing; run ./run.sh --build-only first" >&2
    exit 1
}
exec python3 "$ROOT/tools/qemu_smoke.py" "$@"
