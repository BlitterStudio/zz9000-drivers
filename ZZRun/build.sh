#!/bin/sh
set -eu

# Host leg stages the aar.library client headers (libraries/aar.h,
# libraries/aar_zz9000.h, proto/aar.h, inline/aar.h, clib/aar_protos.h and
# pragmas/aar_pragmas.h) and the AAR contract headers (aar/*.h) from the aar
# checkout, then re-execs in the compiler image. The container only sees this
# repo, so the second stage must not stage again. ZZRun calls aar.library
# through its vectors: it links no transport code and no ZZ9000 private wire.
staged=0
if [ "${1:-}" = "--staged" ]; then
    staged=1
    shift
fi

script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH='' cd -- "$script_dir/.." && pwd)
stage_dir=$script_dir/aar-headers
firmware=
aar=

find_firmware() {
    # Explicit ZZ9000_FIRMWARE, else ZZ9000_SDK's parent, else the sibling
    # zz9000-firmware, else the clone in sdk/work.
    firmware=${ZZ9000_FIRMWARE:-}
    if [ -z "$firmware" ] && [ -n "${ZZ9000_SDK:-}" ]; then
        firmware=$(CDPATH='' cd -- "$ZZ9000_SDK/.." && pwd)
    fi
    if [ -z "$firmware" ] && [ -d "$repo_root/../zz9000-firmware/sdk" ]; then
        firmware=$(CDPATH='' cd -- "$repo_root/../zz9000-firmware" && pwd)
    fi
    if [ -z "$firmware" ] && [ -d "$repo_root/sdk/work/zz9000-firmware/sdk" ]; then
        firmware=$(CDPATH='' cd -- "$repo_root/sdk/work/zz9000-firmware" && pwd)
    fi
}

find_aar() {
    # The aar checkout the firmware pins in sdk/AAR_REF: explicit AAR_DIR,
    # else the firmware's own sdk/aar (sdk/scripts/fetch-aar.sh), else a
    # sibling aar checkout.
    aar=${AAR_DIR:-}
    if [ -z "$aar" ] && [ -n "$firmware" ] &&
       [ -f "$firmware/sdk/aar/include/aar/transport.h" ]; then
        aar=$firmware/sdk/aar
    fi
    if [ -z "$aar" ] && [ -f "$repo_root/../aar/include/aar/transport.h" ]; then
        aar=$(CDPATH='' cd -- "$repo_root/../aar" && pwd)
    fi
}

stage_headers() {
    find_firmware
    find_aar
    if [ -z "$aar" ] || [ ! -f "$aar/include/aar/transport.h" ]; then
        echo "ERROR: aar/transport.h not found: run sdk/scripts/fetch-aar.sh in the" >&2
        echo "       firmware checkout, or set AAR_DIR=/path/to/aar" >&2
        exit 1
    fi
    for f in libraries/aar.h libraries/aar_zz9000.h proto/aar.h inline/aar.h \
             clib/aar_protos.h pragmas/aar_pragmas.h; do
        if [ ! -s "$aar/client/include/$f" ]; then
            echo "ERROR: $f is missing from $aar/client/include;" >&2
            echo "       the aar checkout is older than aar.library" >&2
            exit 1
        fi
    done
    rm -rf "$stage_dir"
    mkdir -p "$stage_dir/aar" "$stage_dir/libraries" "$stage_dir/proto" \
             "$stage_dir/inline" "$stage_dir/clib" "$stage_dir/pragmas"
    cp "$aar"/include/aar/*.h "$stage_dir/aar/"
    cp "$aar/client/include/libraries/aar.h" \
       "$aar/client/include/libraries/aar_zz9000.h" "$stage_dir/libraries/"
    cp "$aar/client/include/proto/aar.h" "$stage_dir/proto/"
    cp "$aar/client/include/inline/aar.h" "$stage_dir/inline/"
    cp "$aar/client/include/clib/aar_protos.h" "$stage_dir/clib/"
    cp "$aar/client/include/pragmas/aar_pragmas.h" "$stage_dir/pragmas/"
}

if [ "$staged" -eq 0 ]; then
    stage_headers
fi

for f in aar/transport.h aar/app.h libraries/aar.h libraries/aar_zz9000.h \
         proto/aar.h inline/aar.h clib/aar_protos.h pragmas/aar_pragmas.h; do
    if [ ! -f "$stage_dir/$f" ]; then
        echo "ERROR: $f was not staged" >&2
        exit 1
    fi
done

if ! command -v m68k-amigaos-gcc >/dev/null 2>&1; then
    exec "$repo_root/tools/amiga-docker.sh" ZZRun ./build.sh --staged "$@"
fi

cd "$script_dir"
m68k-amigaos-gcc ZZRun.c \
    -m68030 -O2 -std=gnu11 -o ZZRun \
    -Wall -Wextra -Wno-unused-parameter \
    -Iaar-headers \
    -lamiga -noixemul
