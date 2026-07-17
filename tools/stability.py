#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Run repeated full QEMU gates against one persistent ext4 backing file."""

import argparse
import json
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from benchmark import DEFAULT_BASELINE, check_baseline, parse, percentile, report_value


SUMMARY_METRICS = (
    "pipeline.bytes_per_sec",
    "network_rx_throughput.p50_bytes_per_sec",
    "network_tx.bytes_per_sec",
    "network_rx_sustained.bytes_per_sec",
    "network_tx_throughput.bytes_per_sec",
    "block.sequential_write_bytes_per_sec",
    "block.cold_read_bytes_per_sec",
    "block.hot_read_bytes_per_sec",
    "block.random_write_iops",
    "block.random_read_iops",
    "lifecycle.create_p99_ns",
    "lifecycle.start_p99_ns",
    "lifecycle.destroy_p99_ns",
)

RESOURCE_INVARIANTS = (
    "resources.managed_frame_pages",
    "resources.child_tcbs",
    "resources.child_notifications",
    "resources.child_heap_pages",
    "resources.disk_window_pages",
    "resources.network_window_pages",
    "resources.child_thread_slots",
    "resources.child_sync_slots",
)


def run_checked(command: list[str], root: Path) -> subprocess.CompletedProcess[bytes]:
    return subprocess.run(
        command,
        cwd=root,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )


def main() -> int:
    parser = argparse.ArgumentParser(
        description="repeat Luna's full 100-child QEMU gate and check drift"
    )
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--timeout", type=float, default=600.0)
    parser.add_argument(
        "--net-backend", choices=("socket", "slirp", "passt", "tap"),
        default="socket",
    )
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--no-baseline", action="store_true")
    parser.add_argument("--log-dir", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.rounds < 2:
        parser.error("--rounds must be at least 2 to detect cross-QEMU drift")
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    if args.baseline and args.no_baseline:
        parser.error("--baseline and --no-baseline are mutually exclusive")

    root = Path(__file__).resolve().parents[1]
    baseline_path = args.baseline
    if not baseline_path and not args.no_baseline and args.net_backend == "socket":
        baseline_path = DEFAULT_BASELINE
    baseline = None
    if baseline_path:
        try:
            loaded = json.loads(baseline_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as error:
            print(f"cannot load performance baseline: {error}", file=sys.stderr)
            return 2
        if not isinstance(loaded, dict):
            print("performance baseline must be a JSON object", file=sys.stderr)
            return 2
        baseline = loaded

    log_dir = args.log_dir.resolve() if args.log_dir else None
    if log_dir:
        log_dir.mkdir(parents=True, exist_ok=True)

    reports: list[dict[str, object]] = []
    marker = f"luna-stability-{int(time.time())}-{round(time.monotonic_ns() % 1000000)}"
    with tempfile.TemporaryDirectory(prefix="luna-stability-") as state:
        disk = Path(state) / "rootfs.ext4"
        for round_index in range(1, args.rounds + 1):
            print(f"stability: QEMU round {round_index}/{args.rounds}", flush=True)
            command = [
                sys.executable,
                str(root / "tools/qemu_smoke.py"),
                "--timeout", str(args.timeout),
                "--disk-image", str(disk),
                "--net-backend", args.net_backend,
            ]
            if round_index == 1:
                command.extend(("--reset-disk", "--persist-write", marker))
            else:
                command.extend(("--persist-read", marker))
            result = run_checked(command, root)
            if log_dir:
                (log_dir / f"qemu-{round_index:03d}.log").write_bytes(result.stdout)
            if result.returncode:
                sys.stderr.buffer.write(result.stdout)
                print(
                    f"stability: QEMU round {round_index} failed",
                    file=sys.stderr,
                )
                return result.returncode
            try:
                report = parse(result.stdout)
            except ValueError as error:
                print(
                    f"stability: invalid benchmark data in round {round_index}: {error}",
                    file=sys.stderr,
                )
                return 1
            if baseline:
                try:
                    regressions = check_baseline(report, baseline)
                except ValueError as error:
                    print(f"invalid performance baseline: {error}", file=sys.stderr)
                    return 2
                if regressions:
                    print(
                        f"stability: baseline failed in round {round_index}",
                        file=sys.stderr,
                    )
                    for regression in regressions:
                        print(f"- {regression}", file=sys.stderr)
                    return 1
            fsck = run_checked(["e2fsck", "-fn", str(disk)], root)
            if fsck.returncode:
                sys.stderr.buffer.write(fsck.stdout)
                print(
                    f"stability: host e2fsck failed after round {round_index}",
                    file=sys.stderr,
                )
                return fsck.returncode
            reports.append(report)

    invariant_reference = {
        path: report_value(reports[0], path) for path in RESOURCE_INVARIANTS
    }
    for round_index, report in enumerate(reports[1:], 2):
        for path, expected in invariant_reference.items():
            actual = report_value(report, path)
            if actual != expected:
                print(
                    f"stability: resource drift in round {round_index}: "
                    f"{path} expected {expected}, got {actual}",
                    file=sys.stderr,
                )
                return 1

    metric_summary: dict[str, dict[str, int]] = {}
    for path in SUMMARY_METRICS:
        values = [int(report_value(report, path)) for report in reports]
        metric_summary[path] = {
            "min": min(values),
            "p50": percentile(values, 50),
            "p95": percentile(values, 95),
            "max": max(values),
        }
    reference_comparison: dict[str, dict[str, int]] = {}
    if baseline and isinstance(baseline.get("reference"), dict):
        for path, reference in baseline["reference"].items():
            if path not in metric_summary or not isinstance(reference, int):
                continue
            observed = metric_summary[path]["p50"]
            reference_comparison[path] = {
                "reference": reference,
                "observed_p50": observed,
                "percent_of_reference": observed * 100 // reference,
            }
    summary = {
        "rounds": args.rounds,
        "network_backend": args.net_backend,
        "cross_qemu_persistence": True,
        "fsck_after_each_round": True,
        "baseline": str(baseline_path) if baseline_path else None,
        "resource_invariants": invariant_reference,
        "metrics": metric_summary,
        "reference_comparison": reference_comparison,
    }
    encoded = json.dumps(summary, indent=2, sort_keys=True)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(encoded + "\n", encoding="utf-8")
    print(encoded)
    print("STABILITY TEST PASSED", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
