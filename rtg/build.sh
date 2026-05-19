#!/bin/sh
set -eu

if ! command -v m68k-amigaos-gcc >/dev/null 2>&1; then
  script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
  exec "$script_dir/../tools/amiga-docker.sh" rtg ./build.sh "$@"
fi

export PATH=/opt/amiga/bin:$PATH

m68k-amigaos-gcc mntgfx-gcc.c -m68020 -mtune=68020-60 -O2 -o ZZ9000.card -noixemul -Wall -Wextra -Wno-unused-parameter -fomit-frame-pointer -nostartfiles -lamiga
