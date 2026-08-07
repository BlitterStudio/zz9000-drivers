#!/bin/sh
# Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Drift guard for the ZZ9000.CFG key set.
#
# ZZTop's Settings window rewrites ZZ9000.CFG wholesale from the keys it
# knows about, deliberately: carrying unrecognised lines through would also
# carry through corrupt ones. That only stays safe while ZZTop knows every
# key the firmware accepts. It did not - offscreen_bitmaps, yuv_rect and
# video_overlay were all added to the firmware after the Settings window was
# written, so saving from ZZTop silently deleted them.
#
# The same key has to appear in four places, in two repositories:
#
#   firmware  ZZ9000OS/src/zz_config.c   the parser - the source of truth
#   firmware  ZZ9000.CFG                 the sample shipped in release ZIPs
#   firmware  README.md                  the documented option table
#   drivers   common/zzcfg_amiga.c       ZZTop's editor model
#
# This compares all four and fails on any disagreement.
#
# Usage: tools/check-cfg-keys.sh [path-to-zz9000-firmware]
# Defaults to a sibling checkout, or $ZZ9K_FIRMWARE_DIR.
#
# Exit: 0 = in sync, 1 = mismatch, 2 = an input could not be read.
set -eu

script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
drivers_root=$(CDPATH='' cd -- "$script_dir/.." && pwd)
fw=${1:-${ZZ9K_FIRMWARE_DIR:-"$drivers_root/../zz9000-firmware"}}

parser="$fw/ZZ9000_proto.sdk/ZZ9000OS/src/zz_config.c"
sample="$fw/ZZ9000.CFG"
readme="$fw/README.md"
editor="$drivers_root/common/zzcfg_amiga.c"

for f in "$parser" "$sample" "$readme" "$editor"; do
    if [ ! -r "$f" ]; then
        echo "check-cfg-keys: cannot read $f" >&2
        echo "check-cfg-keys: pass the firmware checkout path or set ZZ9K_FIRMWARE_DIR" >&2
        exit 2
    fi
done

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# The parser is the source of truth: every token_eq(key, "...") branch.
sed -n 's/.*token_eq(key, "\([a-z_0-9]*\)").*/\1/p' "$parser" | sort -u > "$work/parser"

# The sample documents a key as a commented-out assignment.
sed -n 's/^#\{0,1\}\([a-z_0-9]\{1,\}\) = .*/\1/p' "$sample" | sort -u > "$work/sample"

# The README lists them in a markdown table as `key`. Scope the search to the
# configuration section: other tables in the file (the bitstream variants, for
# one) have the same shape and would otherwise be picked up as config keys.
sed -n '/^## Configuration File/,/^## [^C]/p' "$readme" \
    | sed -n 's/^| `\([a-z_0-9]\{1,\}\)` |.*/\1/p' | sort -u > "$work/readme"

# ZZTop's editor matches each key by name.
sed -n 's/.*zzcfg_str_eq_ci(key, "\([a-z_0-9]*\)").*/\1/p' "$editor" | sort -u > "$work/editor"

status=0
report() {
    label=$1
    file=$2
    missing=$(comm -23 "$work/parser" "$file")
    extra=$(comm -13 "$work/parser" "$file")
    if [ -n "$missing" ]; then
        status=1
        echo "  MISSING from $label:" $missing
    fi
    if [ -n "$extra" ]; then
        status=1
        echo "  UNKNOWN in $label (not accepted by the firmware):" $extra
    fi
}

echo "check-cfg-keys: firmware parser = $(wc -l < "$work/parser" | tr -d ' ') key(s)"
report "the sample ZZ9000.CFG" "$work/sample"
report "the firmware README table" "$work/readme"
report "ZZTop's editor (zzcfg_amiga.c)" "$work/editor"

if [ "$status" -eq 0 ]; then
    echo "check-cfg-keys: OK - all four agree"
else
    echo "check-cfg-keys: FAIL - see above" >&2
fi
exit "$status"
