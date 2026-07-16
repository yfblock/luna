#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DO_BUILD=0
DO_SMOKE=0
DO_ANALYZE=0

usage() {
    echo "usage: $0 [--build] [--analyze] [--smoke]"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build) DO_BUILD=1;;
        --analyze) DO_BUILD=1; DO_ANALYZE=1;;
        --smoke) DO_BUILD=1; DO_SMOKE=1;;
        -h|--help) usage; exit 0;;
        *) usage >&2; exit 2;;
    esac
    shift
done

cd "$ROOT"
./setup-deps.sh --check-only

while IFS= read -r script; do
    bash -n "$script"
done < <(find . -path ./deps -prune -o -type f -name '*.sh' -print)

python3 -m py_compile tools/*.py
python3 -m unittest discover -s tools/tests -p 'test_*.py'
git diff --check

if command -v shellcheck >/dev/null; then
    mapfile -t shell_scripts < <(
        find . -path ./deps -prune -o -type f -name '*.sh' -print
    )
    shellcheck "${shell_scripts[@]}"
else
    echo "quality gate: shellcheck not installed; syntax checks still passed"
fi

if [[ $DO_BUILD == 1 ]]; then
    ./run.sh --build-only
fi
if [[ $DO_ANALYZE == 1 ]]; then
    ./tools/gcc-analyzer.py
fi
if [[ $DO_SMOKE == 1 ]]; then
    ./tools/smoke-test.sh --cross-qemu --timeout 600
    ./tools/benchmark.py --timeout 600 --check-baseline
fi

echo "QUALITY GATE PASSED"
