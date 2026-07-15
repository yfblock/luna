#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
set -euo pipefail

output="$1"
seed="$2"

rm -f "$output"
mke2fs -q -F -t ext4 -b 1024 -O ^has_journal -L luna-rootfs \
    -U 4c554e41-0000-4000-8000-000000000023 \
    -E root_owner=0:0,lazy_itable_init=0,lazy_journal_init=0 \
    -d "$seed" "$output" 16384
e2fsck -fn "$output"
