#!/bin/sh
# Create a fresh archive with LH5 compression and level-0 headers for Amiga LhA.
set -eu

if [ "$#" -lt 2 ]; then
    echo "Usage: $0 archive.lha path [path ...]" >&2
    exit 1
fi

exec lha c0o5 "$@"
