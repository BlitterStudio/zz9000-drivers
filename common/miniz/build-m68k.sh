#!/bin/sh
# Compile the vendored inflater for m68k-amigaos with the caller's flags.
#
# The Bebbo "bbb" peephole pass miscompiles miniz_tinfl.c (GCC 6.5.0b at
# -O1 and up: a release BOOT.bin inflates with a CRC-32 mismatch), so it is
# always off for this file. GCC 16 accepts the same flag. ZZTop's build
# and common/tests/fw_release_m68k.sh both use this script, so the native
# test covers the production object.
#
# Usage: build-m68k.sh <output.o> [m68k-amigaos-gcc flags...]
set -eu

if [ "$#" -lt 1 ]; then
    echo "Usage: $0 <output.o> [flags...]" >&2
    exit 64
fi
out=$1
shift
script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
m68k-amigaos-gcc -c "$script_dir/miniz_tinfl.c" "$@" -fbbb=- -o "$out"
