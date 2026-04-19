#!/bin/bash
# To trace driver activity on the Zorro serial debug register, build with
# DEBUG=1 (or any non-empty value). Without it, debugstr()/debughex() are
# compile-time no-ops and the driver is ~500 bytes smaller.
DEBUG_FLAG=""
if [ -n "${DEBUG:-}" ]; then
  DEBUG_FLAG="-DZZSD_DRIVER_DEBUG=1"
fi

docker run --rm -v "$(pwd)":/work amigadev/crosstools:m68k-amigaos \
  m68k-amigaos-gcc -m68000 -s -Wall -Wextra -Wno-unused-parameter \
  -fomit-frame-pointer -nostdlib -nostartfiles -Os \
  $DEBUG_FLAG \
  -o zzsd.device zzsd_device.c zzsd_cmd.c zzsd_boot.c

xxd -i -a zzsd.device > zzsd-device.h
