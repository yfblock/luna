#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
import struct
import sys
from pathlib import Path


MAGIC = b"LUNAFS23"
BLOCK_SIZE = 4096
HEADER = struct.Struct("<8sQII")
INDEX = struct.Struct("<I")


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: pack-rootfs.py <image> <pack>", file=sys.stderr)
        return 2
    image_path = Path(sys.argv[1])
    output_path = Path(sys.argv[2])
    image = image_path.read_bytes()
    if not image or len(image) % BLOCK_SIZE:
        print("rootfs image must be a non-empty multiple of 4096 bytes",
              file=sys.stderr)
        return 1

    blocks = []
    for index in range(len(image) // BLOCK_SIZE):
        block = image[index * BLOCK_SIZE:(index + 1) * BLOCK_SIZE]
        if any(block):
            blocks.append((index, block))

    with output_path.open("wb") as output:
        output.write(HEADER.pack(MAGIC, len(image), BLOCK_SIZE, len(blocks)))
        for index, block in blocks:
            output.write(INDEX.pack(index))
            output.write(block)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
