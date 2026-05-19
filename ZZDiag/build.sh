#!/bin/sh
set -eu

if ! command -v m68k-amigaos-gcc >/dev/null 2>&1; then
    script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
    exec "$script_dir/../tools/amiga-docker.sh" ZZDiag ./build.sh "$@"
fi

export PATH=/opt/amiga/bin:"$PATH"

m68k-amigaos-gcc ZZDiag.c -m68030 -O2 -o ZZDiag \
    -Wall -Wextra -Wno-unused-parameter -I../include -lamiga -noixemul
