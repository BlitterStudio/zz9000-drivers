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
# GCC 16.2.1b (AmigaDev 16.2-rc23-20), the compiler the A4000 runs passed
# with. The tag follows release candidates, so the digest pins it. CI's
# AMIGA_IMAGE (.github/workflows/ci.yml) names the same image.
image=${AMIGA_IMAGE:-amigadev/crosstools:m68k-amigaos-gcc10@sha256:47b5556f434ea10e6837c0db3011357f9a08cbc96cdb9c86cd84412cd0456fe1}
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
