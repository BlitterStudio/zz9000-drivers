#!/bin/sh
set -eu

tag=${1:-local}
script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH='' cd -- "$script_dir/.." && pwd)
staging="$repo_root/zz9000-drivers-$tag"
zipfile="$repo_root/zz9000-drivers-$tag.zip"
inst="$repo_root/installer/ZZ9000Installer"

cd "$repo_root"
tools/check-release.sh

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
install -Dm755 ax-direct/axmp3                  "$inst/Tools/axmp3"
install -Dm644 ahi/README.md                    "$inst/Docs/ahi-README.md"
install -Dm644 usb-poseidon/README.md           "$inst/Docs/usb-poseidon-README.md"

mkdir -p "$staging"
cp installer/README.md "$staging/README.md"
cp installer/ZZ9000Installer.info "$staging/ZZ9000Installer.info"
cp -R installer/ZZ9000Installer "$staging/ZZ9000Installer"
zip -r "$zipfile" "$(basename "$staging")"
printf '%s\n' "$zipfile"
