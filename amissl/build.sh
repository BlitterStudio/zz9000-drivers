#!/bin/sh
# Build the ZZ9000-accelerated amissl.library (the zz9000-sdk crypto-offload
# provider compiled into AmiSSL) and collect it into amissl/out/.
#
# Run on the host: it builds the adtools toolchain image (amissl/Dockerfile)
# and clones both source trees onto the container's own filesystem — the
# 2017 adtools gcc cannot read Windows bind-mount inodes, and the sources
# must be LF. Expect a long first build (full AmiSSL + OpenSSL 3 for m68k).
#
# Environment:
#   ZZ9000_SDK  existing SDK-subtree root (default: sibling firmware
#               checkout's sdk/, else clone the firmware repo at sdk/SDK_REF)
#   OS          Build only this AmiSSL target (os3-68020 or os3-68060).
#               Default: build BOTH, the two CPU variants AmiSSL itself ships
#               (tools/mkrelease.sh) — 68020-40 covers 68020/030/040(/080),
#               68060 its own. The installer picks the matching one by CPU.
set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH='' cd -- "$here/.." && pwd)
SDK_REF=$(cat "$repo_root/sdk/SDK_REF")
SDK_REPO=${SDK_REPO:-https://github.com/BlitterStudio/zz9000-firmware.git}
# One or both AmiSSL CPU targets. AmiSSL's mkrelease.sh maps these to the
# Libs/AmiSSL/<libdir>/ layout the installer expects (os3-68020 -> 68020-40,
# os3-68060 -> 68060); we mirror it exactly.
TARGETS=${OS:-"os3-68020 os3-68060"}

if [ -n "${ZZ9000_SDK:-}" ]; then
    # The override points at an SDK-subtree root (e.g.
    # ../zz9000-firmware/sdk); derive the firmware repo root from it.
    fw=$(CDPATH='' cd -- "$ZZ9000_SDK/.." && pwd)
elif [ -d "$repo_root/../zz9000-firmware/sdk/include/zz9k" ]; then
    fw=$(CDPATH='' cd -- "$repo_root/../zz9000-firmware" && pwd)
else
    fw="$here/work/zz9000-firmware"
    if [ ! -d "$fw/.git" ]; then
        git clone "$SDK_REPO" "$fw"
    fi
    git -C "$fw" fetch origin 2>/dev/null || true
    git -C "$fw" checkout -f "$SDK_REF"
fi
echo ">> firmware checkout: $fw (SDK at $fw/sdk)"

echo ">> Building adtools image"
docker build -t zz9000-amissl-adtools "$here"

mkdir -p "$here/out"
echo ">> Building amissl.library for: $TARGETS — this takes a while"
# The sdk/ subtree has no .git of its own, so the Docker mount carries
# the full firmware repo and the container's internal clone points
# ZZ9000_SDK at the clone's sdk/ subdirectory.
docker run --rm \
    -v "$fw":/fw-src:ro \
    -v "$here/out":/out \
    zz9000-amissl-adtools sh -ec '
        git config --global --add safe.directory "*"
        git clone -q /fw-src /build/zz9000-firmware
        for os in '"$TARGETS"'; do
            case "$os" in
                os3-68020) libdir=68020-40 ;;
                os3-68060) libdir=68060 ;;
                *) echo "!! unknown OS target: $os" >&2; exit 1 ;;
            esac
            echo ">> Building $os -> out/$libdir/"
            AMISSL_DIR= ZZ9000_SDK=/build/zz9000-firmware/sdk OS="$os" \
                WORK=/build/work \
                sh /build/zz9000-firmware/sdk/integration/amissl/build.sh
            mkdir -p "/out/$libdir"
            cp -v /build/work/out/"$os"/amissl_v*.library "/out/$libdir/"
        done
    '

echo ">> Done:"
ls -lR "$here/out/"
