#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
import argparse
import os
import re
import selectors
import signal
import subprocess
import sys
import time
from pathlib import Path


BASE_COMMANDS = [
    "echo busybox-interactive-ok",
    "pwd",
    "cd /tmp",
    "pwd",
    "echo timer-ok > /tmp/ash-msg",
    "cat /tmp/ash-msg",
    "test -f /tmp/ash-msg && echo busybox-test-ok",
    "fsync /tmp/ash-msg; echo busybox-fsync-ok",
    "sync; echo busybox-sync-ok",
    "sleep 1; echo busybox-sleep-ok",
    "cat /etc/luna-release",
    "cat /luna-persist",
    "cat /proc/meminfo",
    "exit",
]

REQUIRED = [
    b"LUNA_ISOLATION_FAULT_OK",
    b"LUNA_ISOLATION_CHANNEL_OK",
    b"LUNA_LKL_CHILD_LINKED",
    b"LUNA_RESOURCE_POOL_OK",
    b"LUNA_CHILD_ALLOCATOR_OK pages=8192",
    b"LUNA_CHILD_ALLOCATOR_RELEASE_OK",
    b"LUNA_SYNC_TLS_OK",
    b"LUNA_THREAD_TIMER_OK",
    b"LUNA_VIRTIO_BLOCK_OK bytes=16777216",
    b"LUNA_HOST_FILE_BACKING_OK bytes=16777216",
    b"LUNA_VIRTIO_NET_OK backend=qemu-virtio-pci",
    b"LUNA_NETWORK_IPV4_OK address=10.0.2.15/24",
    b"LUNA_NETWORK_ASYNC_RX_OK notification=receive-only",
    b"LUNA_NETWORK_IRQ_OK line=",
    b"LUNA_NETWORK_ICMP_OK peer=10.0.2.2",
    b"LUNA_NETWORK_TCP_OK peer=10.0.2.2:18080",
    b"LUNA_NET_QUEUE_STATS received=",
    b"high_water=31 backpressure=1",
    b"empty_fetches=0",
    b"LUNA_NETWORK_PRESSURE_OK burst=64 payload=1200",
    b"LUNA_NET_TX_QUEUE_STATS sent=2048",
    b"LUNA_NET_PEER_TX_COMPLETE unique=2048 count=2048",
    b"LUNA_NETWORK_TX_PRESSURE_OK packets=2048 payload=1200",
    b"LUNA_NETWORK_RECLAIM_OK rounds=100",
    b"LUNA_PERSISTENCE_OK rounds=100",
    b"LUNA_STATIC_USER_OK path=/bin/busybox abi=1",
    b"LUNA_BUSYBOX_HEAP_OK bytes=1048576",
    b"LUNA_SPAWN_WAIT_OK pid=",
    b"LUNA_BUSYBOX_OK command=cat /tmp/x",
    b"LUNA_BUSYBOX_INTERACTIVE_READY prompt=luna-ash#",
    b"LUNA_BUSYBOX_INTERACTIVE_OK status=0 forbidden=0",
    b"LUNA_PHASE2_4_USER_OK",
    b"LUNA_LKL_CHILD_INIT_OK",
    b"LUNA_LKL_CHILD_BOOT_OK",
    b"LUNA_LKL_CHILD_HALT_OK",
    b"LUNA_LKL_CHILD_SHELL_READY",
    b"LUNA_ISOLATION_RESTART_OK",
    b"LUNA_RESTART_STRESS_OK rounds=100",
    b"LUNA_ISOLATION_OK",
    b"busybox-interactive-ok",
    b"busybox-test-ok",
    b"busybox-fsync-ok",
    b"busybox-sync-ok",
    b"busybox-sleep-ok",
    b"/tmp",
    b"timer-ok",
    b"luna Phase 2.3 persistent rootfs",
    b"luna-phase-2.3-persistent",
    b"MemTotal:",
    b"LUNA_SHUTDOWN_OK",
]

FORBIDDEN = [
    b"Caught cap fault",
    b"vm fault",
    b"LKL host panic",
    b"Kernel panic",
    b"host heap exhausted",
    b"invalid host heap free",
    b"corrupt host heap free",
    b"host heap not fully released",
    b"host page map failed",
    b"host page unmap failed",
    b"child heap request failed",
    b"failed to delete child resource cap",
    b"Cannot clear reserved entries mid level",
    b"Untyped Retype",
    b"freeing semaphore with waiters",
    b"semaphore count overflow",
    b"freeing owned mutex",
    b"mutex unlock by non-owner",
    b"mutex owner errors during LKL runtime",
    b"persistent disk prepare failed",
    b"persistent disk finish failed",
    b"failed to leave persistent root",
    b"post-halt disk cleanup failed",
    b"child disk request failed",
    b"child network request failed",
    b"virtio net add failed",
    b"IPv4 configuration failed",
    b"ICMP smoke failed",
    b"TCP smoke failed",
    b"network pressure smoke failed",
    b"network TX pressure smoke failed",
    b"virtio-net IRQ verification failed",
    b"LUNA_NETWORK_IRQ_FALLBACK_OK",
    b"host-file disk I/O setup failed",
    b"ivshmem disk missing",
    b"ivshmem disk PCI configuration invalid",
    b"ivshmem host-file disk mapping failed",
    b"Buffer I/O error",
    b"I/O error while writing superblock",
    b"persistent rootfs pack metadata invalid",
    b"static BusyBox manifest invalid",
    b"BusyBox heap self-test failed",
    b"BusyBox spawn/wait failed",
    b"interactive BusyBox failed",
    b"Error attempting syscall",
]
PROMPT = re.compile(rb"luna-ash# ")


def stop_process(proc: subprocess.Popen[bytes]) -> None:
    if proc.poll() is not None:
        return
    try:
        if proc.stdin:
            proc.stdin.write(b"\x01x")
            proc.stdin.flush()
        proc.wait(timeout=3)
        return
    except (BrokenPipeError, subprocess.TimeoutExpired):
        pass
    os.killpg(proc.pid, signal.SIGTERM)
    try:
        proc.wait(timeout=3)
    except subprocess.TimeoutExpired:
        os.killpg(proc.pid, signal.SIGKILL)
        proc.wait()


def main() -> int:
    parser = argparse.ArgumentParser(description="Drive the luna LKL shell and verify a QEMU boot")
    parser.add_argument("--timeout", type=float, default=480.0)
    parser.add_argument("--disk-image", type=Path)
    parser.add_argument("--reset-disk", action="store_true")
    parser.add_argument("--persist-write")
    parser.add_argument("--persist-read")
    args = parser.parse_args()
    for value in (args.persist_write, args.persist_read):
        if value and not re.fullmatch(r"[A-Za-z0-9._-]{1,64}", value):
            parser.error("persistence marker must use 1-64 safe characters")

    commands = list(BASE_COMMANDS)
    required = list(REQUIRED)
    if args.persist_write:
        commands.insert(
            -1,
            "echo " + args.persist_write +
            " > /qemu-power-persist; fsync /qemu-power-persist; "
            "sync; cat /qemu-power-persist",
        )
        required.append(args.persist_write.encode())
    if args.persist_read:
        commands.insert(-1, "cat /qemu-power-persist")
        required.append(args.persist_read.encode())

    root = Path(__file__).resolve().parents[1]
    env = os.environ.copy()
    if args.disk_image:
        env["LUNA_DISK_IMAGE"] = str(args.disk_image.resolve())
    if args.reset_disk:
        env["LUNA_DISK_RESET"] = "1"
    proc = subprocess.Popen(
        [str(root / "run.sh"), "--run-only", "--no-timeout"],
        cwd=root,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        bufsize=0,
        start_new_session=True,
        env=env,
    )
    assert proc.stdout is not None and proc.stdin is not None

    selector = selectors.DefaultSelector()
    selector.register(proc.stdout, selectors.EVENT_READ)
    output = bytearray()
    prompt_scan = 0
    command_index = 0
    deadline = time.monotonic() + args.timeout
    success_marker = False

    try:
        while time.monotonic() < deadline:
            events = selector.select(timeout=0.25)
            for key, _ in events:
                chunk = os.read(key.fileobj.fileno(), 4096)
                if not chunk:
                    break
                output.extend(chunk)
                sys.stdout.buffer.write(chunk)
                sys.stdout.buffer.flush()

                while command_index < len(commands):
                    match = PROMPT.search(output, prompt_scan)
                    if not match:
                        break
                    prompt_scan = match.end()
                    command = commands[command_index]
                    proc.stdin.write(command.encode() + b"\n")
                    proc.stdin.flush()
                    command_index += 1

                if b"LUNA_SHUTDOWN_OK" in output:
                    success_marker = True
                    break
            if success_marker or proc.poll() is not None:
                break
    finally:
        selector.close()
        stop_process(proc)

    data = bytes(output)
    errors = []
    if not success_marker:
        errors.append("shutdown success marker was not observed")
    if command_index != len(commands):
        errors.append(f"only sent {command_index}/{len(commands)} shell commands")
    for marker in required:
        if marker not in data:
            errors.append(f"missing output: {marker.decode(errors='replace')}")
    for marker in FORBIDDEN:
        if marker in data:
            errors.append(f"forbidden output: {marker.decode(errors='replace')}")
    irq = re.search(
        rb"LUNA_NETWORK_IRQ_OK line=(\d+) interrupts=(\d+) "
        rb"kick_polls=(\d+) fallback_polls=(\d+)", data
    )
    if not irq:
        errors.append("virtio-net IRQ statistics were not reported")
    else:
        line, interrupts, kick_polls, fallback_polls = map(int, irq.groups())
        if not 1 <= line < 24:
            errors.append(f"virtio-net IRQ line out of range: {line}")
        if interrupts <= 0:
            errors.append("virtio-net did not deliver any interrupts")
        if kick_polls > 4096:
            errors.append(f"kick-driven poll count unexpectedly high: {kick_polls}")
        if fallback_polls != 0:
            errors.append(f"unexpected continuous polling: {fallback_polls}")

    if errors:
        print("\nSMOKE TEST FAILED", file=sys.stderr)
        for error in errors:
            print(f"- {error}", file=sys.stderr)
        return 1
    print("\nSMOKE TEST PASSED")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
