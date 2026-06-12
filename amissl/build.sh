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
#   OS          AmiSSL target: os3-68020 (default) or os3-68060
set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH='' cd -- "$here/.." && pwd)
SDK_REF=$(cat "$repo_root/sdk/SDK_REF")
SDK_REPO=${SDK_REPO:-https://github.com/BlitterStudio/zz9000-sdk.git}
OS=${OS:-os3-68020}

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
echo ">> Building amissl.library ($OS) — this takes a while"
docker run --rm \
    -v "$src":/sdk-src:ro \
    -v "$here/out":/out \
    zz9000-amissl-adtools sh -ec '
        git config --global --add safe.directory "*"
        git clone -q /sdk-src /build/zz9000-sdk
        AMISSL_DIR= ZZ9000_SDK=/build/zz9000-sdk OS='"$OS"' \
            WORK=/build/work \
            sh /build/zz9000-sdk/integration/amissl/build.sh
        cp -v /build/work/out/'"$OS"'/amissl_v*.library /out/
    '

echo ">> Done:"
ls -l "$here/out/"
