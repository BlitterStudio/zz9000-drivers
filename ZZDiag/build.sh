#!/bin/sh
set -eu

if ! command -v m68k-amigaos-gcc >/dev/null 2>&1; then
    script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
    exec "$script_dir/../tools/amiga-docker.sh" ZZDiag ./build.sh "$@"
fi

# LRA register allocation (-mlra) exists on Bebbo GCC 10+ only; the older
# 6.5.0b toolchain rejects the flag, so enable it where supported.
lra=
if m68k-amigaos-gcc -mlra -x c -fsyntax-only /dev/null 2>/dev/null; then
  lra=-mlra
fi

m68k-amigaos-gcc ZZDiag.c -m68030 -O2 $lra -o ZZDiag \
    -Wall -Wextra -Wno-unused-parameter -I../include -I../usb-poseidon -lamiga -noixemul
