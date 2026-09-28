#!/bin/sh
set -eu

if ! command -v m68k-amigaos-gcc >/dev/null 2>&1; then
    script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
    exec "$script_dir/../tools/amiga-docker.sh" ZZNetReady ./build.sh "$@"
fi

export PATH=/opt/amiga/bin:"$PATH"

m68k-amigaos-gcc ZZNetReady.c -m68020 -O2 -o ZZNetReady \
    -Wall -Wextra -noixemul -lamiga
