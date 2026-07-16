#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Run or parse Luna's repeatable data-path and resource benchmarks."""

import argparse
import json
import subprocess
import sys
import tempfile
from pathlib import Path


DEFAULT_BASELINE = Path(__file__).with_name("performance-baseline.json")


def marker(data: bytes, pattern: bytes, name: str) -> list[int]:
    import re

    match = re.search(pattern, data)
    if not match:
        raise ValueError(f"missing benchmark marker: {name}")
    return [int(value) for value in match.groups()]


def marker_last(data: bytes, pattern: bytes, name: str) -> list[int]:
    import re

    matches = re.findall(pattern, data)
    if not matches:
        raise ValueError(f"missing benchmark marker: {name}")
    values = matches[-1]
    if not isinstance(values, tuple):
        values = (values,)
    return [int(value) for value in values]


def percentile(values: list[int], percent: int) -> int:
    """Return the nearest-rank percentile for a non-empty sample set."""
    if not values:
        raise ValueError("cannot calculate a percentile without samples")
    ordered = sorted(values)
    rank = (len(ordered) * percent + 99) // 100
    return ordered[max(1, rank) - 1]


def report_value(report: dict[str, object], path: str) -> object:
    value: object = report
    for component in path.split("."):
        if not isinstance(value, dict) or component not in value:
            raise ValueError(f"baseline references missing metric: {path}")
        value = value[component]
    return value


def check_baseline(
    report: dict[str, object], baseline: dict[str, object]
) -> list[str]:
    """Return human-readable regressions against an explicit baseline."""
    if not isinstance(report, dict) or not isinstance(baseline, dict):
        raise ValueError("report and baseline must be JSON objects")
    errors: list[str] = []
    required_backend = baseline.get("network_backend")
    if required_backend is not None and report.get("network_backend") != required_backend:
        errors.append(
            "network_backend: expected "
            f"{required_backend}, got {report.get('network_backend')}"
        )
    comparisons = (
        ("minimums", lambda actual, limit: actual >= limit, "below minimum"),
        ("maximums", lambda actual, limit: actual <= limit, "above maximum"),
        ("equals", lambda actual, limit: actual == limit, "changed"),
    )
    for section, accepted, description in comparisons:
        limits = baseline.get(section, {})
        if not isinstance(limits, dict):
            raise ValueError(f"baseline section must be an object: {section}")
        for path, limit in limits.items():
            actual = report_value(report, path)
            if not isinstance(actual, (int, float)) or not isinstance(
                limit, (int, float)
            ):
                raise ValueError(f"baseline metric must be numeric: {path}")
            if not accepted(actual, limit):
                errors.append(
                    f"{path}: {actual} {description} {limit}"
                )
    return errors


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
    pipeline_detail = marker(
        data,
        rb"LUNA_PIPELINE_BENCHMARK_OK .* samples=(\d+) p50_ns=(\d+) "
        rb"p95_ns=(\d+) p99_ns=(\d+) read_calls=(\d+) "
        rb"read_bytes=(\d+) write_calls=(\d+) write_bytes=(\d+)",
        "pipeline-detail",
    )
    pipeline_samples = [int(value) for value in re.findall(
        rb"LUNA_PIPELINE_SAMPLE ns=(\d+)", data
    )]
    if len(pipeline_samples) != pipeline_detail[0]:
        raise ValueError("incomplete pipeline benchmark samples")
    rx = marker(
        data,
        rb"LUNA_NET_QUEUE_STATS received=(\d+).*elapsed_ns=(\d+)",
        "network-rx",
    )
    rx_throughput = marker(
        data,
        rb"LUNA_NET_RX_THROUGHPUT_OK packets=(\d+) bytes=(\d+) "
        rb"samples=(\d+) p50_ns=(\d+) p95_ns=(\d+) p99_ns=(\d+)",
        "network-rx-throughput",
    )
    rx_throughput_samples = [int(value) for value in re.findall(
        rb"LUNA_NET_RX_THROUGHPUT_SAMPLE ns=(\d+)", data
    )]
    if len(rx_throughput_samples) != rx_throughput[2]:
        raise ValueError("incomplete network RX throughput samples")
    tx = marker(
        data,
        rb"LUNA_NET_TX_QUEUE_STATS sent=(\d+).*elapsed_ns=(\d+)",
        "network-tx",
    )
    block = marker(
        data,
        rb"LUNA_BLOCK_BENCHMARK_OK bytes=(\d+) seq_write_ns=(\d+) "
        rb"cold_read_ns=(\d+) hot_read_ns=(\d+) random_ops=(\d+) "
        rb"random_write_ns=(\d+) random_read_ns=(\d+)",
        "block",
    )
    net_counters = marker(
        data,
        rb"LUNA_NET_MANAGER_COUNTERS tx_ipc=(\d+) tx_batches=(\d+) "
        rb"tx_packets=(\d+) tx_copies=(\d+) tx_kicks=(\d+) "
        rb"rx_ipc=(\d+) rx_batches=(\d+) rx_packets=(\d+) "
        rb"rx_copies=(\d+)",
        "network-counters",
    )
    disk_counters = marker_last(
        data,
        rb"LUNA_DISK_BATCH_COUNTERS ipc=(\d+) batches=(\d+) "
        rb"requests=(\d+) copies=(\d+) backpressure=(\d+) "
        rb"queue_high_water=(\d+)",
        "disk-counters",
    )
    disk_manager = marker(
        data,
        rb"LUNA_DISK_MANAGER_COUNTERS ipc=(\d+) batches=(\d+) "
        rb"requests=(\d+) copies=(\d+) queue_high_water=(\d+)",
        "disk-manager-counters",
    )
    child_pool = marker_last(
        data,
        rb"LUNA_CHILD_RESOURCE_HIGH_WATER threads=(\d+)/(\d+) "
        rb"sync=(\d+)/(\d+) manager_ipc=(\d+)",
        "child-resource-high-water",
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
            "samples": pipeline_detail[0],
            "p50_ns": pipeline_detail[1],
            "p95_ns": pipeline_detail[2],
            "p99_ns": pipeline_detail[3],
            "read_calls": pipeline_detail[4],
            "read_bytes": pipeline_detail[5],
            "average_read_size": (
                pipeline_detail[5] // pipeline_detail[4]
                if pipeline_detail[4] else 0
            ),
            "write_calls": pipeline_detail[6],
            "write_bytes": pipeline_detail[7],
            "average_write_size": (
                pipeline_detail[7] // pipeline_detail[6]
                if pipeline_detail[6] else 0
            ),
        },
        "network_rx": {
            "packets": rx[0],
            "bytes": rx[0] * 1200,
            "elapsed_ns": rx[1],
            "bytes_per_sec": rate(rx[0] * 1200, rx[1]),
        },
        "network_rx_throughput": {
            "packets": rx_throughput[0],
            "bytes": rx_throughput[1],
            "samples": rx_throughput[2],
            "p50_ns": rx_throughput[3],
            "p95_ns": rx_throughput[4],
            "p99_ns": rx_throughput[5],
            "p50_bytes_per_sec": rate(rx_throughput[1], rx_throughput[3]),
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
            "cold_read_ns": block[2],
            "cold_read_bytes_per_sec": rate(block[0], block[2]),
            "hot_read_ns": block[3],
            "hot_read_bytes_per_sec": rate(block[0], block[3]),
            "random_ops": block[4],
            "random_write_ns": block[5],
            "random_write_iops": rate(block[4], block[5]),
            "random_read_ns": block[6],
            "random_read_iops": rate(block[4], block[6]),
        },
        "data_path_counters": {
            "network": {
                "tx_ipc": net_counters[0],
                "tx_batches": net_counters[1],
                "tx_packets": net_counters[2],
                "tx_copies": net_counters[3],
                "tx_kicks": net_counters[4],
                "rx_ipc": net_counters[5],
                "rx_batches": net_counters[6],
                "rx_packets": net_counters[7],
                "rx_copies": net_counters[8],
            },
            "block_child": {
                "ipc": disk_counters[0],
                "batches": disk_counters[1],
                "requests": disk_counters[2],
                "copies": disk_counters[3],
                "backpressure": disk_counters[4],
                "queue_high_water": disk_counters[5],
            },
            "block_manager": {
                "ipc": disk_manager[0],
                "batches": disk_manager[1],
                "requests": disk_manager[2],
                "copies": disk_manager[3],
                "queue_high_water": disk_manager[4],
            },
        },
        "lifecycle": {
            "rounds": lifecycle[0],
            "create_avg_ns": sum(lifecycle_samples["CREATE"]) // lifecycle[0],
            "create_max_ns": max(lifecycle_samples["CREATE"]),
            "create_p50_ns": percentile(lifecycle_samples["CREATE"], 50),
            "create_p95_ns": percentile(lifecycle_samples["CREATE"], 95),
            "create_p99_ns": percentile(lifecycle_samples["CREATE"], 99),
            "start_avg_ns": sum(lifecycle_samples["START"]) // lifecycle[0],
            "start_max_ns": max(lifecycle_samples["START"]),
            "start_p50_ns": percentile(lifecycle_samples["START"], 50),
            "start_p95_ns": percentile(lifecycle_samples["START"], 95),
            "start_p99_ns": percentile(lifecycle_samples["START"], 99),
            "destroy_avg_ns": sum(lifecycle_samples["DESTROY"]) // lifecycle[0],
            "destroy_max_ns": max(lifecycle_samples["DESTROY"]),
            "destroy_p50_ns": percentile(lifecycle_samples["DESTROY"], 50),
            "destroy_p95_ns": percentile(lifecycle_samples["DESTROY"], 95),
            "destroy_p99_ns": percentile(lifecycle_samples["DESTROY"], 99),
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
            "child_thread_high_water": child_pool[0],
            "child_thread_slots": child_pool[1],
            "child_sync_high_water": child_pool[2],
            "child_sync_slots": child_pool[3],
            "child_manager_ipc": child_pool[4],
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
    parser.add_argument(
        "--check-baseline",
        nargs="?",
        const=DEFAULT_BASELINE,
        type=Path,
        metavar="JSON",
        help="fail when the report violates performance/resource thresholds",
    )
    parser.add_argument(
        "--output", type=Path, help="also write the JSON report to this file"
    )
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
    encoded = json.dumps(report, indent=2, sort_keys=True)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(encoded + "\n", encoding="utf-8")
    print(encoded)
    if args.check_baseline:
        try:
            baseline = json.loads(args.check_baseline.read_text(encoding="utf-8"))
            if not isinstance(baseline, dict):
                raise ValueError("baseline must be a JSON object")
            errors = check_baseline(report, baseline)
        except (OSError, json.JSONDecodeError, ValueError) as error:
            print(f"invalid performance baseline: {error}", file=sys.stderr)
            return 1
        if errors:
            print("PERFORMANCE BASELINE FAILED", file=sys.stderr)
            for error in errors:
                print(f"- {error}", file=sys.stderr)
            return 1
        print("PERFORMANCE BASELINE PASSED", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
