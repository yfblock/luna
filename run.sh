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
#   LUNA_QEMU_MEM=1G ./run.sh # 覆盖 QEMU 内存（默认 512M）
#
# 前置：deps/ 已就绪（seL4 manifest 源树 + lkl-linux）；~/bin/xmllint stub 与 python 依赖已装。
# 详见 README.md / PHASE1-RESULTS.md。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEPS="$ROOT/deps"
BUILD="$DEPS/build_lkl"
LKL_LINUX="$DEPS/lkl-linux"
BUSYBOX="$DEPS/busybox"
LKL_LIBA="$LKL_LINUX/tools/lkl/liblkl.a"
KERNEL_OBJ="$ROOT/build-artifacts/lkl-kernel.o"
QEMU="$(command -v qemu-system-x86_64 || true)"
QEMU_MEM="${LUNA_QEMU_MEM:-512M}"
NET_PEER_PORT="${LUNA_NET_PEER_PORT:-18081}"
NET_QEMU_PORT="${LUNA_NET_QEMU_PORT:-18082}"
DISK_IMAGE="${LUNA_DISK_IMAGE:-$BUILD/luna-rootfs-state.ext4}"
DISK_RESET="${LUNA_DISK_RESET:-0}"
DISK_SIZE=16777216

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
    [[ -d "$BUSYBOX/.git" ]] || { echo "缺 BusyBox：$BUSYBOX" >&2; exit 1; }
    command -v xmllint >/dev/null || { echo "缺 xmllint stub（见 README）" >&2; exit 1; }
    git -C "$LKL_LINUX" apply --check --reverse "$ROOT/patches/lkl-tty.patch" >/dev/null 2>&1 || {
        echo "LKL tty 补丁未应用；请运行 ./setup-deps.sh" >&2
        exit 1
    }
    git -C "$BUSYBOX" apply --unidiff-zero --check --reverse "$ROOT/patches/busybox-nofork-cat.patch" >/dev/null 2>&1 || {
        echo "BusyBox Luna 补丁未应用；请运行 ./setup-deps.sh" >&2
        exit 1
    }

    step "build LKL kernel archive"
    # luna only extracts lkl.o. Building the default tools/lkl target also links
    # optional tests and hijack libraries, adding unrelated CI dependencies.
    # On a clean checkout, generate Makefile.conf first; LKL defines the archive
    # rule using its absolute OUTPUT path, so a relative liblkl.a goal is not
    # recognized during that first invocation.
    make -C "$LKL_LINUX/tools/lkl" conf
    make -C "$LKL_LINUX/tools/lkl" -j"$(nproc)" \
        "$LKL_LINUX/tools/lkl/liblkl.a"

    step "extract clean kernel object lkl.o"
    mkdir -p "$ROOT/build-artifacts"
    ( cd "$ROOT/build-artifacts" && ar x "$LKL_LIBA" lkl.o && mv lkl.o lkl-kernel.o )

    step "configure root manager and isolated LKL task"
    export LUNA_SETTINGS="$DEPS/lkl_settings.cmake"
    export LKL_LINUX_DIR="$LKL_LINUX"
    export LKL_KERNEL_OBJ="$KERNEL_OBJ"
    export BUSYBOX_DIR="$BUSYBOX"
    cmake -G Ninja -B "$BUILD" -S "$ROOT/apps/lkl-root-task" >/dev/null

    step "ninja build"
    ninja -C "$BUILD"
fi

if [[ $DO_RUN == 1 ]]; then
    step "boot on QEMU  (qemu: $QEMU, memory: $QEMU_MEM)"
    [[ -x "$QEMU" ]] || { echo "缺 qemu-system-x86_64" >&2; exit 1; }
    [[ -x "$BUILD/simulate" ]] || { echo "未构建：先 ./run.sh --build-only" >&2; exit 1; }
    "$QEMU" -netdev help 2>&1 | grep -q '^socket$' || {
        echo "QEMU 缺少 Phase 2.5 所需的 socket netdev backend" >&2
        exit 1
    }
    "$QEMU" -device help 2>&1 | grep -q 'virtio-net-pci' || {
        echo "QEMU 缺少 Phase 2.5 所需的 virtio-net-pci device" >&2
        exit 1
    }
    "$QEMU" -device help 2>&1 | grep -q 'ivshmem-plain' || {
        echo "QEMU 缺少 host-file ext4 所需的 ivshmem-plain device" >&2
        exit 1
    }
    [[ -f "$BUILD/luna-rootfs.ext4" ]] || {
        echo "缺少 rootfs seed image：先 ./run.sh --build-only" >&2
        exit 1
    }
    if [[ "$DISK_RESET" == 1 || ! -f "$DISK_IMAGE" ]]; then
        mkdir -p "$(dirname "$DISK_IMAGE")"
        cp --reflink=auto "$BUILD/luna-rootfs.ext4" "$DISK_IMAGE"
    fi
    [[ "$(stat -c '%s' "$DISK_IMAGE")" == "$DISK_SIZE" ]] || {
        echo "ext4 backing 大小必须为 $DISK_SIZE bytes：$DISK_IMAGE" >&2
        exit 1
    }
    NET_READY="$(mktemp)"
    rm -f "$NET_READY"
    python3 "$ROOT/tools/net-peer.py" \
        --listen-port "$NET_PEER_PORT" --qemu-port "$NET_QEMU_PORT" \
        --ready-file "$NET_READY" &
    NET_PEER_PID=$!
    cleanup_net_peer() {
        kill "$NET_PEER_PID" >/dev/null 2>&1 || true
        wait "$NET_PEER_PID" >/dev/null 2>&1 || true
        rm -f "$NET_READY"
    }
    trap cleanup_net_peer EXIT
    for _ in $(seq 1 100); do
        [[ -s "$NET_READY" ]] && break
        kill -0 "$NET_PEER_PID" >/dev/null 2>&1 || {
            echo "Phase 2.5 主机网络对端启动失败" >&2
            exit 1
        }
        sleep 0.01
    done
    [[ -s "$NET_READY" ]] || { echo "Phase 2.5 主机网络对端未就绪" >&2; exit 1; }
    QEMU_NET_ARGS="-nic none -netdev socket,id=lunanet,udp=127.0.0.1:${NET_PEER_PORT},localaddr=127.0.0.1:${NET_QEMU_PORT} -device virtio-net-pci,disable-modern=on,netdev=lunanet,mac=52:54:00:12:34:56,addr=05.0"
    printf -v DISK_IMAGE_Q '%q' "$DISK_IMAGE"
    QEMU_DISK_ARGS="-object memory-backend-file,id=lunadisk,mem-path=${DISK_IMAGE_Q},size=${DISK_SIZE},share=on -device ivshmem-plain,memdev=lunadisk,addr=06.0"
    QEMU_EXTRA_ARGS="$QEMU_NET_ARGS $QEMU_DISK_ARGS"
    cd "$BUILD"
    # 直接透传，不过 sed：sed 按行处理会吞掉逐字符回显（交互 shell 必须）。
    # 启动时 seL4 的 ANSI（ESC[?7l ESC[2J）会清一次屏，可接受。
    if [[ $TIMEOUT -gt 0 ]]; then
        timeout "$TIMEOUT" ./simulate -b "$QEMU" -m "$QEMU_MEM" \
            --extra-qemu-args="$QEMU_EXTRA_ARGS"
    else
        ./simulate -b "$QEMU" -m "$QEMU_MEM" \
            --extra-qemu-args="$QEMU_EXTRA_ARGS"
    fi
fi
