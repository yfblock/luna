#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Report which optional QEMU network backends are runnable on this host."""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path


BACKENDS = ("socket", "slirp", "passt", "tap")


def command_output(command: list[str]) -> tuple[int, str]:
    result = subprocess.run(
        command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, check=False,
    )
    return result.returncode, result.stdout


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--require", action="append", choices=BACKENDS, default=[],
        help="return failure if this backend is unavailable",
    )
    parser.add_argument(
        "--tap-ifname", default=os.environ.get("LUNA_TAP_IFNAME", "luna-tap0")
    )
    args = parser.parse_args()

    qemu = os.environ.get("LUNA_QEMU") or shutil.which("qemu-system-x86_64")
    matrix: dict[str, dict[str, object]] = {}
    netdevs: set[str] = set()
    devices = ""
    if qemu:
        _, netdev_help = command_output([qemu, "-netdev", "help"])
        netdevs = set(re.findall(r"^([a-z0-9-]+)$", netdev_help, re.MULTILINE))
        _, devices = command_output([qemu, "-device", "help"])

    common_reason = None
    if not qemu:
        common_reason = "qemu-system-x86_64 is missing"
    elif "virtio-net-pci" not in devices:
        common_reason = "QEMU lacks virtio-net-pci"

    for backend in BACKENDS:
        reasons: list[str] = []
        if common_reason:
            reasons.append(common_reason)
        required_qemu_backend = {
            "socket": "socket",
            "slirp": "user",
            "passt": "passt",
            "tap": "tap",
        }[backend]
        if qemu and required_qemu_backend not in netdevs:
            reasons.append(f"QEMU lacks {required_qemu_backend} netdev")
        if backend == "passt" and not shutil.which("passt"):
            reasons.append("passt helper is missing from PATH")
        if backend == "tap":
            code, _ = command_output(["ip", "link", "show", "dev", args.tap_ifname])
            if code:
                reasons.append(f"TAP interface {args.tap_ifname} does not exist")
            else:
                code, addresses = command_output(
                    ["ip", "-4", "addr", "show", "dev", args.tap_ifname]
                )
                if code or "10.0.2.2/24" not in addresses:
                    reasons.append(
                        f"TAP interface {args.tap_ifname} lacks 10.0.2.2/24"
                    )
                disable_ipv6 = (
                    Path("/proc/sys/net/ipv6/conf") /
                    args.tap_ifname / "disable_ipv6"
                )
                if not disable_ipv6.exists() or disable_ipv6.read_text().strip() != "1":
                    reasons.append(
                        f"TAP interface {args.tap_ifname} must disable IPv6"
                    )
                _, link = command_output(
                    ["ip", "link", "show", "dev", args.tap_ifname]
                )
                if "MULTICAST" in link.splitlines()[0]:
                    reasons.append(
                        f"TAP interface {args.tap_ifname} must disable multicast"
                    )
        matrix[backend] = {
            "available": not reasons,
            "reasons": reasons,
        }

    print(json.dumps(matrix, indent=2, sort_keys=True))
    failed = [name for name in args.require if not matrix[name]["available"]]
    if failed:
        print(
            "required network backends unavailable: " + ", ".join(failed),
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
