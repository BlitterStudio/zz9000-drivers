#!/bin/sh
# Build the zz9000-sdk user-facing payloads (zz9k.library, the accelerated
# mpega.library, zz9k-picture.datatype + descriptors, and end-user CLI tools)
# by driving the SDK's own Docker build, then collect them into sdk/out/ in
# installer layout. Sources stay in the zz9000-sdk repo ("pull, not move");
# this repo only consumes its build products.
#
# Run on the host (the SDK build script invokes Docker itself — do not wrap
# this in tools/amiga-docker.sh).
#
# Environment:
#   ZZ9000_SDK      use an existing zz9000-sdk checkout as-is (default: a
#                   sibling ../zz9000-sdk checkout if present, else clone into
#                   sdk/work/ at the ref pinned in SDK_REF)
#   SDK_REPO        git URL used when cloning
#   SDK_SKIP_BUILD  set to 1 to only collect payloads from an SDK checkout
#                   already built+packaged (Windows hosts: run the SDK's
#                   scripts\*.ps1 first — Git Bash mangles the .sh variant's
#                   docker paths)
set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
SDK_REF=$(cat "$here/SDK_REF")
SDK_REPO=${SDK_REPO:-https://github.com/BlitterStudio/zz9000-sdk.git}

if [ -n "${ZZ9000_SDK:-}" ]; then
    src="$ZZ9000_SDK"
    echo ">> Using zz9000-sdk checkout: $src"
elif [ -d "$here/../../zz9000-sdk/.git" ]; then
    src=$(CDPATH='' cd -- "$here/../../zz9000-sdk" && pwd)
    echo ">> Using sibling zz9000-sdk checkout: $src"
else
    src="$here/work/zz9000-sdk"
    if [ ! -d "$src/.git" ]; then
        echo ">> Cloning zz9000-sdk into $src"
        git clone "$SDK_REPO" "$src"
    fi
    echo ">> Checking out pinned ref $SDK_REF"
    git -C "$src" fetch origin 2>/dev/null || true
    git -C "$src" checkout -f "$SDK_REF"
fi

if [ "${SDK_SKIP_BUILD:-0}" != 1 ]; then
    echo ">> Building + packaging the SDK (Docker)"
    sh "$src/scripts/build-m68k-amigaos.sh"
    sh "$src/scripts/package-m68k-amigaos.sh" --skip-build
fi

pkg="$src/build/package/amigaos3"
out="$here/out"
rm -rf "$out"
mkdir -p "$out/Libs" "$out/Classes/DataTypes" "$out/Storage/DataTypes" "$out/C"

cp "$pkg/Libs/zz9k.library"                       "$out/Libs/"
cp "$pkg/Libs/mpega.library"                      "$out/Libs/"
cp "$pkg/Classes/DataTypes/zz9k-picture.datatype" "$out/Classes/DataTypes/"
cp -R "$pkg/Storage/DataTypes/."                  "$out/Storage/DataTypes/"
# End-user CLI tools: runtime diagnostics plus the tools that exercise the
# ZZ9000's accelerated features (image viewers, MP3, crypto bench, archive).
# The remaining SDK C/ tools are developer-oriented and ship with the SDK
# package instead.
for tool in zz9k-info zz9k-services zz9k-view zz9k-mp3 zz9k-cryptobench zz9k-archive; do
    cp "$pkg/C/$tool" "$out/C/"
done

echo ">> SDK payloads collected:"
find "$out" -type f | sort
