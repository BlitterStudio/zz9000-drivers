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
# docker build (which mounts only this repo) can include them. Mirrors
# mhi/build.sh: ZZ9000_SDK override, else a sibling checkout as-is, else
# clone/reuse the SDK at the pinned sdk/SDK_REF. Runs on the host before the
# docker re-exec below; inside the container the staged copy is already
# present.
script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
sdk_src=${ZZ9000_SDK:-}
if [ "$staged" -eq 1 ]; then
  # Second-stage (re-exec'd) invocation: the host leg already staged the
  # headers into ahi/driver/zz9k-headers (validated below); never clone here.
  sdk_src=
else
  if [ -z "$sdk_src" ] && [ -d "$script_dir/../../../zz9000-sdk/include/zz9k" ]; then
    sdk_src="$script_dir/../../../zz9000-sdk"
  fi
  if [ -z "$sdk_src" ] && command -v git >/dev/null 2>&1; then
    SDK_REF=$(cat "$script_dir/../../sdk/SDK_REF")
    SDK_REPO=${SDK_REPO:-https://github.com/BlitterStudio/zz9000-sdk.git}
    sdk_src="$script_dir/../../sdk/work/zz9000-sdk"
    if [ ! -d "$sdk_src/.git" ]; then
      echo ">> Cloning zz9000-sdk into $sdk_src"
      git clone "$SDK_REPO" "$sdk_src"
    fi
    echo ">> Checking out pinned ref $SDK_REF"
    git -C "$sdk_src" fetch origin 2>/dev/null || true
    git -C "$sdk_src" checkout -f "$SDK_REF"
  fi
fi
if [ -n "$sdk_src" ] && [ -d "$sdk_src/include/zz9k" ]; then
  rm -rf "$script_dir/zz9k-headers"
  mkdir -p "$script_dir/zz9k-headers/zz9k" \
           "$script_dir/zz9k-headers/proto" \
           "$script_dir/zz9k-headers/clib"
  cp -r "$sdk_src/include/zz9k/." "$script_dir/zz9k-headers/zz9k/"
  cp -r "$sdk_src/host/include/zz9k/." "$script_dir/zz9k-headers/zz9k/"
  cp -r "$sdk_src/amiga/include/zz9k/." "$script_dir/zz9k-headers/zz9k/"
  cp "$sdk_src/amiga/include/proto/zz9k.h" "$script_dir/zz9k-headers/proto/"
  cp "$sdk_src/amiga/include/clib/zz9k_protos.h" "$script_dir/zz9k-headers/clib/"
fi
if [ ! -d "$script_dir/zz9k-headers/zz9k" ]; then
  echo "ERROR: zz9k headers not staged. Provide a zz9000-sdk checkout as a" >&2
  echo "       sibling directory or set ZZ9000_SDK=/path/to/zz9000-sdk." >&2
  exit 1
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
