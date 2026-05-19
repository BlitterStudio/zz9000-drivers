#!/bin/sh
set -eu

if ! command -v m68k-amigaos-gcc >/dev/null 2>&1; then
  script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
  exec "$script_dir/../tools/amiga-docker.sh" usb-poseidon ./build.sh "$@"
fi

export PATH=/opt/amiga/bin:$PATH

m68k-amigaos-gcc -m68020 -mtune=68020-60 -msoft-float -s -Wall -Wextra -Wno-unused-parameter -fomit-frame-pointer -nostdlib -nostartfiles -O2 -I. -o zzusbhw.device zzusbhw_device.c -lgcc -lc
