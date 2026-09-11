#!/bin/sh
set -eu

script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH='' cd -- "$script_dir/.." && pwd)
amiga_docker="$script_dir/amiga-docker.sh"

cd "$repo_root"

"$amiga_docker" rtg ./build.sh
"$amiga_docker" ZZTop ./build-gcc.sh
# shellcheck disable=SC2016  # $lra expands in the container shell
"$amiga_docker" ZZScanlines sh -c 'lra=; m68k-amigaos-gcc -mlra -x c -fsyntax-only /dev/null 2>/dev/null && lra=-mlra; exec m68k-amigaos-gcc -O2 $lra -noixemul -I../include -o ZZScanlines ZZScanlines.c -lamiga'
# shellcheck disable=SC2016  # $lra expands in the container shell
"$amiga_docker" ZZFwUpdate sh -c 'lra=; m68k-amigaos-gcc -mlra -x c -fsyntax-only /dev/null 2>/dev/null && lra=-mlra; exec m68k-amigaos-gcc -O2 $lra -noixemul -Wall -Wextra -I../include -I../common -o ZZFwUpdate ZZFwUpdate.c ../common/fwup_amiga.c ../common/fwup_client.c -lamiga'
"$amiga_docker" usb-poseidon ./build.sh
"$amiga_docker" sd-boot ./build.sh
"$amiga_docker" net make
# shellcheck disable=SC2016  # $lra expands in the container shell
"$amiga_docker" net/ZZNetStats sh -c 'lra=; m68k-amigaos-gcc -mlra -x c -fsyntax-only /dev/null 2>/dev/null && lra=-mlra; exec m68k-amigaos-gcc -O2 $lra -noixemul -Wall -Wextra -Wno-unused-parameter -I../../include -o ZZNetStats ZZNetStats.c -lamiga'
# mhi/build.sh stages zz9k headers from the sibling zz9000-sdk checkout on
# the host, then re-execs itself through amiga-docker.sh.
mhi/build.sh
"$amiga_docker" ahi/driver ./build.sh
"$amiga_docker" ahi/duplextest ./build.sh
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
