#!/bin/sh
# Test of the ZZ9000AX driver for WHDLoad.Audio.
#
# Assembles the driver with tests/harness.s into a flat blob (m68k toolchain
# container, see ../build.sh) and runs it under qemu-m68k with
# zz9000_audio_test.c, which provides exec, the libraries and a model of the
# firmware's audio fabric lease. Exit status 0 = all checks passed.
#
# With m68k-linux-gnu-gcc and qemu-m68k on the PATH the second step runs
# there, otherwise in a container (HARNESS_IMAGE, default ubuntu:24.04, which
# gets gcc-m68k-linux-gnu, libc6-dev-m68k-cross and qemu-user from apt).
# QEMU_CPU selects the CPU, default m68040.
set -eu

script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
dir=$(CDPATH='' cd -- "$script_dir/.." && pwd)
cd "$dir"

step=${1:-all}

if [ "$step" = all ]; then
    ./build.sh harness
    exec sh tests/run-tests.sh run
fi

mkdir -p out
if command -v m68k-linux-gnu-gcc >/dev/null 2>&1 &&
   command -v qemu-m68k >/dev/null 2>&1; then
    m68k-linux-gnu-gcc -std=gnu11 -O1 -g -Wall -Wextra -Werror -static \
        -o out/zz9000_audio_test tests/zz9000_audio_test.c
    exec qemu-m68k -cpu "${QEMU_CPU:-m68040}" out/zz9000_audio_test \
        out/harness.bin
fi

image=${HARNESS_IMAGE:-ubuntu:24.04}
engine=${CONTAINER_ENGINE:-docker}
mount=$dir
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        mount=$(cygpath -w "$dir")
        export MSYS_NO_PATHCONV=1
        ;;
esac
# shellcheck disable=SC2016
exec "$engine" run --rm -v "$mount":/w -w /w -e QEMU_CPU="${QEMU_CPU:-m68040}" \
    "$image" sh -c '
set -eu
if ! command -v m68k-linux-gnu-gcc >/dev/null 2>&1 ||
   ! command -v qemu-m68k >/dev/null 2>&1; then
    apt-get update -qq
    DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends \
        gcc-m68k-linux-gnu libc6-dev-m68k-cross qemu-user >/dev/null
fi
exec sh tests/run-tests.sh run
'
