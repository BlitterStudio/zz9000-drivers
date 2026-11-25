#!/bin/sh
set -eu

# Build ahi/driver/library_lifecycle_test.c with the m68k toolchain and run it
# under vamos. The test compiles the real driver and runs its Open, Close and
# Expunge vectors, init() and AllocAudio against a private library list, so it
# needs no card. Like ahi/driver/build.sh it stages the zz9k.library client
# headers on the host first (the docker image mounts only this repository),
# then runs inside the amiga-compiler container.
#
# Environment: ZZ9000_SDK (an sdk/ subtree root) as for the other staging
# builds; AMIGA_IMAGE as for tools/amiga-docker.sh.

script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH='' cd -- "$script_dir/.." && pwd)

"$script_dir/stage-zz9k-headers.sh" "$repo_root/ahi/driver/zz9k-headers" "$repo_root"

exec "$script_dir/amiga-docker.sh" ahi/driver sh -c \
  'm68k-amigaos-gcc library_lifecycle_test.c voice_model.c asmfuncs.s -O1 -m68020-60 -I../../include -Izz9k-headers -Wl,--allow-multiple-definition -o library_lifecycle_test -ldebug &&
   vamos --cpu=68020 -m 8000 --stack-size=64 library_lifecycle_test'
