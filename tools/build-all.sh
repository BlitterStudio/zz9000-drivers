#!/bin/sh
set -eu

script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH='' cd -- "$script_dir/.." && pwd)
amiga_docker="$script_dir/amiga-docker.sh"

cd "$repo_root"

"$amiga_docker" rtg ./build.sh
"$amiga_docker" ZZTop ./build-gcc.sh
"$amiga_docker" ZZScanlines m68k-amigaos-gcc -O2 -noixemul -I../include \
    -o ZZScanlines ZZScanlines.c -lamiga
"$amiga_docker" ZZFwUpdate m68k-amigaos-gcc -O2 -noixemul -Wall -Wextra \
    -I../include -o ZZFwUpdate ZZFwUpdate.c -lamiga
"$amiga_docker" usb-poseidon ./build.sh
"$amiga_docker" sd-boot ./build.sh
"$amiga_docker" net make
"$amiga_docker" net/ZZNetStats m68k-amigaos-gcc -O2 -noixemul -Wall -Wextra \
    -Wno-unused-parameter -I../../include -o ZZNetStats ZZNetStats.c -lamiga
"$amiga_docker" mhi ./build.sh
"$amiga_docker" ahi/driver ./build.sh
"$amiga_docker" ax-direct ./build-gcc.sh
"$amiga_docker" ZZDiag ./build.sh

# Runs its own Docker invocation (the SDK's build scripts wrap the same
# image) — called directly, not through amiga-docker.sh.
sdk/build.sh

# amissl/build.sh is intentionally NOT part of build-all: it is a full
# AmiSSL + OpenSSL cross-build (slow, separate adtools toolchain image).
# Run it explicitly when cutting a release; package-local.sh stages its
# output when present.

make -C rtg/tests test
tools/check-release.sh --quick
