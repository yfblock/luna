#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
[[ -x "$ROOT/deps/build_lkl/simulate" ]] || {
    echo "build missing; run ./run.sh --build-only first" >&2
    exit 1
}
STATE_DIR="$(mktemp -d)"
STATE_IMAGE="$STATE_DIR/luna-rootfs.ext4"
cleanup() {
    rm -rf "$STATE_DIR"
}
trap cleanup EXIT

if [[ "${1:-}" == "--cross-qemu" ]]; then
    shift
    MARKER="luna-cross-qemu-persist-ok"
    python3 "$ROOT/tools/qemu_smoke.py" "$@" \
        --disk-image "$STATE_IMAGE" --reset-disk \
        --persist-write "$MARKER"
    python3 "$ROOT/tools/qemu_smoke.py" "$@" \
        --disk-image "$STATE_IMAGE" --persist-read "$MARKER"
else
    python3 "$ROOT/tools/qemu_smoke.py" "$@" \
        --disk-image "$STATE_IMAGE" --reset-disk
fi
e2fsck -fn "$STATE_IMAGE" >/dev/null
echo "HOST FILE FSCK PASSED"
