#!/bin/sh
set -eu

if ! command -v m68k-amigaos-gcc >/dev/null 2>&1; then
  script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
  exec "$script_dir/../tools/amiga-docker.sh" usb-poseidon ./build.sh "$@"
fi

# LRA register allocation (-mlra) exists on Bebbo GCC 10+ only; the older
# 6.5.0b toolchain rejects the flag, so enable it where supported.
lra=
if m68k-amigaos-gcc -mlra -x c -fsyntax-only /dev/null 2>/dev/null; then
  lra=-mlra
fi

m68k-amigaos-gcc -m68020 -mtune=68020-60 -msoft-float -s -Wall -Wextra -Wno-unused-parameter -fomit-frame-pointer -nostdlib -nostartfiles -O2 $lra -I. -I../include -o zzusbhw.device zzusbhw_device.c zzusb_engine.c zzusb_iso.c zzusb_policy.c -lgcc -lc
