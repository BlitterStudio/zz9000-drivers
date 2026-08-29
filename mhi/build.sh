#!/bin/sh
set -eu

script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)

# When the m68k toolchain isn't on PATH this script re-execs itself inside
# the amiga-compiler container (see the docker re-exec below) and passes this
# sentinel to mark that second stage. The host leg has already staged the
# zz9k headers into mhi/zz9k-headers (mounted into the container), so the
# second stage must NOT stage again -- inside that container only this repo
# is mounted, so the sibling checkout / SDK-clone sources are unreachable.
# We key off the sentinel rather than /.dockerenv because a dev container (or
# a direct `docker run ... ./build.sh`) is a first-stage host that still has
# to stage: /.dockerenv would wrongly suppress staging there.
staged=0
if [ "${1:-}" = "--zz9k-staged" ]; then
  staged=1
  shift
fi

# Stage the zz9k.library client headers from a zz9000-sdk checkout so the
# docker build (which mounts only this repo) can include them. The host
# leg stages (tools/stage-zz9k-headers.sh: ZZ9000_SDK override, else a
# sibling checkout, else the pinned sdk/SDK_REF clone into the shared
# sdk/work tree); the re-exec'd second stage must not -- see the sentinel
# note above.
if [ "$staged" -eq 0 ]; then
  "$script_dir/../tools/stage-zz9k-headers.sh" \
    "$script_dir/zz9k-headers" "$script_dir/.."
fi
# Fail with a clear message instead of a compile error when the staged SDK
# predates the resumable audio-stream drain API this driver is built on.
if ! grep -q "ZZ9K_LIBRARY_MIN_REVISION_AUDIO_STREAM_DRAIN" \
    "$script_dir/zz9k-headers/zz9k/library_vectors.h"; then
  echo "ERROR: the staged zz9000-sdk headers lack audio-stream drain support" >&2
  echo "       (ZZ9K_LIBRARY_MIN_REVISION_AUDIO_STREAM_DRAIN). Point ZZ9000_SDK" >&2
  echo "       at a checkout that includes the resumable-drain changes." >&2
  exit 1
fi

# Same discipline for the firmware-authoritative control plane this driver
# submits its source trim through.
if ! grep -q "ZZ9K_OP_AUDIO_TRIM_SUBMIT" \
    "$script_dir/zz9k-headers/zz9k/abi.h"; then
  echo "ERROR: the staged zz9000-sdk headers lack audio control-plane support" >&2
  echo "       (ZZ9K_OP_AUDIO_TRIM_SUBMIT). Point ZZ9000_SDK at a checkout" >&2
  echo "       that includes the control-plane ABI definitions." >&2
  exit 1
fi

cd "$script_dir"

if ! command -v m68k-amigaos-gcc >/dev/null 2>&1; then
  exec "$script_dir/../tools/amiga-docker.sh" mhi ./build.sh --zz9k-staged "$@"
fi

export PATH=/opt/amiga/bin:"$PATH"

m68k-amigaos-gcc StartUp.c LibInit.c mhizz9000.c asmfuncs.s -m68020 -O3 -I../include -Izz9k-headers -o mhizz9000.library.debug -g -ggdb -Wall -Wextra -Wno-unused-parameter -Wno-pointer-to-int-cast -Wno-pointer-sign -nostartfiles -ldebug
m68k-amigaos-strip -s -o mhizz9000.library mhizz9000.library.debug

# Trace variant: same driver with KPrintF tracing compiled in; capture
# the output on the Amiga with Sashimi. Swap it in for mhizz9000.library
# when diagnosing player behaviour.
m68k-amigaos-gcc StartUp.c LibInit.c mhizz9000.c asmfuncs.s -m68020 -O3 -DZZ_MHI_TRACE=1 -I../include -Izz9k-headers -o mhizz9000.library.trace.debug -g -ggdb -Wall -Wextra -Wno-unused-parameter -Wno-pointer-to-int-cast -Wno-pointer-sign -nostartfiles -ldebug
m68k-amigaos-strip -s -o mhizz9000.library.trace mhizz9000.library.trace.debug

# Decode-only diagnostic: exercises the MHI feeder and accelerated decoder but
# suppresses AX binding and drains PCM through READ. It is intentionally silent
# and is packaged only for feeder-vs-pump hardware isolation.
m68k-amigaos-gcc StartUp.c LibInit.c mhizz9000.c asmfuncs.s -m68020 -O3 -DZZ_MHI_TRACE=1 -DZZ_MHI_DIAG_DECODE_ONLY=1 -I../include -Izz9k-headers -o mhizz9000.library.decode-only.debug -g -ggdb -Wall -Wextra -Wno-unused-parameter -Wno-pointer-to-int-cast -Wno-pointer-sign -nostartfiles -ldebug
m68k-amigaos-strip -s -o mhizz9000.library.decode-only mhizz9000.library.decode-only.debug
