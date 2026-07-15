#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEPS="$ROOT/deps"
PATCH="$ROOT/patches/lkl-tty.patch"
SEL4_MANIFEST_URL="https://github.com/seL4/seL4-tutorials-manifest.git"
SEL4_MANIFEST_REV="cf8e88fbd953fedbf65ddee6eac6ccabb4a36df3"
LKL_URL="https://github.com/lkl/linux.git"
LKL_REV="6bce81422a8a420389c1b100d7e0473e066638b6"
CHECK_ONLY=0

usage() {
    echo "usage: $0 [--check-only]"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --check-only) CHECK_ONLY=1;;
        -h|--help) usage; exit 0;;
        *) usage >&2; exit 2;;
    esac
    shift
done

require_cmd() {
    command -v "$1" >/dev/null || { echo "missing required command: $1" >&2; return 1; }
}

check_python_modules() {
    python3 - <<'PY'
import importlib.util
import sys

modules = ["sh", "aenum", "elftools", "sortedcontainers", "future", "jinja2"]
missing = [name for name in modules if importlib.util.find_spec(name) is None]
if missing:
    print("missing Python modules: " + " ".join(missing), file=sys.stderr)
    print("install with: python3 -m pip install --user sh aenum pyelftools sortedcontainers future jinja2", file=sys.stderr)
    raise SystemExit(1)
PY
}

check_tools() {
    local failed=0
    for cmd in git repo cmake ninja make ar python3 xmllint qemu-system-x86_64 \
               mke2fs e2fsck; do
        require_cmd "$cmd" || failed=1
    done
    check_python_modules || failed=1
    [[ $failed == 0 ]]
}

check_sel4() {
    [[ -d "$DEPS/.repo" ]] || { echo "seL4 repo checkout missing: $DEPS/.repo" >&2; return 1; }
    local actual
    actual="$(git -C "$DEPS/.repo/manifests" rev-parse HEAD)"
    [[ "$actual" == "$SEL4_MANIFEST_REV" ]] || {
        echo "seL4 manifest mismatch: expected $SEL4_MANIFEST_REV, got $actual" >&2
        return 1
    }
    (cd "$DEPS" && repo status >/dev/null)
}

check_lkl() {
    [[ -d "$DEPS/lkl-linux/.git" ]] || { echo "LKL checkout missing" >&2; return 1; }
    local actual
    actual="$(git -C "$DEPS/lkl-linux" rev-parse HEAD)"
    [[ "$actual" == "$LKL_REV" ]] || {
        echo "LKL revision mismatch: expected $LKL_REV, got $actual" >&2
        return 1
    }
    git -C "$DEPS/lkl-linux" apply --check --reverse "$PATCH" >/dev/null 2>&1 || {
        echo "LKL tty patch is not applied cleanly" >&2
        return 1
    }
}

if [[ $CHECK_ONLY == 1 ]]; then
    check_tools
    check_sel4
    check_lkl
    echo "dependency check passed"
    exit 0
fi

check_tools
mkdir -p "$DEPS"

(cd "$DEPS" && repo init -u "$SEL4_MANIFEST_URL" -b "$SEL4_MANIFEST_REV" -m default.xml)
(cd "$DEPS" && repo sync -c --no-clone-bundle --no-tags -j"$(nproc)")

if [[ ! -d "$DEPS/lkl-linux/.git" ]]; then
    git clone "$LKL_URL" "$DEPS/lkl-linux"
fi

actual="$(git -C "$DEPS/lkl-linux" rev-parse HEAD)"
if [[ "$actual" != "$LKL_REV" ]]; then
    [[ -z "$(git -C "$DEPS/lkl-linux" status --porcelain)" ]] || {
        echo "refusing to change a dirty LKL checkout" >&2
        exit 1
    }
    git -C "$DEPS/lkl-linux" fetch origin "$LKL_REV"
    git -C "$DEPS/lkl-linux" checkout --detach "$LKL_REV"
fi

if git -C "$DEPS/lkl-linux" apply --check --reverse "$PATCH" >/dev/null 2>&1; then
    echo "LKL tty patch already applied"
elif git -C "$DEPS/lkl-linux" apply --check "$PATCH" >/dev/null 2>&1; then
    git -C "$DEPS/lkl-linux" apply "$PATCH"
    echo "applied LKL tty patch"
else
    echo "LKL tty patch cannot be applied to $LKL_REV" >&2
    exit 1
fi

check_sel4
check_lkl
echo "dependencies ready"
