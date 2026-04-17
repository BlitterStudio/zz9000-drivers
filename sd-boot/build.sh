#!/bin/bash
docker run --rm -v "$(pwd)":/work amigadev/crosstools:m68k-amigaos \
  m68k-amigaos-gcc -m68000 -s -Wall -Wextra -Wno-unused-parameter \
  -fomit-frame-pointer -nostdlib -nostartfiles -Os \
  -o zzsd.device zzsd_device.c zzsd_cmd.c zzsd_boot.c

xxd -i -a zzsd.device > zzsd-device.h
