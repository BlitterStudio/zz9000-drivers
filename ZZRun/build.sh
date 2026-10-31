#!/bin/sh
set -eu

# Host leg stages zz9k client headers and zzapp_proto.h, then re-execs in
# the compiler image. The container only sees this repo, so the second
# stage must not stage again.
staged=0
if [ "${1:-}" = "--staged" ]; then
    staged=1
    shift
fi

script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH='' cd -- "$script_dir/.." && pwd)
proto_rel=ZZ9000_proto.sdk/ZZ9000OS/src/zzapp_proto.h
stage_dir=$script_dir/zz9k-headers

stage_proto() {
    firmware=${ZZ9000_FIRMWARE:-"$repo_root/../zz9000-app-runtime"}
    src=$firmware/$proto_rel

    if [ ! -s "$src" ]; then
        echo "ERROR: stage zzapp_proto.h from feature/app-runtime with" >&2
        echo "       ZZ9000_FIRMWARE=/path/to/zz9000-firmware" >&2
        exit 1
    fi
    cp "$src" "$stage_dir/zzapp_proto.h"
}

if [ "$staged" -eq 0 ]; then
    "$repo_root/tools/stage-zz9k-headers.sh" "$stage_dir" "$repo_root"
    stage_proto
fi

if [ ! -f "$stage_dir/zzapp_proto.h" ]; then
    echo "ERROR: zzapp_proto.h was not staged" >&2
    exit 1
fi

if ! command -v m68k-amigaos-gcc >/dev/null 2>&1; then
    exec "$repo_root/tools/amiga-docker.sh" ZZRun ./build.sh --staged "$@"
fi

m68k-amigaos-gcc ZZRun.c -m68030 -O2 -std=gnu11 -o ZZRun \
    -Wall -Wextra -Wno-unused-parameter \
    -I../include -Izz9k-headers \
    -lamiga -noixemul
