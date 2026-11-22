#!/bin/sh
# Usage: smoke-m68k-vamos.sh <program>...
#
# Start each m68k command-line program once under vamos, without
# arguments, and fail on a CPU fault, a system alert or a vamos crash.
# Without a ZZ9000 the programs are expected to stop early ("board not
# found", missing library); what this catches is startup and exit code that
# breaks before that point, such as a library base that is never opened or a
# stack swap that returns to the wrong stack.
#
# The CPU is 68020: most tools here are built for 68020 or 68030, and under
# vamos's default 68000 their startup raises an alert (00068020) before
# main. A 68000 program runs unchanged on 68020. The alert is also matched
# as a fault.
#
# The image has no display. A Workbench tool that opens a screen or window
# makes vamos log "no display available" and then fail in its display
# backend; that is the end of what can be checked headless, so such a run
# passes and says so. Needs vamos (in the Amiga toolchain images).
set -u

if [ "$#" -eq 0 ]; then
    echo "usage: $0 <program>..." >&2
    exit 64
fi

failed=0
for prog in "$@"; do
    dir=$(CDPATH='' cd -- "$(dirname -- "$prog")" && pwd)
    name=$(basename -- "$prog")
    out=$(cd "$dir" && timeout 30 vamos -c "" -C 68020 -- "$name" 2>&1)
    rc=$?
    if printf '%s\n' "$out" | grep -q 'no display available'; then
        echo "PASS $name (no display: stopped at its first screen or window)"
    elif [ "$rc" -eq 124 ] ||
        printf '%s\n' "$out" | grep -qiE 'error in cpu run|invalidmemoryaccess|guru|vamos failed|traceback|error: +alert|alertnum'; then
        echo "FAIL $name (exit $rc)"
        printf '%s\n' "$out" | tail -20
        failed=1
    else
        echo "PASS $name"
    fi
done
exit "$failed"
