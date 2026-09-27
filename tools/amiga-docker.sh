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
# Git Bash / MSYS: docker.exe is a Windows binary. Convert the mount to a
# Windows path and stop MSYS from rewriting the POSIX container paths
# (-v splits on ':' as a path list, -w /src gains a C:\ prefix otherwise).
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        repo_root=$(cygpath -w "$repo_root")
        export MSYS_NO_PATHCONV=1
        ;;
esac
image=${AMIGA_IMAGE:-sacredbanana/amiga-compiler:m68k-amigaos}
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

# Expand PATH inside the container, not on the host.
# shellcheck disable=SC2016
exec "$engine" run --rm \
    -v "$repo_root":/src \
    -w "/src/$workdir" \
    "$image" \
    sh -lc 'export PATH=/opt/amiga/bin:$PATH; exec "$@"' sh "$@"
