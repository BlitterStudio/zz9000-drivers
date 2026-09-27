#!/bin/sh
set -eu

tag=${1:-local}
script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH='' cd -- "$script_dir/.." && pwd)
staging="$repo_root/zz9000-drivers-$tag"
zipfile="$repo_root/zz9000-drivers-$tag.zip"
inst="$repo_root/installer/ZZ9000Installer"

# Python interpreter: python3 on CI/Linux; Windows may expose a Store
# alias stub that prints an install hint instead of running, so verify
# it actually executes and fall back to python.
if python3 -c "pass" >/dev/null 2>&1; then
    PY=python3
else
    PY=python
fi

cd "$repo_root"
tools/check-release.sh

# Amiga-side manuals ship as AmiGuide databases generated from the
# canonical Markdown sources (raw .md has no Amiga reader). Locate the
# pinned zz9000-sdk checkout exactly like sdk/build.sh: explicit
# ZZ9000_SDK, a sibling checkout, or the sdk/work clone.
if [ -n "${ZZ9000_SDK:-}" ]; then
    sdk_src=$ZZ9000_SDK
elif [ -d "$repo_root/../zz9000-sdk/.git" ]; then
    sdk_src=$(CDPATH='' cd -- "$repo_root/../zz9000-sdk" && pwd)
elif [ -d "$repo_root/sdk/work/.git" ]; then
    sdk_src=$repo_root/sdk/work
else
    echo "ERROR: no zz9000-sdk checkout for md2guide; set ZZ9000_SDK or run sdk/build.sh first" >&2
    exit 1
fi
md2guide=$sdk_src/scripts/md2guide.py
SIBLINGS="--sibling ahi.guide --sibling usb-poseidon.guide --sibling ZZCapture.guide --sibling sdk.guide --sibling amissl.guide --sibling ZZPlay.guide --sibling ZZTop.guide --sibling tools.guide"
make_guide() {
    # shellcheck disable=SC2086
    "$PY" "$md2guide" --name "$2" $SIBLINGS "$repo_root/$1" "$inst/Docs/$2.guide"
}

rm -rf "$staging" "$zipfile"

install -Dm644 rtg/ZZ9000.card                  "$inst/Libs/Picasso96/ZZ9000.card"
install -Dm644 mhi/mhizz9000.library            "$inst/Libs/MHI/mhizz9000.library"
install -Dm644 usb-poseidon/zzusbhw.device      "$inst/Devs/USBHardware/zzusbhw.device"
install -Dm644 net/ZZ9000Net.device             "$inst/Devs/Networks/ZZ9000Net.device"
install -Dm644 ahi/driver/zz9000ax.audio        "$inst/Devs/AHI/zz9000ax.audio"
install -Dm644 ahi/driver/ZZ9000AX              "$inst/Devs/AudioModes/ZZ9000AX"
install -Dm755 ZZTop/ZZTop                      "$inst/Tools/ZZTop"
install -Dm755 ZZScanlines/ZZScanlines          "$inst/Tools/ZZScanlines"
install -Dm755 ZZFwUpdate/ZZFwUpdate            "$inst/Tools/ZZFwUpdate"
install -Dm755 net/ZZNetStats/ZZNetStats        "$inst/Tools/ZZNetStats"
install -Dm755 ZZDiag/ZZDiag                    "$inst/Tools/ZZDiag"
install -Dm755 ZZNetReady/ZZNetReady              "$inst/Tools/ZZNetReady"
install -Dm755 ZZCapture/ZZCapture              "$inst/Tools/ZZCapture"
make_guide ahi/README.md ahi
make_guide usb-poseidon/README.md usb-poseidon
make_guide ZZCapture/README.md ZZCapture
make_guide ZZTop/README.md ZZTop
make_guide docs/cli-tools.md tools
# Diagnostic-only MHI build (feeder-vs-pump hardware isolation); staged
# under Docs, deliberately never installed as the production library.
if [ -f mhi/mhizz9000.library.decode-only ]; then
    install -Dm644 mhi/mhizz9000.library.decode-only \
        "$inst/Docs/mhizz9000.library.decode-only"
fi

# SDK runtime payloads (built by sdk/build.sh from the pinned zz9000-sdk ref).
install -Dm644 sdk/out/Libs/zz9k.library        "$inst/Libs/zz9k.library"
install -Dm644 sdk/out/Libs/mpega.library       "$inst/Libs/mpega.library"
install -Dm644 sdk/out/Classes/DataTypes/zz9k-picture.datatype \
                                                "$inst/Classes/DataTypes/zz9k-picture.datatype"
mkdir -p "$inst/Storage/DataTypes"
cp -R sdk/out/Storage/DataTypes/.               "$inst/Storage/DataTypes/"
# End-user CLI tools collected by sdk/build.sh (diagnostics + feature tools).
for tool in zz9k-info zz9k-services zz9k-view zz9k-mp3 zz9k-cryptobench zz9k-archive; do
    install -Dm755 "sdk/out/C/$tool" "$inst/Tools/$tool"
done
# ZZPlay ships with its icon; the installer puts both in SYS:Utilities.
install -Dm755 sdk/out/C/ZZPlay      "$inst/Tools/ZZPlay"
install -Dm644 sdk/out/C/ZZPlay.info "$inst/Tools/ZZPlay.info"
make_guide sdk/README.md sdk
# ZZPlay ships an end-user manual; the manual also tells people to copy
# the project icon out of Docs/.
install -Dm644 sdk/out/Docs/ZZPlay.guide        "$inst/Docs/ZZPlay.guide"
install -Dm644 sdk/out/Docs/ZZPlay-project.info "$inst/Docs/ZZPlay-project.info"

# Accelerated amissl.library, per CPU (optional: built by amissl/build.sh,
# slow). Laid out exactly like AmiSSL's own release — Libs/AmiSSL/<cpu>/ — so
# the installer can pick the build matching the host CPU (68020-40 covers
# 68020/030/040(/080); 68060 its own). An os3-68020 lib on a 68060 traps and
# emulates 64-bit multiplies in software (~2.5x slower crypto), so this match
# matters.
staged_amissl=0
# Drop any stale flat-layout copy from an earlier packaging run.
rm -f "$inst/Libs/AmiSSL/amissl_v362.library"
for cpu in 68020-40 68060; do
    if [ -f "amissl/out/$cpu/amissl_v362.library" ]; then
        install -Dm644 "amissl/out/$cpu/amissl_v362.library" \
            "$inst/Libs/AmiSSL/$cpu/amissl_v362.library"
        staged_amissl=1
    fi
done
if [ "$staged_amissl" = 1 ]; then
    make_guide amissl/README.md amissl
else
    echo "NOTE: amissl/out/<cpu>/amissl_v362.library not built; packaging without it" >&2
fi

mkdir -p "$staging"
"$PY" "$md2guide" --name ZZ9000-Drivers installer/README.md "$inst/Docs/ZZ9000-Drivers.guide"
cp "$inst/Docs/ZZ9000-Drivers.guide" "$staging/ZZ9000-Drivers.guide"
cp "$inst/Docs/ZZ9000-Drivers.guide.info" "$staging/ZZ9000-Drivers.guide.info"
cp installer/ZZ9000Installer.info "$staging/ZZ9000Installer.info"
cp -R installer/ZZ9000Installer "$staging/ZZ9000Installer"
# .keep files only exist to keep empty source dirs in git.
find "$staging" -name '.keep' -type f -delete
"$PY" tools/encode-amiga-docs.py "$staging"
if command -v zip >/dev/null 2>&1; then
    zip -r "$zipfile" "$(basename "$staging")"
else
    # Git Bash ships no zip; Python's zipfile produces the same layout
    # (deflated, forward-slash names, staging dir as the zip root).
    "$PY" - "$zipfile" "$(basename "$staging")" <<'PYEOF'
import os, sys, zipfile
zipfile_path, root = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(zipfile_path, "w", zipfile.ZIP_DEFLATED) as zf:
    for dirpath, dirnames, files in os.walk(root):
        for name in sorted(files):
            path = os.path.join(dirpath, name)
            zf.write(path, path.replace(os.sep, "/"))
PYEOF
fi
printf '%s\n' "$zipfile"
