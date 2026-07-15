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

sed -i 's|^CONFIG_EXTRA_LDFLAGS=.*$|CONFIG_EXTRA_LDFLAGS="-Wl,--unresolved-symbols=ignore-all"|' \
    "$build_dir/.config"

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

abi_objcopy_args=(
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
    --redefine-sym printf=luna_bb_printf \
    --redefine-sym vprintf=luna_bb_vprintf \
    --redefine-sym dprintf=luna_bb_dprintf \
    --redefine-sym fprintf=luna_bb_fprintf \
    --redefine-sym vfprintf=luna_bb_vfprintf \
    --redefine-sym putchar=luna_bb_putchar \
    --redefine-sym putchar_unlocked=luna_bb_putchar_unlocked \
    --redefine-sym puts=luna_bb_puts \
    --redefine-sym fputs=luna_bb_fputs \
    --redefine-sym fputs_unlocked=luna_bb_fputs_unlocked \
    --redefine-sym putc=luna_bb_putc \
    --redefine-sym putc_unlocked=luna_bb_putc_unlocked \
    --redefine-sym fflush=luna_bb_fflush \
    --redefine-sym ferror=luna_bb_ferror \
    --redefine-sym ferror_unlocked=luna_bb_ferror_unlocked \
    --redefine-sym clearerr=luna_bb_clearerr \
    --redefine-sym getopt=luna_bb_getopt \
    --redefine-sym getopt_long=luna_bb_getopt_long \
    --redefine-sym fopen=luna_bb_fopen \
    --redefine-sym fdopen=luna_bb_fdopen \
    --redefine-sym fclose=luna_bb_fclose \
    --redefine-sym fileno=luna_bb_fileno \
    --redefine-sym fileno_unlocked=luna_bb_fileno_unlocked \
    --redefine-sym getc=luna_bb_getc \
    --redefine-sym getc_unlocked=luna_bb_getc_unlocked \
    --redefine-sym fgetc=luna_bb_fgetc \
    --redefine-sym fread=luna_bb_fread \
    --redefine-sym fwrite=luna_bb_fwrite \
    --redefine-sym fgets=luna_bb_fgets \
    --redefine-sym feof=luna_bb_feof \
    --redefine-sym ungetc=luna_bb_ungetc \
    --redefine-sym rewind=luna_bb_rewind \
    --redefine-sym fseek=luna_bb_fseek \
    --redefine-sym ftell=luna_bb_ftell \
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
    --redefine-sym mkdir=luna_bb_mkdir \
    --redefine-sym rmdir=luna_bb_rmdir \
    --redefine-sym chmod=luna_bb_chmod \
    --redefine-sym ftruncate=luna_bb_ftruncate \
    --redefine-sym uname=luna_bb_uname \
    --redefine-sym utimensat=luna_bb_utimensat \
    --redefine-sym futimens=luna_bb_futimens \
    --redefine-sym readlink=luna_bb_readlink \
    --redefine-sym realpath=luna_bb_realpath \
    --redefine-sym symlink=luna_bb_symlink \
    --redefine-sym link=luna_bb_link \
    --redefine-sym isatty=luna_bb_isatty \
    --redefine-sym ioctl=luna_bb_ioctl \
    --redefine-sym tcgetattr=luna_bb_tcgetattr \
    --redefine-sym tcsetattr=luna_bb_tcsetattr \
    --redefine-sym sleep=luna_bb_sleep \
    --redefine-sym sync=luna_bb_sync \
    --redefine-sym fsync=luna_bb_fsync \
    --redefine-sym fdatasync=luna_bb_fdatasync \
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
    --redefine-sym pipe=luna_bb_pipe
)

shell_object="$build_dir/luna-busybox.shell.o"
objcopy "${abi_objcopy_args[@]}" "$raw_object" "$shell_object"

worker_entries=(
    basename cat cut dirname echo false head ln mkdir printenv printf readlink
    realpath rmdir touch true truncate uname uniq unlink wc
)
worker_undefined=()
for entry in "${worker_entries[@]}"; do
    worker_undefined+=(-u "${entry}_main")
done
worker_raw="$build_dir/luna-busybox.worker.raw.o"
ld -r "${worker_undefined[@]}" -o "$worker_raw" \
    --start-group "$build_dir/coreutils/lib.a" "$build_dir/libbb/lib.a" \
    "$build_dir/libpwdgrp/lib.a" --end-group
worker_mapped="$build_dir/luna-busybox.worker.mapped.o"
objcopy "${abi_objcopy_args[@]}" "$worker_raw" "$worker_mapped"

worker_objects=()
for slot in 0 1 2 3; do
    prefix="luna_bb_slot${slot}_"
    restore=()
    while read -r symbol; do
        [[ -n "$symbol" ]] || continue
        [[ "$symbol" == optind || "$symbol" == optarg ]] && continue
        restore+=(--redefine-sym "${prefix}${symbol}=${symbol}")
    done < <(nm -u "$worker_mapped" | awk '{print $2}')
    prefixed_object="$build_dir/luna-busybox.worker${slot}.prefixed.o"
    worker_object="$build_dir/luna-busybox.worker${slot}.o"
    objcopy --prefix-symbols="$prefix" "$worker_mapped" "$prefixed_object"
    objcopy "${restore[@]}" "$prefixed_object" "$worker_object"
    worker_objects+=("$worker_object")
done

ld -r -o "$output" "$shell_object" "${worker_objects[@]}"

if nm "$output" | grep -Eq ' [Tt] main$'; then
    echo "BusyBox object unexpectedly defines main" >&2
    exit 1
fi
if nm -u "$output" | grep -Eq '__isoc23_| (open|read|write|fork|execve|waitpid)$'; then
    echo "BusyBox object contains an unredirected ABI symbol" >&2
    exit 1
fi
