#!/bin/sh
set -eu

if ! command -v m68k-amigaos-gcc >/dev/null 2>&1; then
  script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
  exec "$script_dir/../../tools/amiga-docker.sh" ahi/driver ./build.sh "$@"
fi

export PATH=/opt/amiga/bin:"$PATH"

vasmm68k_mot -quiet -phxass -Fhunk -m68020 -o PREFSFILE.uncut prefsfile.a -I/opt/amiga/m68k-amigaos/ndk-include -I/opt/amiga/m68k-amigaos/include

# remove 0x28 bytes from the start
dd bs=1 skip=40 if=PREFSFILE.uncut of=ZZ9000AX

m68k-amigaos-gcc zz9000ax-ahi.c asmfuncs.s -O3 -I../../include -o zz9000ax.audio -Wall -Wextra -Wno-unused-parameter -nostartfiles -m68020 -ldebug
