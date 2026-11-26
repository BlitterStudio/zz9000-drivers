#!/bin/sh
# Build WHDLoad.Audio with the ZZ9000AX driver.
#
# WHDLoad loads one plugin by the fixed name WHDLoad.Audio and the plugin
# probes its drivers from a table in its own hunk, so the ZZ9000AX driver
# (zz9000.s) has to be part of that hunk. This script fetches the plugin
# source of Wepl's winstalls at the commit in WINSTALLS_REF, adds the driver
# to its driver table and assembles it with vasm. The upstream files are not
# stored in this repository.
#
# Usage: ./build.sh            build out/WHDLoad.Audio
#        ./build.sh harness    build out/harness.bin for tests/run-tests.sh
# Without vasm on the PATH the script runs itself in the m68k container.
set -eu

script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
cd "$script_dir"

if ! command -v vasmm68k_mot >/dev/null 2>&1; then
    exec "$script_dir/../tools/amiga-docker.sh" whdload-audio ./build.sh "$@"
fi

mode=${1:-plugin}
repo=https://github.com/wepl/winstalls.git
ref=$(tr -d ' \r\n' < WINSTALLS_REF)
work=work
src=$work/winstalls
build=$work/build

# fetch the pinned commit, only the plugin and the include files
if [ ! -f "$src/.ref" ] || [ "$(cat "$src/.ref")" != "$ref" ]; then
    rm -rf "$src"
    mkdir -p "$src"
    git -C "$src" init -q
    git -C "$src" remote add origin "$repo"
    git -C "$src" sparse-checkout set audio include
    git -C "$src" fetch -q --depth 1 --filter=blob:none origin "$ref"
    git -C "$src" -c advice.detachedHead=false checkout -q FETCH_HEAD
    echo "$ref" > "$src/.ref"
fi
if [ "$(git -C "$src" rev-parse HEAD)" != "$ref" ]; then
    echo "ERROR: fetched $(git -C "$src" rev-parse HEAD), expected $ref" >&2
    exit 1
fi

rm -rf "$build"
mkdir -p "$build" out

# options of winstalls Maketools for vasm
vasm() {
    vasmm68k_mot -I"$src/include" -I"$build" -ignore-mult-inc -nosym -quiet \
        -wfail -opt-allbra -opt-clr -opt-lsl -opt-nmoveq -opt-pea -opt-size \
        -opt-st -ldots "$@"
}

cp zz9000.s "$build/"

if [ "$mode" = harness ]; then
    cp tests/harness.s "$build/"
    vasm -Fbin -o out/harness.bin "$build/harness.s"
    ls -l out/harness.bin
    exit 0
fi
if [ "$mode" != plugin ]; then
    echo "usage: $0 [plugin|harness]" >&2
    exit 64
fi

cp "$src/audio/WHDLoad.Audio.asm" "$src/audio/amigus.s" \
    "$src/audio/vs1063a-patches.bin" "$build/"

# add the driver to the table and the includes, after the AmiGUS driver;
# fail if the upstream source changed shape
awk '
    /^_drivers[ \t]+dc\.l[ \t]+amigus_driver[ \t]*$/ {
        print; print "\t\tdc.l\tzz9000_driver"; table++; next }
    /^[ \t]+INCLUDE[ \t]+amigus\.s[ \t]*$/ {
        print; print "\tINCLUDE\tzz9000.s"; inc++; next }
    { print }
    END { if (table != 1 || inc != 1) exit 1 }
' "$build/WHDLoad.Audio.asm" > "$build/WHDLoad.Audio.zz9000.asm" || {
    echo "ERROR: WHDLoad.Audio.asm does not have the expected driver table" >&2
    exit 1
}

# $VER date, shown by WHDLoad in its informational messages
printf '(%s, with the ZZ9000AX driver)' "$(date -u +%d.%m.%Y)" > "$build/.date"

vasm -Fhunkexe -o out/WHDLoad.Audio "$build/WHDLoad.Audio.zz9000.asm"
ls -l out/WHDLoad.Audio
