#!/bin/sh
set -eu

# Host leg stages the zz9k client headers, the ZZ9000 private wire
# (zzapp_proto.h), the AAR transport headers (aar/*.h) and the client
# transport module (aar_xport_zz9000.[ch]), then re-execs in the compiler
# image. The container only sees this repo, so the second stage must not stage
# again.
staged=0
if [ "${1:-}" = "--staged" ]; then
    staged=1
    shift
fi

script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH='' cd -- "$script_dir/.." && pwd)
proto_rel=ZZ9000_proto.sdk/ZZ9000OS/src/zzapp_proto.h
module_rel=sdk/amiga/src
stage_dir=$script_dir/zz9k-headers

find_firmware() {
    # Same checkout tools/stage-zz9k-headers.sh just used: explicit
    # ZZ9000_FIRMWARE, else ZZ9000_SDK's parent, else the sibling
    # zz9000-firmware, else the clone that staging left in sdk/work.
    firmware=${ZZ9000_FIRMWARE:-}
    if [ -z "$firmware" ] && [ -n "${ZZ9000_SDK:-}" ]; then
        firmware=$(CDPATH='' cd -- "$ZZ9000_SDK/.." && pwd)
    fi
    if [ -z "$firmware" ] && [ -d "$repo_root/../zz9000-firmware/sdk/include/zz9k" ]; then
        firmware=$(CDPATH='' cd -- "$repo_root/../zz9000-firmware" && pwd)
    fi
    if [ -z "$firmware" ] && [ -d "$repo_root/sdk/work/zz9000-firmware/sdk/include/zz9k" ]; then
        firmware=$(CDPATH='' cd -- "$repo_root/sdk/work/zz9000-firmware" && pwd)
    fi
}

find_aar() {
    # The aar checkout the firmware pins in sdk/AAR_REF: explicit AAR_DIR,
    # else the firmware's own sdk/aar (sdk/scripts/fetch-aar.sh), else a
    # sibling aar checkout.
    aar=${AAR_DIR:-}
    if [ -z "$aar" ] && [ -f "$firmware/sdk/aar/include/aar/transport.h" ]; then
        aar=$firmware/sdk/aar
    fi
    if [ -z "$aar" ] && [ -f "$repo_root/../aar/include/aar/transport.h" ]; then
        aar=$(CDPATH='' cd -- "$repo_root/../aar" && pwd)
    fi
}

stage_transport() {
    find_firmware
    src=${firmware:+$firmware/$proto_rel}
    if [ -z "$firmware" ] || [ ! -s "$src" ]; then
        echo "ERROR: stage zzapp_proto.h from the firmware checkout with" >&2
        echo "       ZZ9000_FIRMWARE=/path/to/zz9000-firmware" >&2
        exit 1
    fi
    for f in aar_xport_zz9000.h aar_xport_zz9000.c; do
        if [ ! -s "$firmware/$module_rel/$f" ]; then
            echo "ERROR: $f is missing from $firmware/$module_rel;" >&2
            echo "       the firmware checkout is older than the transport swap" >&2
            exit 1
        fi
    done
    find_aar
    if [ -z "$aar" ] || [ ! -f "$aar/include/aar/transport.h" ]; then
        echo "ERROR: aar/transport.h not found: run sdk/scripts/fetch-aar.sh in the" >&2
        echo "       firmware checkout, or set AAR_DIR=/path/to/aar" >&2
        exit 1
    fi
    cp "$src" "$stage_dir/zzapp_proto.h"
    cp "$firmware/$module_rel/aar_xport_zz9000.h" "$stage_dir/"
    cp "$firmware/$module_rel/aar_xport_zz9000.c" "$stage_dir/"
    mkdir -p "$stage_dir/aar"
    cp "$aar"/include/aar/*.h "$stage_dir/aar/"
}

if [ "$staged" -eq 0 ]; then
    # An explicit firmware checkout also supplies the SDK headers, unless
    # ZZ9000_SDK overrides them.
    if [ -z "${ZZ9000_SDK:-}" ] && [ -n "${ZZ9000_FIRMWARE:-}" ]; then
        ZZ9000_SDK=$ZZ9000_FIRMWARE/sdk
        export ZZ9000_SDK
    fi
    "$repo_root/tools/stage-zz9k-headers.sh" "$stage_dir" "$repo_root"
    stage_transport
fi

for f in zzapp_proto.h aar_xport_zz9000.h aar_xport_zz9000.c aar/transport.h; do
    if [ ! -f "$stage_dir/$f" ]; then
        echo "ERROR: $f was not staged" >&2
        exit 1
    fi
done

if ! command -v m68k-amigaos-gcc >/dev/null 2>&1; then
    exec "$repo_root/tools/amiga-docker.sh" ZZRun ./build.sh --staged "$@"
fi

cd "$script_dir"
m68k-amigaos-gcc ZZRun.c zz9k-headers/aar_xport_zz9000.c \
    -m68030 -O2 -std=gnu11 -o ZZRun \
    -Wall -Wextra -Wno-unused-parameter \
    -I../include -Izz9k-headers \
    -lamiga -noixemul
