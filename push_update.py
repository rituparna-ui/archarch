#!/usr/bin/env python3
"""
Push a live kernel update to disk.img.

Writes the update header at sector 4 and the kernel.bin image
starting at sector 6. The running kernel's update-poller task
will detect the new header and apply the update.

Usage:
    python3 push_update.py disk.img kernel.bin [generation]

If generation is not specified, it reads the current generation
from the disk and increments it.
"""
import struct
import sys
import os

SECTOR_SIZE = 512
HEADER_SECTOR = 4
IMAGE_SECTOR = 6
HEADER_MAGIC = 0x4C495645554E4557  # "LIVEUPDW"

def checksum(data):
    return sum(data) & 0xFFFFFFFF

def read_current_gen(disk_path):
    """Read current generation from disk header."""
    try:
        with open(disk_path, 'rb') as f:
            f.seek(HEADER_SECTOR * SECTOR_SIZE)
            hdr = f.read(SECTOR_SIZE)
            magic, size, sectors, gen, csum = struct.unpack_from('<QIIIQ', hdr, 0)
            # Unpack differently — our struct is: u64 magic, u32 size, u32 sectors, u32 gen, u32 csum
            magic, size, sectors, gen, csum = struct.unpack_from('<QIIII', hdr, 0)
            if magic == HEADER_MAGIC:
                return gen
    except:
        pass
    return 0

def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <disk.img> <kernel.bin> [generation]")
        sys.exit(1)

    disk_path = sys.argv[1]
    image_path = sys.argv[2]

    # Read image
    with open(image_path, 'rb') as f:
        image_data = f.read()

    image_size = len(image_data)
    image_sectors = (image_size + SECTOR_SIZE - 1) // SECTOR_SIZE

    # Determine generation
    if len(sys.argv) >= 4:
        generation = int(sys.argv[3])
    else:
        generation = read_current_gen(disk_path) + 1

    csum = checksum(image_data)

    print(f"[push_update] Image: {image_path} ({image_size} bytes, {image_sectors} sectors)")
    print(f"[push_update] Generation: {generation}")
    print(f"[push_update] Checksum: 0x{csum:08x}")

    # Pad image to sector boundary
    padded = image_data + b'\x00' * (image_sectors * SECTOR_SIZE - image_size)

    # Build header (sector 4)
    # struct update_header { u64 magic; u32 image_size; u32 image_sectors; u32 generation; u32 checksum; }
    header = struct.pack('<QIIII',
                         HEADER_MAGIC,
                         image_size,
                         image_sectors,
                         generation,
                         csum)
    header = header.ljust(SECTOR_SIZE, b'\x00')

    # Write to disk
    with open(disk_path, 'r+b') as f:
        # Write image at sector 6
        f.seek(IMAGE_SECTOR * SECTOR_SIZE)
        f.write(padded)
        print(f"[push_update] Wrote image at sector {IMAGE_SECTOR}")

        # Write header at sector 4 (AFTER image, so header is only
        # visible once image is fully written — atomic-ish)
        f.seek(HEADER_SECTOR * SECTOR_SIZE)
        f.write(header)
        print(f"[push_update] Wrote header at sector {HEADER_SECTOR}")

    print(f"[push_update] Done! Kernel will detect gen={generation}")

if __name__ == '__main__':
    main()
