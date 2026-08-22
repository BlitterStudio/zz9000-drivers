#!/bin/sh
set -eu

# When the m68k toolchain isn't on PATH this script re-execs itself inside
# the amiga-compiler container (see the docker re-exec below) and passes this
# sentinel to mark that second stage. The host leg has already staged the
# zz9k headers into ahi/driver/zz9k-headers (mounted into the container), so
# the second stage must NOT stage again -- inside that container only this
# repo is mounted, so the sibling checkout / SDK-clone sources are
# unreachable. We key off the sentinel rather than /.dockerenv because a dev
# container (or a direct `docker run ... ./build.sh`) is a first-stage host
# that still has to stage: /.dockerenv would wrongly suppress staging there.
staged=0
if [ "${1:-}" = "--zz9k-staged" ]; then
  staged=1
  shift
fi

# Stage the zz9k.library client headers from a zz9000-sdk checkout so the
# docker build (which mounts only this repo) can include them. The host
# leg stages (tools/stage-zz9k-headers.sh: ZZ9000_SDK override, else a
# sibling checkout, else the pinned sdk/SDK_REF clone); the re-exec'd
# second stage must not -- see the sentinel note above.
script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
if [ "$staged" -eq 0 ]; then
  "$script_dir/../../tools/stage-zz9k-headers.sh" \
    "$script_dir/zz9k-headers" "$script_dir/../.."
fi
# Fail with a clear message instead of a compile error when the staged SDK
# predates the audio control plane this driver submits its source trim
# through (ZZ9K_OP_AUDIO_TRIM_SUBMIT / ZZ9K_CAP_AUDIO_CONTROL).
if ! grep -q "ZZ9K_OP_AUDIO_TRIM_SUBMIT" \
    "$script_dir/zz9k-headers/zz9k/abi.h"; then
  echo "ERROR: the staged zz9000-sdk headers lack audio control-plane support" >&2
  echo "       (ZZ9K_OP_AUDIO_TRIM_SUBMIT). Point ZZ9000_SDK at a checkout" >&2
  echo "       that includes the control-plane ABI definitions." >&2
  exit 1
fi

if ! command -v m68k-amigaos-gcc >/dev/null 2>&1; then
  exec "$script_dir/../../tools/amiga-docker.sh" ahi/driver ./build.sh --zz9k-staged "$@"
fi

export PATH=/opt/amiga/bin:"$PATH"

vasmm68k_mot -quiet -phxass -Fhunk -m68020 -o PREFSFILE.uncut prefsfile.a -I/opt/amiga/m68k-amigaos/ndk-include -I/opt/amiga/m68k-amigaos/include

# remove 0x28 bytes from the start
dd bs=1 skip=40 if=PREFSFILE.uncut of=ZZ9000AX

m68k-amigaos-gcc zz9000ax-ahi.c asmfuncs.s -O3 -I../../include -Izz9k-headers -o zz9000ax.audio -Wall -Wextra -Wno-unused-parameter -nostartfiles -m68020 -ldebug
