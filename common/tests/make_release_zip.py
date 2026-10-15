#!/usr/bin/env python3
"""Write a firmware-release-shaped ZIP and print BOOT.bin's SHA-256.

The image carries the Zynq boot header and mixes literal, run and copied
data so the deflate stream exercises dynamic Huffman blocks and long
matches. The layout matches zz9000-firmware's build_release_assets.sh:
a directory entry, BOOT.bin, then ZZ9000.CFG.
"""
import hashlib
import random
import sys
import zipfile


def boot_image(size):
    rng = random.Random(9000)
    data = bytearray(b"\xfe\xff\xff\xea" * 8 + b"\x66\x55\x99\xaaXNLX")
    while len(data) < size:
        kind = rng.randrange(3)
        n = rng.randrange(64, 8192)
        if kind == 0:
            data += bytes(rng.randrange(256) for _ in range(n // 8))
        elif kind == 1:
            data += bytes([rng.randrange(256)]) * n
        else:
            start = rng.randrange(max(1, len(data) - n))
            data += data[start:start + n]
    return bytes(data)


def main():
    data = boot_image(3 * 1024 * 1024)
    root = "zz9000-firmware-vtest-zorro3/"
    with zipfile.ZipFile(sys.argv[1], "w", zipfile.ZIP_DEFLATED,
                         compresslevel=9) as z:
        z.writestr(root, b"")
        z.writestr(root + "BOOT.bin", data)
        z.writestr(root + "ZZ9000.CFG", b"fast_ram = on\n")
    print(hashlib.sha256(data).hexdigest())


if __name__ == "__main__":
    main()
