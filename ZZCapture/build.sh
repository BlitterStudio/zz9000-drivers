#!/bin/sh
set -eu

if ! command -v m68k-amigaos-gcc >/dev/null 2>&1; then
    script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
    exec "$script_dir/../tools/amiga-docker.sh" ZZCapture ./build.sh "$@"
fi

m68k-amigaos-gcc ZZCapture.c ../common/zz_vcap_client.c ../common/zz_vcap_live.c -m68030 -O2 -o ZZCapture \
    -Wall -Wextra -I../common -I../include -lamiga -noixemul
