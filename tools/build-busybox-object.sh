#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
set -euo pipefail

if [[ $# -ne 3 ]]; then
    echo "usage: $0 <busybox-source> <config-fragment> <output-object>" >&2
    exit 2
fi

source_dir="$(cd "$1" && pwd)"
config="$(cd "$(dirname "$2")" && pwd)/$(basename "$2")"
output="$(cd "$(dirname "$3")" && pwd)/$(basename "$3")"
build_dir="$(dirname "$output")/busybox-phase2.4"

make -C "$source_dir" distclean >/dev/null
rm -rf "$build_dir"
mkdir -p "$build_dir"
make -C "$source_dir" O="$build_dir" allnoconfig >/dev/null

while IFS= read -r setting; do
    [[ -z "$setting" || "$setting" == \#* ]] && continue
    key="${setting%%=*}"
    case "$setting" in
        CONFIG_*=y)
            sed -i "s/^# ${key} is not set$/${key}=y/" "$build_dir/.config"
            ;;
        CONFIG_*=*)
            escaped="${setting//|/\\|}"
            sed -i "s|^${key}=.*$|${escaped}|" "$build_dir/.config"
            ;;
        *)
            echo "unsupported BusyBox config setting: $setting" >&2
            exit 1
            ;;
    esac
done < "$config"

make -C "$source_dir" O="$build_dir" oldconfig </dev/null >/dev/null
make -C "$source_dir" O="$build_dir" -j"$(nproc)" busybox_unstripped >/dev/null

libraries=(
    coreutils/lib.a libbb/lib.a libpwdgrp/lib.a shell/lib.a
)
library_paths=()
for library in "${libraries[@]}"; do
    library_paths+=("$build_dir/$library")
done

raw_object="$build_dir/luna-busybox.raw.o"
ld -r -u ash_main -o "$raw_object" \
    --start-group "${library_paths[@]}" --end-group

objcopy \
    --redefine-sym __isoc23_sscanf=sscanf \
    --redefine-sym __isoc23_strtol=strtol \
    --redefine-sym __isoc23_strtoll=strtoll \
    --redefine-sym __isoc23_strtoul=strtoul \
    --redefine-sym __isoc23_strtoull=strtoull \
    --redefine-sym __errno_location=luna_bb_errno_location \
    --redefine-sym _exit=luna_bb__exit \
    --redefine-sym exit=luna_bb_exit \
    --redefine-sym open=luna_bb_open \
    --redefine-sym close=luna_bb_close \
    --redefine-sym read=luna_bb_read \
    --redefine-sym write=luna_bb_write \
    --redefine-sym lseek=luna_bb_lseek \
    --redefine-sym dup=luna_bb_dup \
    --redefine-sym dup2=luna_bb_dup2 \
    --redefine-sym fcntl=luna_bb_fcntl \
    --redefine-sym fstat=luna_bb_fstat \
    --redefine-sym stat=luna_bb_stat \
    --redefine-sym lstat=luna_bb_lstat \
    --redefine-sym chdir=luna_bb_chdir \
    --redefine-sym fchdir=luna_bb_fchdir \
    --redefine-sym chroot=luna_bb_chroot \
    --redefine-sym getcwd=luna_bb_getcwd \
    --redefine-sym isatty=luna_bb_isatty \
    --redefine-sym ioctl=luna_bb_ioctl \
    --redefine-sym tcgetattr=luna_bb_tcgetattr \
    --redefine-sym tcsetattr=luna_bb_tcsetattr \
    --redefine-sym umask=luna_bb_umask \
    --redefine-sym unlink=luna_bb_unlink \
    --redefine-sym rename=luna_bb_rename \
    --redefine-sym getpid=luna_bb_getpid \
    --redefine-sym getppid=luna_bb_getppid \
    --redefine-sym geteuid=luna_bb_geteuid \
    --redefine-sym getegid=luna_bb_getegid \
    --redefine-sym mallopt=luna_bb_mallopt \
    --redefine-sym malloc=luna_bb_malloc \
    --redefine-sym free=luna_bb_free \
    --redefine-sym realloc=luna_bb_realloc \
    --redefine-sym strdup=luna_bb_strdup \
    --redefine-sym strndup=luna_bb_strndup \
    --redefine-sym raise=luna_bb_raise \
    --redefine-sym signal=luna_bb_signal \
    --redefine-sym sigaction=luna_bb_sigaction \
    --redefine-sym sigaddset=luna_bb_sigaddset \
    --redefine-sym sigemptyset=luna_bb_sigemptyset \
    --redefine-sym sigfillset=luna_bb_sigfillset \
    --redefine-sym sigprocmask=luna_bb_sigprocmask \
    --redefine-sym sigsuspend=luna_bb_sigsuspend \
    --redefine-sym prctl=luna_bb_prctl \
    --redefine-sym fork=luna_bb_fork \
    --redefine-sym vfork=luna_bb_vfork \
    --redefine-sym execve=luna_bb_execve \
    --redefine-sym execvp=luna_bb_execvp \
    --redefine-sym waitpid=luna_bb_waitpid \
    --redefine-sym pipe=luna_bb_pipe \
    "$raw_object" "$output"

if nm "$output" | grep -Eq ' [Tt] main$'; then
    echo "BusyBox object unexpectedly defines main" >&2
    exit 1
fi
if nm -u "$output" | grep -Eq '__isoc23_| (open|read|write|fork|execve|waitpid)$'; then
    echo "BusyBox object contains an unredirected ABI symbol" >&2
    exit 1
fi
