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
# GCC 16 (amigadev/crosstools), pinned by digest because the tag follows
# release candidates. This is the only pin: CI builds through this wrapper,
# and tools/m68k-toolchain-pin.sh moves it.
image=${AMIGA_IMAGE:-amigadev/crosstools:m68k-amigaos@sha256:a018cd1de600ae1c704f2eb3501f86843636f9953dd4aba6cffdf62cd7d9e8bb}
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

# The image has the cross tools under /opt/m68k-amigaos/bin. No login shell:
# it aborts `sh -lc` on a missing /root/.cargo/env.
# shellcheck disable=SC2016
exec "$engine" run --rm \
    -v "$repo_root":/src \
    -w "/src/$workdir" \
    "$image" \
    sh -c 'export PATH=/opt/m68k-amigaos/bin:$PATH; exec "$@"' sh "$@"
