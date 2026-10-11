#!/bin/sh
# Create a fresh archive with LH5 compression and level-1 headers.
# Level 1 stores directories in an extended header that Amiga LhA and 7-Zip
# both read. Level 0 does not: LHa for UNIX writes 0xFF as the path separator
# there, so 7-Zip extracts one flat file per entry with 0xFF in its name.
set -eu

if [ "$#" -lt 2 ]; then
    echo "Usage: $0 archive.lha path [path ...]" >&2
    exit 1
fi

exec lha c1o5 "$@"
