# Dependency lock and checkout notes

The dependency source and build trees under `deps/` are intentionally not
stored in the luna Git repository. They currently occupy about 5.7 GiB and
contain generated files larger than GitHub's 100 MiB per-file limit.

The luna-owned seL4 build settings remain tracked as
`deps/lkl_settings.cmake`. The LKL changes required by luna are tracked as a
patch in `patches/lkl-tty.patch`.
BusyBox is also pinned and its Luna-specific nofork metadata change is tracked
in `patches/busybox-nofork-cat.patch`. The controlled static worker hooks in
ash are tracked separately in `patches/busybox-static-runtime.patch`.

## seL4 dependencies

- Manifest repository: `https://github.com/seL4/seL4-tutorials-manifest.git`
- Manifest commit: `cf8e88fbd953fedbf65ddee6eac6ccabb4a36df3`
- Manifest file: `default.xml`

The supported path from the luna repository root is:

```sh
./setup-deps.sh
```

It verifies the host tools and Python modules, synchronizes the pinned seL4
manifest, checks out the exact LKL base revision, and applies the tty patch
idempotently. Use `./setup-deps.sh --check-only` to audit an existing checkout.

The equivalent manual seL4 commands are:

```sh
mkdir -p deps
cd deps
repo init -u https://github.com/seL4/seL4-tutorials-manifest.git \
  -b cf8e88fbd953fedbf65ddee6eac6ccabb4a36df3 -m default.xml
repo sync
cd ..
```

## LKL/Linux

- Repository: `https://github.com/lkl/linux.git`
- Base commit: `6bce81422a8a420389c1b100d7e0473e066638b6`
- Local feature patch: `patches/lkl-tty.patch`

```sh
git clone https://github.com/lkl/linux.git deps/lkl-linux
git -C deps/lkl-linux checkout 6bce81422a8a420389c1b100d7e0473e066638b6
git -C deps/lkl-linux apply ../../patches/lkl-tty.patch
```

## BusyBox

- Repository: `https://github.com/mirror/busybox.git`
- Base commit/tag 1.36.1: `1a64f6a20aaf6ea4dbba68bbfa8cc1ab7e5c57c4`
- Local feature patches: `patches/busybox-nofork-cat.patch`,
  `patches/busybox-static-runtime.patch`

```sh
git clone https://github.com/mirror/busybox.git deps/busybox
git -C deps/busybox checkout 1a64f6a20aaf6ea4dbba68bbfa8cc1ab7e5c57c4
git -C deps/busybox apply ../../patches/busybox-nofork-cat.patch
git -C deps/busybox apply ../../patches/busybox-static-runtime.patch
```

Phase 2.4 additionally uses `ld`, `objcopy`, and `nm` to produce and audit the
relocatable BusyBox host-program object. Phase 2.4.4 links four symbol-prefixed
worker copies so concurrent applets do not share mutable libbb global state.

Phase 2.5 requires a QEMU build with the `socket` netdev backend, legacy
`virtio-net-pci` support, and `ivshmem-plain` for the host-file ext4 backing.
It does not require slirp, TAP, root privileges, or
external network access. `tools/net-peer.py` uses only the Python standard
library and binds two configurable localhost UDP ports (18081/18082 by
default).

After checkout, follow the build instructions in `README.md` or run
`./run.sh --build-only`.

The current build also expects the Python packages and `xmllint` setup noted
in `README.md`. Phase 2.3 additionally requires `mke2fs` and `e2fsck`
(normally provided by the `e2fsprogs` package); the build refuses to continue
without them and validates the generated rootfs with `e2fsck -fn`.
