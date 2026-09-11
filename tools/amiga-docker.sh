#!/bin/sh
set -eu

if [ "$#" -lt 2 ]; then
    echo "Usage: $0 <repo-subdir> <command> [args...]" >&2
    exit 64
fi

workdir=$1
shift

script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH='' cd -- "$script_dir/.." && pwd)
image=${AMIGA_IMAGE:-amigadev/crosstools:m68k-amigaos-gcc10}
engine=${CONTAINER_ENGINE:-}

if [ -z "$engine" ]; then
    if command -v docker >/dev/null 2>&1; then
        engine=docker
    elif command -v podman >/dev/null 2>&1; then
        engine=podman
    else
        echo "ERROR: neither docker nor podman is available" >&2
        exit 127
    fi
fi

# The image ships the toolchain on its default PATH (GCC 16.2.0b under
# /opt/m68k-amigaos/bin), so no PATH setup is needed here.
exec "$engine" run --rm \
    -v "$repo_root":/src \
    -w "/src/$workdir" \
    "$image" \
    sh -c 'exec "$@"' sh "$@"
