#!/bin/sh
# Usage: smoke-m68k-vamos.sh <program>...
#
# Start each m68k command-line program once under vamos, without
# arguments, and fail on a CPU fault or a vamos crash. Without a ZZ9000 the
# programs are expected to stop early ("board not found", missing library);
# what this catches is startup and exit code that breaks before that point,
# such as a library base that is never opened or a stack swap that returns
# to the wrong stack. Needs vamos (in the Amiga toolchain images).
set -u

if [ "$#" -eq 0 ]; then
    echo "usage: $0 <program>..." >&2
    exit 64
fi

failed=0
for prog in "$@"; do
    dir=$(CDPATH='' cd -- "$(dirname -- "$prog")" && pwd)
    name=$(basename -- "$prog")
    out=$(cd "$dir" && timeout 30 vamos -c "" -- "$name" 2>&1)
    rc=$?
    if [ "$rc" -eq 124 ] ||
        printf '%s\n' "$out" | grep -qiE 'error in cpu run|invalidmemoryaccess|guru|vamos failed|traceback'; then
        echo "FAIL $name (exit $rc)"
        printf '%s\n' "$out" | tail -20
        failed=1
    else
        echo "PASS $name"
    fi
done
exit "$failed"
