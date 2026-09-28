#!/bin/sh
# Build the zz9000-sdk user-facing payloads (zz9k.library, the accelerated
# mpega.library, zz9k-picture.datatype + descriptors, and end-user CLI tools)
# by driving the SDK's own Docker build, then collect them into sdk/out/ in
# installer layout.
#
# Since the SDK consolidation the SDK sources live at sdk/ inside the
# zz9000-firmware repository; SDK_REF pins a firmware commit and this
# script extracts the subtree. Sources stay in the firmware repo
# ("pull, not move"); this repo only consumes build products.
#
# Run on the host (the SDK build script invokes Docker itself — do not wrap
# this in tools/amiga-docker.sh).
#
# Environment:
#   ZZ9000_SDK      use an existing SDK-subtree root (e.g.
#                   ../zz9000-firmware/sdk) as-is; default: a sibling
#                   firmware checkout's sdk/ if present, else clone the
#                   firmware repo into sdk/work/ at the ref in SDK_REF
#                   and use its sdk/
#   SDK_REPO        git URL of the firmware repository used when cloning
#   SDK_SKIP_BUILD  set to 1 to only collect payloads from an SDK tree
#                   already built+packaged (Windows hosts: run the SDK's
#                   scripts\*.ps1 first — Git Bash mangles the .sh variant's
#                   docker paths)
set -eu

here=$(CDPATH='' cd -- "$(dirname "$0")" && pwd)
SDK_REF=$(cat "$here/SDK_REF")
SDK_REPO=${SDK_REPO:-https://github.com/BlitterStudio/zz9000-firmware.git}

if [ -n "${ZZ9000_SDK:-}" ]; then
    src="$ZZ9000_SDK"
    echo ">> Using SDK tree: $src"
elif [ -d "$here/../../zz9000-firmware/sdk/include/zz9k" ]; then
    src=$(CDPATH='' cd -- "$here/../../zz9000-firmware/sdk" && pwd)
    echo ">> Using sibling firmware checkout's SDK tree: $src"
else
    fw="$here/work/zz9000-firmware"
    if [ ! -d "$fw/.git" ]; then
        echo ">> Cloning zz9000-firmware into $fw"
        git clone "$SDK_REPO" "$fw"
    fi
    echo ">> Checking out pinned firmware ref $SDK_REF"
    git -C "$fw" fetch origin 2>/dev/null || true
    git -C "$fw" checkout -f "$SDK_REF"
    src="$fw/sdk"
fi

if [ "${SDK_SKIP_BUILD:-0}" != 1 ]; then
    echo ">> Building + packaging the SDK (Docker)"
    sh "$src/scripts/build-m68k-amigaos.sh"
    sh "$src/scripts/package-m68k-amigaos.sh" --skip-build
fi

pkg="$src/build/package/amigaos3"
out="$here/out"
rm -rf "$out"
mkdir -p "$out/Libs" "$out/Classes/DataTypes" "$out/Storage/DataTypes" "$out/C" "$out/Docs"

cp "$pkg/Libs/zz9k.library"                       "$out/Libs/"
cp "$pkg/Libs/mpega.library"                      "$out/Libs/"
cp "$pkg/Classes/DataTypes/zz9k-picture.datatype" "$out/Classes/DataTypes/"
cp -R "$pkg/Storage/DataTypes/."                  "$out/Storage/DataTypes/"
# End-user CLI tools: runtime diagnostics plus the tools that exercise the
# ZZ9000's accelerated features (image/video viewers, MP3, crypto bench,
# archive).
# The remaining SDK C/ tools are developer-oriented and ship with the SDK
# package instead.
for tool in zz9k-info zz9k-services zz9k-view zz9k-mp3 zz9k-cryptobench zz9k-archive; do
    cp "$pkg/C/$tool" "$out/C/"
done
# ZZPlay is a Workbench application, not a CLI tool: it ships capitalised and
# with an icon, and the installer puts it in SYS:Utilities rather than C:.
cp "$pkg/C/ZZPlay"      "$out/C/"
cp "$pkg/C/ZZPlay.info" "$out/C/"
# ZZPlay's manual, and the project icon the manual tells people to copy next
# to a media file. The SDK stages several developer references into Docs/ too;
# only these two are end-user material and belong in the installer.
cp "$pkg/Docs/ZZPlay.guide"         "$out/Docs/"
cp "$pkg/Docs/ZZPlay-project.info" "$out/Docs/"

echo ">> SDK payloads collected:"
find "$out" -type f | sort
