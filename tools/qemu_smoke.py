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


LONG_UNKNOWN = "x" * 240

COMMANDS = [
    LONG_UNKNOWN,
    "time",
    "sleep 100",
    "cat /etc/luna-release",
    "cat /luna-persist",
    "mkdir /smoke",
    "write /smoke/msg timer-ok",
    "cat /smoke/msg",
    "sync",
    "free",
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
    b"LUNA_PERSISTENCE_OK rounds=100",
    b"LUNA_LKL_CHILD_INIT_OK",
    b"LUNA_LKL_CHILD_BOOT_OK",
    b"LUNA_LKL_CHILD_HALT_OK",
    b"LUNA_LKL_CHILD_SHELL_READY",
    b"LUNA_ISOLATION_RESTART_OK",
    b"LUNA_RESTART_STRESS_OK rounds=100",
    b"LUNA_ISOLATION_OK",
    b"unknown: " + b"x" * 200,
    b"slept 100 ms",
    b"timer-ok",
    b"luna Phase 2.3 persistent rootfs",
    b"luna-phase-2.3-persistent",
    b"synced",
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
    b"persistent rootfs pack metadata invalid",
]
PROMPT = re.compile(rb"lkl:[^\r\n]*# ")


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
    args = parser.parse_args()

    root = Path(__file__).resolve().parents[1]
    proc = subprocess.Popen(
        [str(root / "run.sh"), "--run-only", "--no-timeout"],
        cwd=root,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        bufsize=0,
        start_new_session=True,
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

                while command_index < len(COMMANDS):
                    match = PROMPT.search(output, prompt_scan)
                    if not match:
                        break
                    prompt_scan = match.end()
                    command = COMMANDS[command_index]
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
    if command_index != len(COMMANDS):
        errors.append(f"only sent {command_index}/{len(COMMANDS)} shell commands")
    for marker in REQUIRED:
        if marker not in data:
            errors.append(f"missing output: {marker.decode(errors='replace')}")
    for marker in FORBIDDEN:
        if marker in data:
            errors.append(f"forbidden output: {marker.decode(errors='replace')}")
    monotonic = re.search(rb"monotonic_ns=(\d+)", data)
    if not monotonic or int(monotonic.group(1)) <= 0:
        errors.append("monotonic clock did not return a positive value")
    elapsed = re.search(rb"slept 100 ms \(elapsed (\d+) ns\)", data)
    if not elapsed:
        errors.append("sleep elapsed time was not reported")
    elif not 80_000_000 <= int(elapsed.group(1)) <= 5_000_000_000:
        errors.append(f"100ms sleep elapsed value out of range: {elapsed.group(1).decode()} ns")

    if errors:
        print("\nSMOKE TEST FAILED", file=sys.stderr)
        for error in errors:
            print(f"- {error}", file=sys.stderr)
        return 1
    print("\nSMOKE TEST PASSED")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
