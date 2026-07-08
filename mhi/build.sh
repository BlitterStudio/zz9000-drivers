#!/bin/sh
set -eu

script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)

# Stage the zz9k.library client headers from the zz9000-sdk checkout so the
# docker build (which mounts only this repo) can include them. Same sibling
# convention as sdk/build.sh; override with ZZ9000_SDK. Runs on the host
# before the docker re-exec below; inside the container the staged copy is
# already present.
sdk_src=${ZZ9000_SDK:-$script_dir/../../zz9000-sdk}
if [ -d "$sdk_src/include/zz9k" ]; then
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
# predates the audio-playback API this driver is built on.
if ! grep -q "ZZ9K_LIBRARY_MIN_REVISION_AUDIO_PLAYBACK" \
    "$script_dir/zz9k-headers/zz9k/library_vectors.h"; then
  echo "ERROR: the staged zz9000-sdk headers lack audio-playback support" >&2
  echo "       (ZZ9K_LIBRARY_MIN_REVISION_AUDIO_PLAYBACK). Point ZZ9000_SDK" >&2
  echo "       at a checkout that includes the audio-playback-op changes." >&2
  exit 1
fi

cd "$script_dir"

if ! command -v m68k-amigaos-gcc >/dev/null 2>&1; then
  exec "$script_dir/../tools/amiga-docker.sh" mhi ./build.sh "$@"
fi

export PATH=/opt/amiga/bin:"$PATH"

m68k-amigaos-gcc StartUp.c LibInit.c mhizz9000.c asmfuncs.s -m68020 -O3 -I../include -Izz9k-headers -o mhizz9000.library.debug -g -ggdb -Wall -Wextra -Wno-unused-parameter -Wno-pointer-to-int-cast -Wno-pointer-sign -nostartfiles -ldebug
m68k-amigaos-strip -s -o mhizz9000.library mhizz9000.library.debug

# Trace variant: same driver with KPrintF tracing compiled in; capture
# the output on the Amiga with Sashimi. Swap it in for mhizz9000.library
# when diagnosing player behaviour.
m68k-amigaos-gcc StartUp.c LibInit.c mhizz9000.c asmfuncs.s -m68020 -O3 -DZZ_MHI_TRACE=1 -I../include -Izz9k-headers -o mhizz9000.library.trace.debug -g -ggdb -Wall -Wextra -Wno-unused-parameter -Wno-pointer-to-int-cast -Wno-pointer-sign -nostartfiles -ldebug
m68k-amigaos-strip -s -o mhizz9000.library.trace mhizz9000.library.trace.debug
