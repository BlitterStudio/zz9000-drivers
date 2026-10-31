#!/bin/sh
# Stage the card module payload into a drivers installer or SD-card kit.
set -eu

if [ "$#" -ne 2 ]; then
    echo "usage: $0 KIT_DIR DESTINATION" >&2
    exit 2
fi

kit_dir=$1
destination=$2
module=$kit_dir/MINIGL.ZZM
manifest=$kit_dir/MINIGL.KIT.json
firmware_dir=${ZZ9K_FIRMWARE_DIR:-../zz9000-firmware}
validator=$firmware_dir/util/check_minigl_kit.py

if [ ! -f "$module" ] || [ ! -f "$manifest" ]; then
    echo "ERROR: MiniGL kit must contain MINIGL.ZZM and MINIGL.KIT.json" >&2
    exit 1
fi
if [ ! -f "$validator" ]; then
    echo "ERROR: need a firmware checkout with util/check_minigl_kit.py" >&2
    exit 1
fi

python3 "$validator" validate "$manifest" --module "$module"
mkdir -p "$destination/ZZ9K"
install -m 0644 "$module" "$destination/ZZ9K/MINIGL.ZZM"
install -m 0644 "$manifest" "$destination/ZZ9K/MINIGL.KIT.json"
