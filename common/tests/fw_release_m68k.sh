#!/bin/sh
# Native m68k check of the release-ZIP extractor: build fw_release.c and the
# vendored inflater the way ZZTop does, run them under vamos on a generated
# release archive and compare the extracted BOOT.bin with the original.
# Host tests cannot catch m68k code generation faults; GCC 6.5.0b's "bbb"
# pass, for one, silently corrupts the inflater (common/miniz/build-m68k.sh).
#
# Needs the m68k toolchain, vamos and python3; re-execs in the Amiga
# toolchain container (AMIGA_IMAGE) when they are not on PATH.
set -eu

script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
if ! command -v m68k-amigaos-gcc >/dev/null 2>&1; then
    exec "$script_dir/../../tools/amiga-docker.sh" common/tests \
        ./fw_release_m68k.sh "$@"
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
flags="-m68030 -O2 -noixemul -Wall -Wextra -I$script_dir/.. \
  -I$script_dir/../../include"

want=$(python3 "$script_dir/make_release_zip.py" "$tmp/release.zip")
# shellcheck disable=SC2086
sh "$script_dir/../miniz/build-m68k.sh" "$tmp/miniz_tinfl.o" $flags
# shellcheck disable=SC2086
m68k-amigaos-gcc $flags -Werror -o "$tmp/extract" \
    "$script_dir/fw_release_m68k.c" "$script_dir/../fw_release.c" \
    "$tmp/miniz_tinfl.o"

(cd "$tmp" && vamos -C 68020 -m 8000 -V "work:$tmp" extract \
    work:release.zip work:BOOT.bin)
got=$(sha256sum "$tmp/BOOT.bin" | cut -d' ' -f1)
if [ "$got" != "$want" ]; then
    echo "FAIL: m68k extraction differs (want $want, got $got)" >&2
    exit 1
fi
echo "fw_release m68k: BOOT.bin extracted intact"
