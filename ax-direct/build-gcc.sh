#!/bin/sh
set -eu

if ! command -v m68k-amigaos-gcc >/dev/null 2>&1; then
  script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
  exec "$script_dir/../tools/amiga-docker.sh" ax-direct ./build-gcc.sh "$@"
fi

export PATH=/opt/amiga/bin:"$PATH"

m68k-amigaos-gcc axmp3.c -m68030 -O2 -o axmp3 -Wall -Wextra -Wno-unused-parameter -lamiga -noixemul

#m68k-amigaos-gcc axwav.c -m68030 -O2 -o axwav -Wall -Wextra -Wno-unused-parameter -lamiga -noixemul
