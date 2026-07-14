#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# run.sh — 一键构建并启动 LKL-on-seL4（Phase 1.1）。
#
# 用法：
#   ./run.sh                 # 构建 + QEMU 启动（默认 60s 超时）
#   ./run.sh --build-only    # 只构建
#   ./run.sh --run-only      # 只启动（假定已构建）
#   ./run.sh --timeout 30    # 自定义 QEMU 超时秒数
#   ./run.sh --no-timeout    # 不超时（手动 Ctrl-A X 退出）
#
# 前置：deps/ 已就绪（seL4 manifest 源树 + lkl-linux）；~/bin/xmllint stub 与 python 依赖已装。
# 详见 README.md / PHASE1-RESULTS.md。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEPS="$ROOT/deps"
BUILD="$DEPS/build_lkl"
LKL_LINUX="$DEPS/lkl-linux"
LKL_LIBA="$LKL_LINUX/tools/lkl/liblkl.a"
KERNEL_OBJ="$ROOT/build-artifacts/lkl-kernel.o"
QEMU="$(command -v qemu-system-x86_64 || true)"

DO_BUILD=1
DO_RUN=1
TIMEOUT=60
while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-only) DO_RUN=0;;
        --run-only)   DO_BUILD=0;;
        --no-timeout) TIMEOUT=0;;
        --timeout)
            [[ $# -ge 2 && "$2" =~ ^[0-9]+$ ]] || { echo "--timeout 需要非负整数秒数" >&2; exit 2; }
            TIMEOUT="$2"; shift;;
        *) echo "unknown arg: $1" >&2; exit 2;;
    esac
    shift
done

export PATH="$HOME/bin:$PATH"

step() { printf '\n\033[1;34m==>\033[0m %s\n' "$*"; }

if [[ $DO_BUILD == 1 ]]; then
    step "sanity checks"
    [[ -d "$LKL_LINUX/tools/lkl" ]] || { echo "缺 lkl-linux：$LKL_LINUX" >&2; exit 1; }
    command -v xmllint >/dev/null || { echo "缺 xmllint stub（见 README）" >&2; exit 1; }
    git -C "$LKL_LINUX" apply --check --reverse "$ROOT/patches/lkl-tty.patch" >/dev/null 2>&1 || {
        echo "LKL tty 补丁未应用；请运行 ./setup-deps.sh" >&2
        exit 1
    }

    step "build liblkl.a"
    make -C "$LKL_LINUX/tools/lkl" -j"$(nproc)" >/dev/null

    step "extract clean kernel object lkl.o"
    mkdir -p "$ROOT/build-artifacts"
    ( cd "$ROOT/build-artifacts" && ar x "$LKL_LIBA" lkl.o && mv lkl.o lkl-kernel.o )

    step "configure seL4 root task (with LKL)"
    export LUNA_SETTINGS="$DEPS/lkl_settings.cmake"
    export LKL_LINUX_DIR="$LKL_LINUX"
    export LKL_KERNEL_OBJ="$KERNEL_OBJ"
    cmake -G Ninja -B "$BUILD" -S "$ROOT/apps/lkl-root-task" >/dev/null

    step "ninja build"
    ninja -C "$BUILD"
fi

if [[ $DO_RUN == 1 ]]; then
    step "boot on QEMU  (qemu: $QEMU)"
    [[ -x "$QEMU" ]] || { echo "缺 qemu-system-x86_64" >&2; exit 1; }
    [[ -x "$BUILD/simulate" ]] || { echo "未构建：先 ./run.sh --build-only" >&2; exit 1; }
    cd "$BUILD"
    # 直接透传，不过 sed：sed 按行处理会吞掉逐字符回显（交互 shell 必须）。
    # 启动时 seL4 的 ANSI（ESC[?7l ESC[2J）会清一次屏，可接受。
    if [[ $TIMEOUT -gt 0 ]]; then
        timeout "$TIMEOUT" ./simulate -b "$QEMU"
    else
        ./simulate -b "$QEMU"
    fi
fi
