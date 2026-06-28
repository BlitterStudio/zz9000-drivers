#!/bin/sh
set -eu

quick=0
if [ "${1:-}" = "--quick" ]; then
    quick=1
fi

missing=0
check_file() {
    if [ ! -f "$1" ]; then
        echo "MISSING: $1" >&2
        missing=1
    fi
}

check_exec() {
    check_file "$1"
    if [ -f "$1" ] && [ ! -x "$1" ]; then
        echo "NOT EXECUTABLE: $1" >&2
        missing=1
    fi
}

check_file README.md
check_file installer/README.md
check_file installer/ZZ9000Installer/Install\ ZZ9000
check_file installer/ZZ9000Installer/Devs/Picasso96Settings
check_file installer/ZZ9000Installer/Devs/NetInterfaces/ZZ9000Net
check_file ahi/README.md
check_file usb-poseidon/README.md
check_file sdk/README.md
check_file sdk/SDK_REF
check_file amissl/README.md

if [ "$quick" -eq 0 ]; then
    check_file rtg/ZZ9000.card
    check_file usb-poseidon/zzusbhw.device
    check_file net/ZZ9000Net.device
    check_file ahi/driver/zz9000ax.audio
    check_file ahi/driver/ZZ9000AX
    check_file mhi/mhizz9000.library
    check_file sd-boot/zzsd.device
    check_exec ZZTop/ZZTop
    check_exec ZZScanlines/ZZScanlines
    check_exec ZZFwUpdate/ZZFwUpdate
    check_exec net/ZZNetStats/ZZNetStats
    check_exec ZZDiag/ZZDiag
    check_exec ax-direct/axmp3

    # SDK runtime payloads (sdk/build.sh).
    check_file sdk/out/Libs/zz9k.library
    check_file sdk/out/Libs/mpega.library
    check_file sdk/out/Classes/DataTypes/zz9k-picture.datatype
    check_file sdk/out/C/zz9k-info
    check_file sdk/out/C/zz9k-services
    check_file sdk/out/C/zz9k-view
    check_file sdk/out/C/zz9k-jpeg
    check_file sdk/out/C/zz9k-png
    check_file sdk/out/C/zz9k-mp3
    check_file sdk/out/C/zz9k-cryptobench
    check_file sdk/out/C/zz9k-archive

    # amissl_v362.library (amissl/build.sh) is optional for local packaging;
    # release CI builds both CPU variants (Libs/AmiSSL/<cpu>/). Warn without
    # failing if neither is present.
    if [ ! -f amissl/out/68020-40/amissl_v362.library ] && \
       [ ! -f amissl/out/68060/amissl_v362.library ]; then
        echo "NOTE: amissl/out/<cpu>/amissl_v362.library not built (amissl/build.sh)" >&2
    fi

    if [ -f sd-boot/zzsd.device ]; then
        size=$(wc -c < sd-boot/zzsd.device | tr -d ' ')
        if [ "$size" -ge 7424 ]; then
            echo "TOO LARGE: sd-boot/zzsd.device is $size bytes, ceiling is 7423" >&2
            missing=1
        fi
    fi
fi

tracked_artifacts=$(
    for path in \
        rtg/ZZ9000.card \
        usb-poseidon/zzusbhw.device \
        net/ZZ9000Net.device \
        net/ZZ9000Net.device.68000 \
        ahi/driver/zz9000ax.audio \
        ahi/driver/ZZ9000AX \
        mhi/mhizz9000.library \
        mhi/mhizz9000.library.debug \
        sd-boot/zzsd.device \
        sd-boot/boot-rom/boot.bin \
        ZZFwUpdate/ZZFwUpdate \
        ZZScanlines/ZZScanlines \
        ZZTop/ZZTop \
        net/ZZNetStats/ZZNetStats \
        ZZDiag/ZZDiag \
        ax-direct/axmp3 \
        ax-direct/axtest \
        ahi/axtest/axtest
    do
        if git ls-files --error-unmatch "$path" >/dev/null 2>&1; then
            echo "$path"
        fi
    done
)
if [ -n "$tracked_artifacts" ]; then
    echo "Generated binaries are tracked; remove them with git rm --cached:" >&2
    echo "$tracked_artifacts" >&2
    missing=1
fi

exit "$missing"
