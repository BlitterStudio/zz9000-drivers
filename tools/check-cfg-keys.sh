#!/bin/sh
# Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Drift guard for the ZZ9000.CFG key set and native-video profile values.
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
# This compares all four and fails on any key or ordered profile-value
# disagreement. Profile ordering is part of the shared schema: append new
# values so older numeric identities remain stable.
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

# These three keys remain accepted only to load existing hand-written files.
# New firmware/ZZTop writes the atomic videocap_profile instead, so requiring
# the legacy trio in the sample and README would recreate the confusing
# independent controls the profile was introduced to remove.
cat > "$work/legacy" <<'EOF'
nonstandard_vsync
videocap_mode
videocap_shres
EOF
comm -23 "$work/parser" "$work/legacy" > "$work/canonical"

# The scene-name keys are 64 per-scene-per-chunk variants of one grammar;
# they cannot all fit the 4 KiB sample parse budget (a larger sample would
# have its audio tail ignored at boot). The sample documents the grammar
# with scene 0's leading chunks; the rest are exempt from the SAMPLE
# comparison only. README and the editor must still carry every key, and
# the exemption is keyed to the exact grammar (audio_sceneN_nmK), so a
# missing lpf/eq/out/pan key is still caught.
sed -e '/^audio_scene[1-7]_nm[1-8]$/d' \
    -e '/^audio_scene0_nm[5-8]$/d' \
    "$work/canonical" > "$work/sample_expected"

# The sample documents a key as a commented-out assignment.
sed -n 's/^#\{0,1\}\([a-z_0-9]\{1,\}\) = .*/\1/p' "$sample" | sort -u > "$work/sample"

# The README lists them in a markdown table as `key`. Scope the search to the
# configuration section: other tables in the file (the bitstream variants, for
# one) have the same shape and would otherwise be picked up as config keys.
sed -n '/^## Configuration File/,/^## [^C]/p' "$readme" \
    | sed -n 's/^| `\([a-z_0-9]\{1,\}\)` |.*/\1/p' | sort -u > "$work/readme"

# ZZTop's editor matches each key by name.
sed -n 's/.*zzcfg_str_eq_ci(key, "\([a-z_0-9]*\)").*/\1/p' "$editor" | sort -u > "$work/editor"

# Keep the ordered videocap_profile schema aligned as well as the key names.
# The parser branch order is canonical and must match the shipped sample,
# firmware README, and the driver editor's descriptor array exactly.
sed -n '/if (token_eq(key, "videocap_profile")) {/,/if (token_eq(key, "videocap_mode")) {/ {
    s/.*token_eq(value, "\([a-z_0-9]*\)").*/\1/p
}' "$parser" > "$work/profiles-parser"
sed -n 's/^#   \([a-z_0-9][a-z_0-9]*\)[[:space:]]*- .*/\1/p' \
    "$sample" > "$work/profiles-sample"
awk -F'`' '$2 == "videocap_profile" {
    for (i = 4; i <= NF; i += 2)
        if ($i ~ /^[a-z_0-9]+$/) print $i
}' "$readme" > "$work/profiles-readme"
sed -n '/static const struct zzcfg_profile_desc zzcfg_profiles\[\] = {/,/^};/ {
    s/^[[:space:]]*{ "\([a-z_0-9]*\)".*/\1/p
}' "$editor" > "$work/profiles-editor"

status=0
report() {
    label=$1
    expected=$2
    file=$3
    missing=$(comm -23 "$expected" "$file")
    extra=$(comm -13 "$expected" "$file")
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
report "the sample ZZ9000.CFG" "$work/sample_expected" "$work/sample"
report "the firmware README table" "$work/canonical" "$work/readme"
report "ZZTop's editor (zzcfg_amiga.c)" "$work/parser" "$work/editor"

report_profiles() {
    label=$1
    file=$2
    if ! cmp -s "$work/profiles-parser" "$file"; then
        status=1
        echo "  ORDERED videocap_profile values disagree in $label:"
        diff -u "$work/profiles-parser" "$file" || true
    fi
}

report_profiles "the sample ZZ9000.CFG" "$work/profiles-sample"
report_profiles "the firmware README table" "$work/profiles-readme"
report_profiles "ZZTop's editor (zzcfg_amiga.c)" "$work/profiles-editor"

active_sample=$(sed -n 's/^[[:space:]]*\([a-z_0-9][a-z_0-9]*\)[[:space:]]*=.*$/\1/p' "$sample")
if [ -n "$active_sample" ]; then
    status=1
    echo "  ACTIVE assignments in the shipped sample ZZ9000.CFG:"
    printf '%s\n' "$active_sample" | sed 's/^/    /'
fi

if [ "$status" -eq 0 ]; then
    echo "check-cfg-keys: OK - all four agree on keys and profile values"
else
    echo "check-cfg-keys: FAIL - see above" >&2
fi
exit "$status"
