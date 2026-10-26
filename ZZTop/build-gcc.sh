#!/bin/sh
set -eu

# When the m68k toolchain isn't on PATH this script re-execs itself inside
# the amiga-compiler container (see the docker re-exec below) and passes this
# sentinel to mark that second stage. The host leg has already staged the
# zz9k headers into ZZTop/zz9k-headers (mounted into the container), so the
# second stage must NOT stage again -- inside that container only this repo
# is mounted, so the sibling checkout / SDK-clone sources are unreachable.
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
  "$script_dir/../tools/stage-zz9k-headers.sh" \
    "$script_dir/zz9k-headers" "$script_dir/.."
fi
# Fail with a clear message instead of a compile error when the staged SDK
# predates the audio control plane this window talks to
# (ZZ9K_OP_AUDIO_SCENE_SELECT / ZZ9K_CAP_AUDIO_CONTROL).
if ! grep -q "ZZ9K_OP_AUDIO_SCENE_SELECT" \
    "$script_dir/zz9k-headers/zz9k/abi.h"; then
  echo "ERROR: the staged zz9000-sdk headers lack audio control-plane support" >&2
  echo "       (ZZ9K_OP_AUDIO_SCENE_SELECT). Point ZZ9000_SDK at a checkout" >&2
  echo "       that includes the control-plane ABI definitions." >&2
  exit 1
fi

if ! command -v m68k-amigaos-gcc >/dev/null 2>&1; then
  exec "$script_dir/../tools/amiga-docker.sh" ZZTop ./build-gcc.sh --zz9k-staged "$@"
fi

export PATH=/opt/amiga/bin:"$PATH"

# Core/ holds the logic (board, ZZ9000.CFG model, live video, audio control
# plane, Save/Revert/Quit); GadTools/ is the user interface on top of it.
# NO_INLINE_STDARG: the inline varargs macros warn on pointer tag values.
common="../common/fwup_amiga.c ../common/fwup_client.c \
  ../common/zzcfg_amiga.c ../common/zz_vcap_live.c \
  ../common/zz_vcap_client.c ../common/zz_vcap_geometry.c \
  ../common/fw_release.c"
flags="-m68030 -O2 -DNO_INLINE_STDARG -I../common -I../include -Izz9k-headers -ICore \
  -Wall -Wextra -Wno-unused-parameter -Wstack-usage=2048 -Werror"
libs="-lamiga -ldebug -noixemul -lm"

# The release-archive inflater has its own compile rule (see the script).
objdir=$(mktemp -d)
trap 'rm -rf "$objdir"' EXIT
# shellcheck disable=SC2086
sh ../common/miniz/build-m68k.sh "$objdir/miniz_tinfl.o" $flags

# shellcheck disable=SC2086
m68k-amigaos-gcc GadTools/*.c Core/*.c $common "$objdir/miniz_tinfl.o" \
  $flags -IGadTools -o ZZTop $libs
