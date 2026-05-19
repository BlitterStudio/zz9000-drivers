#!/bin/sh
set -eu

if ! command -v m68k-amigaos-gcc >/dev/null 2>&1; then
  script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
  exec "$script_dir/../tools/amiga-docker.sh" ZZTop ./build-gcc.sh "$@"
fi

export PATH=/opt/amiga/bin:"$PATH"

m68k-amigaos-gcc Sources/ZZTop.c -m68030 -O2 -o ZZTop -Wall -Wextra -Wno-unused-parameter -lamiga -noixemul -lm
