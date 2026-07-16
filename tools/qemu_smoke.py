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
    "printf 'stdio-ok:%d\\n' 7 > /tmp/stdio-msg",
    "cat /tmp/stdio-msg",
    "test -f /tmp/ash-msg && echo busybox-test-ok",
    "fsync /tmp/ash-msg; echo busybox-fsync-ok",
    "sync; echo busybox-sync-ok",
    "sleep 1; echo busybox-sleep-ok",
    "mkdir -m 700 -p /tmp/bb-dir/nested && echo busybox-mkdir-ok",
    "echo payload > /tmp/bb-dir/nested/file",
    "truncate -s 3 /tmp/bb-dir/nested/file",
    "cat /tmp/bb-dir/nested/file; echo",
    "truncate -s 0 /tmp/bb-dir/nested/file; "
    "test ! -s /tmp/bb-dir/nested/file && echo busybox-truncate-ok",
    "basename /tmp/bb-dir/nested/file",
    "dirname /tmp/bb-dir/nested/file",
    "export LUNA_APPLET_ENV=env-value",
    "printenv LUNA_APPLET_ENV",
    "uname -s",
    "touch /tmp/bb-touch && test -f /tmp/bb-touch && echo busybox-touch-ok",
    "echo runtime-target > /run/runtime-target",
    "ln -s runtime-target /run/runtime-link && echo busybox-ln-ok",
    "readlink /run/runtime-link",
    "realpath /run/runtime-link",
    "printf 'pipeline-ok\\n' | cat",
    "echo pipeline-three-ok | cat | cat",
    "true | false || echo pipeline-status-ok",
    "printf 'alpha\\nbeta\\n' > /tmp/stream-data",
    "head -n 1 /tmp/stream-data",
    "wc -l < /tmp/stream-data",
    "printf 'left:right\\n' > /tmp/cut-data",
    "cut -d: -f2 /tmp/cut-data",
    "printf 'same\\nsame\\nnext\\n' > /tmp/uniq-data",
    "uniq /tmp/uniq-data",
    "printf 'same\\n' > /run/cmp-a",
    "cp /run/cmp-a /run/cmp-b && cmp /run/cmp-a /run/cmp-b && "
    "echo busybox-cmp-cp-ok",
    "printf 'z\\na\\nm\\n' > /run/sort-data",
    "sort /run/sort-data",
    "grep '^a$' /run/sort-data && echo busybox-grep-ok",
    "printf 'tee-ok\\n' | tee /run/tee-data",
    "cat /run/tee-data",
    "dd if=/run/cmp-a of=/run/dd-copy bs=5 count=1 2>/dev/null && "
    "cmp /run/cmp-a /run/dd-copy && echo busybox-dd-ok",
    "mkdir -p /run/find-root/sub && cp /run/cmp-a /run/find-root/sub/target",
    "find /run/find-root -name target -print",
    "ls -1 /run/find-root/sub",
    "mv /run/cmp-b /run/moved && cmp /run/cmp-a /run/moved && "
    "echo busybox-mv-ok",
    "no-such-static-command >/dev/null 2>&1 || echo static-missing-ok",
    "echo rejected | no-such-static-command >/dev/null 2>&1 || "
    "echo static-pipeline-reject-ok",
    "true | true | true | true | true || echo static-pipeline-limit-ok",
    "sleep 2 & sleep 2 & sleep 2 & sleep 2 & echo overflow & "
    "test $? -ne 0 && echo static-worker-limit-ok; wait; "
    "echo concurrent-background-ok",
    "echo background-ok & wait; echo background-wait-ok",
    "unlink /run/runtime-link && unlink /run/runtime-target",
    "mkdir -p /run/bb-remove/nested && "
    "echo remove-me > /run/bb-remove/nested/file && "
    "unlink /run/bb-remove/nested/file && "
    "rmdir /run/bb-remove/nested /run/bb-remove && "
    "test ! -e /run/bb-remove && echo busybox-applets-ok",
    "cat /etc/luna-release",
    "cat /luna-persist",
    "cat /proc/meminfo",
    "exit",
]

REQUIRED = [
    b"LUNA_ISOLATION_FAULT_OK",
    b"LUNA_ISOLATION_CHANNEL_OK",
    b"LUNA_CHILD_START_ROLLBACK_OK stages=7",
    b"LUNA_LKL_CHILD_LINKED",
    b"LUNA_RESOURCE_POOL_OK",
    b"LUNA_CHILD_ALLOCATOR_OK pages=8192",
    b"LUNA_CHILD_ALLOCATOR_RELEASE_OK",
    b"LUNA_RESOURCE_PEAK_OK managed_frame_pages=8225 child_tcbs=50 "
    b"child_notifications=146 heap_pages=8192 disk_pages=16 net_pages=17",
    b"LUNA_SYNC_TLS_OK",
    b"LUNA_THREAD_TIMER_OK",
    b"LUNA_VIRTIO_BLOCK_OK bytes=16777216",
    b"LUNA_HOST_FILE_BACKING_OK bytes=16777216",
    b"LUNA_VIRTIO_NET_OK backend=qemu-virtio-pci",
    b"LUNA_NETWORK_IPV4_OK address=10.0.2.15/24",
    b"LUNA_NETWORK_ASYNC_RX_OK notification=receive-only",
    b"LUNA_ROOT_ALLOCATOR_POOL_OK bytes=8388608",
    b"LUNA_NETWORK_IRQ_OK line=",
    b"LUNA_NET_MANAGER_COUNTERS tx_ipc=",
    b"LUNA_NET_BATCH_COUNTERS tx_ipc=",
    b"LUNA_DISK_BATCH_COUNTERS ipc=",
    b"LUNA_DISK_MANAGER_COUNTERS ipc=",
    b"LUNA_CHILD_RESOURCE_HIGH_WATER threads=",
    b"LUNA_NETWORK_ICMP_OK peer=10.0.2.2",
    b"LUNA_NETWORK_TCP_OK peer=10.0.2.2:18080",
    b"LUNA_NET_QUEUE_STATS received=",
    b"LUNA_NET_RX_THROUGHPUT_OK packets=24",
    b"high_water=31 backpressure=1",
    b"empty_fetches=0",
    b"LUNA_NETWORK_PRESSURE_OK burst=64 payload=1200",
    b"LUNA_NET_TX_QUEUE_STATS sent=2048",
    b"LUNA_NET_PEER_TX_COMPLETE unique=2048 count=2048",
    b"LUNA_NETWORK_TX_PRESSURE_OK packets=2048 payload=1200",
    b"LUNA_NETWORK_RECLAIM_OK rounds=100",
    b"LUNA_LIFECYCLE_BENCHMARK_OK rounds=100",
    b"LUNA_PERSISTENCE_OK rounds=100",
    b"LUNA_STATIC_USER_OK path=/bin/busybox abi=1",
    b"LUNA_BUSYBOX_HEAP_OK bytes=1048576",
    b"LUNA_BLOCK_BENCHMARK_OK bytes=1048576",
    b"LUNA_PIPELINE_BENCHMARK_OK bytes=1048576",
    b"LUNA_USER_RESOURCE_PEAK_OK heap_bytes=",
    b"LUNA_SPAWN_WAIT_OK pid=",
    b"LUNA_BUSYBOX_OK command=cat /tmp/x",
    b"LUNA_BUSYBOX_INTERACTIVE_READY prompt=luna-ash#",
    b"LUNA_BUSYBOX_INTERACTIVE_OK status=0 forbidden=0",
    b"LUNA_STATIC_RUNTIME_OK workers=30 pipelines=4 background=5",
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
    b"stdio-ok:7",
    b"luna Phase 2.3 persistent rootfs",
    b"luna-phase-2.3-persistent",
    b"MemTotal:",
    b"LUNA_SHUTDOWN_OK",
]

REQUIRED_LINES = [
    b"busybox-mkdir-ok",
    b"pay",
    b"busybox-truncate-ok",
    b"file",
    b"/tmp/bb-dir/nested",
    b"env-value",
    b"Linux",
    b"busybox-touch-ok",
    b"busybox-ln-ok",
    b"runtime-target",
    b"/run/runtime-target",
    b"pipeline-ok",
    b"pipeline-three-ok",
    b"pipeline-status-ok",
    b"alpha",
    b"2",
    b"right",
    b"same",
    b"next",
    b"busybox-cmp-cp-ok",
    b"a",
    b"m",
    b"z",
    b"busybox-grep-ok",
    b"tee-ok",
    b"busybox-dd-ok",
    b"/run/find-root/sub/target",
    b"target",
    b"busybox-mv-ok",
    b"static-missing-ok",
    b"static-pipeline-reject-ok",
    b"static-pipeline-limit-ok",
    b"static-worker-limit-ok",
    b"concurrent-background-ok",
    b"background-ok",
    b"background-wait-ok",
    b"busybox-applets-ok",
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
    b"child start rollback failed",
    b"child start rollback audit failed",
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
    b"block benchmark failed",
    b"pipeline benchmark failed",
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
    parser.add_argument(
        "--net-backend",
        choices=("socket", "slirp", "passt", "tap"),
        default=os.environ.get("LUNA_NET_BACKEND", "socket"),
    )
    args = parser.parse_args()
    for value in (args.persist_write, args.persist_read):
        if value and not re.fullmatch(r"[A-Za-z0-9._-]{1,64}", value):
            parser.error("persistence marker must use 1-64 safe characters")

    commands = list(BASE_COMMANDS)
    required = list(REQUIRED)
    icmp_unavailable = b"LUNA_NETWORK_ICMP_UNAVAILABLE"
    required.append(f"LUNA_QEMU_NET_BACKEND backend={args.net_backend}".encode())
    if args.net_backend == "passt":
        required.remove(b"LUNA_NETWORK_ICMP_OK peer=10.0.2.2")
        required.append(b"LUNA_NETWORK_ICMP_UNAVAILABLE peer=10.0.2.2")
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
    env["LUNA_NET_BACKEND"] = args.net_backend
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
    early_failure = False

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
                if any(marker in output for marker in FORBIDDEN):
                    early_failure = True
                    break
            if success_marker or early_failure or proc.poll() is not None:
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
    output_lines = {line.strip() for line in data.splitlines()}
    for marker in REQUIRED_LINES:
        if marker not in output_lines:
            errors.append(f"missing output line: {marker.decode(errors='replace')}")
    for marker in FORBIDDEN:
        if marker in data:
            errors.append(f"forbidden output: {marker.decode(errors='replace')}")
    if args.net_backend != "passt" and icmp_unavailable in data:
        errors.append("forbidden output: LUNA_NETWORK_ICMP_UNAVAILABLE")
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

    pipeline_bench = re.search(
        rb"LUNA_PIPELINE_BENCHMARK_OK bytes=(\d+) elapsed_ns=(\d+) "
        rb"bytes_per_sec=(\d+)", data
    )
    if not pipeline_bench:
        errors.append("pipeline benchmark statistics were not reported")
    else:
        size, elapsed, rate = map(int, pipeline_bench.groups())
        if size != 1048576 or elapsed <= 0 or rate <= 0:
            errors.append("pipeline benchmark statistics are invalid")
    pipeline_detail = re.search(
        rb"LUNA_PIPELINE_BENCHMARK_OK .* samples=(\d+) p50_ns=(\d+) "
        rb"p95_ns=(\d+) p99_ns=(\d+) read_calls=(\d+) "
        rb"read_bytes=(\d+) write_calls=(\d+) write_bytes=(\d+)", data
    )
    pipeline_samples = [int(value) for value in re.findall(
        rb"LUNA_PIPELINE_SAMPLE ns=(\d+)", data
    )]
    if not pipeline_detail:
        errors.append("pipeline benchmark detail was not reported")
    else:
        values = list(map(int, pipeline_detail.groups()))
        samples, p50, p95, p99, read_calls, read_bytes, write_calls, write_bytes = values
        if samples != 7 or len(pipeline_samples) != samples or any(
            value <= 0 for value in (p50, p95, p99, read_calls, read_bytes,
                                     write_calls, write_bytes)
        ) or not p50 <= p95 <= p99:
            errors.append("pipeline benchmark detail is invalid")

    block_bench = re.search(
        rb"LUNA_BLOCK_BENCHMARK_OK bytes=(\d+) seq_write_ns=(\d+) "
        rb"cold_read_ns=(\d+) hot_read_ns=(\d+) random_ops=(\d+) "
        rb"random_write_ns=(\d+) random_read_ns=(\d+)", data
    )
    if not block_bench:
        errors.append("block benchmark statistics were not reported")
    else:
        values = list(map(int, block_bench.groups()))
        if values[0] != 1048576 or values[4] != 256 or any(
            value <= 0 for value in values[1:4] + values[5:]
        ):
            errors.append("block benchmark statistics are invalid")

    rx_throughput = re.search(
        rb"LUNA_NET_RX_THROUGHPUT_OK packets=(\d+) bytes=(\d+) "
        rb"samples=(\d+) p50_ns=(\d+) p95_ns=(\d+) p99_ns=(\d+)", data
    )
    rx_samples = [int(value) for value in re.findall(
        rb"LUNA_NET_RX_THROUGHPUT_SAMPLE ns=(\d+)", data
    )]
    if not rx_throughput:
        errors.append("pure RX throughput benchmark was not reported")
    else:
        packets, size, samples, p50, p95, p99 = map(
            int, rx_throughput.groups()
        )
        if packets != 24 or size != 28800 or samples != 7 or \
                len(rx_samples) != samples or not 0 < p50 <= p95 <= p99:
            errors.append("pure RX throughput benchmark is invalid")

    net_counters = re.search(
        rb"LUNA_NET_MANAGER_COUNTERS tx_ipc=(\d+) tx_batches=(\d+) "
        rb"tx_packets=(\d+) tx_copies=(\d+) tx_kicks=(\d+) "
        rb"rx_ipc=(\d+) rx_batches=(\d+) rx_packets=(\d+) "
        rb"rx_copies=(\d+)", data
    )
    if not net_counters:
        errors.append("network data-path counters were not reported")
    else:
        values = list(map(int, net_counters.groups()))
        tx_ipc, tx_batches, tx_packets, tx_copies, tx_kicks = values[:5]
        rx_ipc, rx_batches, rx_packets, rx_copies = values[5:]
        if not 0 < tx_batches <= tx_ipc < tx_packets == tx_copies or \
                tx_kicks >= tx_packets or \
                not 0 < rx_batches <= rx_ipc < rx_packets == rx_copies:
            errors.append("network data-path counters are invalid")

    child_pool = re.findall(
        rb"LUNA_CHILD_RESOURCE_HIGH_WATER threads=(\d+)/(\d+) "
        rb"sync=(\d+)/(\d+) manager_ipc=(\d+)", data
    )
    if not child_pool:
        errors.append("child resource high-water was not reported")
    else:
        threads, thread_slots, sync, sync_slots, manager_ipc = map(
            int, child_pool[-1]
        )
        if not 0 < threads <= thread_slots == 47 or \
                not 0 < sync <= sync_slots == 96 or manager_ipc <= 0:
            errors.append("child resource high-water is invalid")

    lifecycle = re.search(
        rb"LUNA_LIFECYCLE_BENCHMARK_OK rounds=(\d+)", data
    )
    if not lifecycle:
        errors.append("lifecycle benchmark statistics were not reported")
    else:
        rounds = int(lifecycle.group(1))
        samples = {
            name: [int(value) for value in re.findall(
                rb"LUNA_LIFECYCLE_" + name + rb"_SAMPLE ns=(\d+)", data
            )]
            for name in (b"CREATE", b"START", b"DESTROY")
        }
        if rounds != 100 or any(
            len(values) != rounds or any(value <= 0 for value in values)
            for values in samples.values()
        ):
            errors.append("lifecycle benchmark statistics are invalid")

    user_resources = re.search(
        rb"LUNA_USER_RESOURCE_PEAK_OK heap_bytes=(\d+) workers=(\d+)", data
    )
    if not user_resources:
        errors.append("user resource peak statistics were not reported")
    else:
        heap_bytes, workers = map(int, user_resources.groups())
        if not 0 < heap_bytes <= 1048576 or workers != 4:
            errors.append("user resource peak statistics are invalid")

    rx_elapsed = re.search(
        rb"LUNA_NET_QUEUE_STATS .* elapsed_ns=(\d+)", data
    )
    tx_elapsed = re.search(
        rb"LUNA_NET_TX_QUEUE_STATS .* elapsed_ns=(\d+)", data
    )
    if not rx_elapsed or int(rx_elapsed.group(1)) <= 0:
        errors.append("RX benchmark elapsed time is invalid")
    if not tx_elapsed or int(tx_elapsed.group(1)) <= 0:
        errors.append("TX benchmark elapsed time is invalid")

    if errors:
        print("\nSMOKE TEST FAILED", file=sys.stderr)
        for error in errors:
            print(f"- {error}", file=sys.stderr)
        return 1
    print("\nSMOKE TEST PASSED")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
