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
#   ZZ9000_SDK  existing zz9000-sdk checkout (default: sibling ../zz9000-sdk,
#               else clone at sdk/SDK_REF)
#   OS          Build only this AmiSSL target (os3-68020 or os3-68060).
#               Default: build BOTH, the two CPU variants AmiSSL itself ships
#               (tools/mkrelease.sh) — 68020-40 covers 68020/030/040(/080),
#               68060 its own. The installer picks the matching one by CPU.
set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH='' cd -- "$here/.." && pwd)
SDK_REF=$(cat "$repo_root/sdk/SDK_REF")
SDK_REPO=${SDK_REPO:-https://github.com/BlitterStudio/zz9000-sdk.git}
# One or both AmiSSL CPU targets. AmiSSL's mkrelease.sh maps these to the
# Libs/AmiSSL/<libdir>/ layout the installer expects (os3-68020 -> 68020-40,
# os3-68060 -> 68060); we mirror it exactly.
TARGETS=${OS:-"os3-68020 os3-68060"}

if [ -n "${ZZ9000_SDK:-}" ]; then
    src="$ZZ9000_SDK"
elif [ -d "$repo_root/../zz9000-sdk/.git" ]; then
    src=$(CDPATH='' cd -- "$repo_root/../zz9000-sdk" && pwd)
else
    src="$here/work/zz9000-sdk"
    if [ ! -d "$src/.git" ]; then
        git clone "$SDK_REPO" "$src"
    fi
    git -C "$src" fetch origin 2>/dev/null || true
    git -C "$src" checkout -f "$SDK_REF"
fi
echo ">> zz9000-sdk checkout: $src"

echo ">> Building adtools image"
docker build -t zz9000-amissl-adtools "$here"

mkdir -p "$here/out"
echo ">> Building amissl.library for: $TARGETS — this takes a while"
docker run --rm \
    -v "$src":/sdk-src:ro \
    -v "$here/out":/out \
    zz9000-amissl-adtools sh -ec '
        git config --global --add safe.directory "*"
        git clone -q /sdk-src /build/zz9000-sdk
        for os in '"$TARGETS"'; do
            case "$os" in
                os3-68020) libdir=68020-40 ;;
                os3-68060) libdir=68060 ;;
                *) echo "!! unknown OS target: $os" >&2; exit 1 ;;
            esac
            echo ">> Building $os -> out/$libdir/"
            AMISSL_DIR= ZZ9000_SDK=/build/zz9000-sdk OS="$os" \
                WORK=/build/work \
                sh /build/zz9000-sdk/integration/amissl/build.sh
            mkdir -p "/out/$libdir"
            cp -v /build/work/out/"$os"/amissl_v*.library "/out/$libdir/"
        done
    '

echo ">> Done:"
ls -lR "$here/out/"
