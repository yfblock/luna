#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Run or parse Luna's repeatable data-path and resource benchmarks."""

import argparse
import json
import subprocess
import sys
import tempfile
from pathlib import Path


def marker(data: bytes, pattern: bytes, name: str) -> list[int]:
    import re

    match = re.search(pattern, data)
    if not match:
        raise ValueError(f"missing benchmark marker: {name}")
    return [int(value) for value in match.groups()]


def parse(data: bytes) -> dict[str, object]:
    import re

    backend_match = re.search(
        rb"LUNA_QEMU_NET_BACKEND backend=(socket|slirp|passt|tap)", data
    )
    if not backend_match:
        raise ValueError("missing network backend marker")
    pipeline = marker(
        data,
        rb"LUNA_PIPELINE_BENCHMARK_OK bytes=(\d+) elapsed_ns=(\d+) "
        rb"bytes_per_sec=(\d+)",
        "pipeline",
    )
    rx = marker(
        data,
        rb"LUNA_NET_QUEUE_STATS received=(\d+).*elapsed_ns=(\d+)",
        "network-rx",
    )
    tx = marker(
        data,
        rb"LUNA_NET_TX_QUEUE_STATS sent=(\d+).*elapsed_ns=(\d+)",
        "network-tx",
    )
    block = marker(
        data,
        rb"LUNA_BLOCK_BENCHMARK_OK bytes=(\d+) seq_write_ns=(\d+) "
        rb"seq_read_ns=(\d+) random_ops=(\d+) random_write_ns=(\d+) "
        rb"random_read_ns=(\d+)",
        "block",
    )
    lifecycle = marker(
        data, rb"LUNA_LIFECYCLE_BENCHMARK_OK rounds=(\d+)", "lifecycle"
    )
    lifecycle_samples = {
        name: [int(value) for value in re.findall(
            rb"LUNA_LIFECYCLE_" + name.encode() + rb"_SAMPLE ns=(\d+)",
            data,
        )]
        for name in ("CREATE", "START", "DESTROY")
    }
    if any(len(values) != lifecycle[0] for values in lifecycle_samples.values()):
        raise ValueError("incomplete lifecycle benchmark samples")
    manager = marker(
        data,
        rb"LUNA_RESOURCE_PEAK_OK managed_frame_pages=(\d+) child_tcbs=(\d+) "
        rb"child_notifications=(\d+) heap_pages=(\d+) disk_pages=(\d+) "
        rb"net_pages=(\d+)",
        "manager-resources",
    )
    user = marker(
        data,
        rb"LUNA_USER_RESOURCE_PEAK_OK heap_bytes=(\d+) workers=(\d+)",
        "user-resources",
    )

    def rate(units: int, elapsed_ns: int) -> int:
        return units * 1_000_000_000 // elapsed_ns

    return {
        "network_backend": backend_match.group(1).decode("ascii"),
        "pipeline": {
            "bytes": pipeline[0],
            "elapsed_ns": pipeline[1],
            "bytes_per_sec": pipeline[2],
        },
        "network_rx": {
            "packets": rx[0],
            "bytes": rx[0] * 1200,
            "elapsed_ns": rx[1],
            "bytes_per_sec": rate(rx[0] * 1200, rx[1]),
        },
        "network_tx": {
            "packets": tx[0],
            "bytes": tx[0] * 1200,
            "elapsed_ns": tx[1],
            "bytes_per_sec": rate(tx[0] * 1200, tx[1]),
        },
        "block": {
            "bytes": block[0],
            "sequential_write_ns": block[1],
            "sequential_write_bytes_per_sec": rate(block[0], block[1]),
            "sequential_read_ns": block[2],
            "sequential_read_bytes_per_sec": rate(block[0], block[2]),
            "random_ops": block[3],
            "random_write_ns": block[4],
            "random_write_iops": rate(block[3], block[4]),
            "random_read_ns": block[5],
            "random_read_iops": rate(block[3], block[5]),
        },
        "lifecycle": {
            "rounds": lifecycle[0],
            "create_avg_ns": sum(lifecycle_samples["CREATE"]) // lifecycle[0],
            "create_max_ns": max(lifecycle_samples["CREATE"]),
            "start_avg_ns": sum(lifecycle_samples["START"]) // lifecycle[0],
            "start_max_ns": max(lifecycle_samples["START"]),
            "destroy_avg_ns": sum(lifecycle_samples["DESTROY"]) // lifecycle[0],
            "destroy_max_ns": max(lifecycle_samples["DESTROY"]),
        },
        "resources": {
            "managed_frame_pages": manager[0],
            "child_tcbs": manager[1],
            "child_notifications": manager[2],
            "child_heap_pages": manager[3],
            "disk_window_pages": manager[4],
            "network_window_pages": manager[5],
            "busybox_heap_peak_bytes": user[0],
            "static_worker_peak": user[1],
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", type=Path, help="parse a saved QEMU log")
    parser.add_argument("--timeout", type=float, default=480.0)
    parser.add_argument(
        "--net-backend", choices=("socket", "slirp", "passt", "tap"),
        default="socket",
    )
    parser.add_argument("--show-log", action="store_true")
    args = parser.parse_args()

    if args.input:
        data = args.input.read_bytes()
    else:
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory(prefix="luna-benchmark-") as state:
            command = [
                sys.executable,
                str(root / "tools/qemu_smoke.py"),
                "--timeout", str(args.timeout),
                "--disk-image", str(Path(state) / "rootfs.ext4"),
                "--reset-disk",
                "--net-backend", args.net_backend,
            ]
            result = subprocess.run(
                command, cwd=root, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, check=False,
            )
            data = result.stdout
            if args.show_log or result.returncode:
                sys.stderr.buffer.write(data)
            if result.returncode:
                return result.returncode
    try:
        report = parse(data)
    except ValueError as error:
        print(str(error), file=sys.stderr)
        return 1
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
