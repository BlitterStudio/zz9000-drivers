#!/bin/sh
# Usage: check-link-map.sh <binary> <map>
#
# Fail (and delete <binary>) when a startup-less driver's link map has a
# __LIB_LIST__ entry: a libnix auto-open library base. Only libnix startup
# code opens those, so in a device or library the base stays at its
# initial value and the first call through it crashes. GCC 16's varargs
# inlines (CreateNewProcTags and friends) read such global bases.
set -eu

binary=$1
map=$2
if grep -q __LIB_LIST__ "$map"; then
    echo "$binary: links a libnix auto-open library base that nothing opens:" >&2
    grep -B1 -A2 __LIB_LIST__ "$map" >&2
    rm -f "$binary"
    exit 1
fi
