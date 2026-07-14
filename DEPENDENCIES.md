# Dependency lock and checkout notes

The dependency source and build trees under `deps/` are intentionally not
stored in the luna Git repository. They currently occupy about 5.7 GiB and
contain generated files larger than GitHub's 100 MiB per-file limit.

The luna-owned seL4 build settings remain tracked as
`deps/lkl_settings.cmake`. The LKL changes required by luna are tracked as a
patch in `patches/lkl-tty.patch`.

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

After checkout, follow the build instructions in `README.md` or run
`./run.sh --build-only`.

The current build also expects the Python packages and `xmllint` setup noted
in `README.md`.
