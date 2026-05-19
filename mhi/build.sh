#!/bin/sh
set -eu

if ! command -v m68k-amigaos-gcc >/dev/null 2>&1; then
  script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
  exec "$script_dir/../tools/amiga-docker.sh" mhi ./build.sh "$@"
fi

export PATH=/opt/amiga/bin:$PATH

m68k-amigaos-gcc StartUp.c LibInit.c mhizz9000.c asmfuncs.s -m68020 -O3 -o mhizz9000.library.debug -g -ggdb -Wall -Wextra -Wno-unused-parameter -Wno-pointer-to-int-cast -Wno-pointer-sign -nostartfiles -ldebug
m68k-amigaos-strip -s -o mhizz9000.library mhizz9000.library.debug
