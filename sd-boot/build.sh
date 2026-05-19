#!/bin/sh
set -eu

if ! command -v m68k-amigaos-gcc >/dev/null 2>&1; then
  script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
  exec "$script_dir/../tools/amiga-docker.sh" sd-boot ./build.sh "$@"
fi

# To trace driver activity on the Zorro serial debug register, build with
# DEBUG=1 (or any non-empty value). Without it, debugstr()/debughex() are
# compile-time no-ops and the driver is ~500 bytes smaller.
set --
if [ -n "${DEBUG:-}" ]; then
  set -- -DZZSD_DRIVER_DEBUG=1
fi

export PATH=/opt/amiga/bin:"$PATH"

write_c_array() {
  input=$1
  output=$2
  symbol=$3
  size=$(wc -c < "$input" | tr -d ' ')

  {
    printf 'unsigned char %s[] = {\n' "$symbol"
    od -An -tx1 -v "$input" | awk '
      {
        for (i = 1; i <= NF; i++) {
          if (n % 12 == 0) printf "  "
          printf "0x%s, ", $i
          n++
          if (n % 12 == 0) printf "\n"
        }
      }
      END {
        if (n % 12 != 0) printf "\n"
      }'
    printf '};\n'
    printf 'unsigned int %s_len = %s;\n' "$symbol" "$size"
  } > "$output"
}

m68k-amigaos-gcc -m68000 -s -Wall -Wextra -Wno-unused-parameter \
  -fomit-frame-pointer -nostdlib -nostartfiles -Os \
  "$@" \
  -o zzsd.device zzsd_device.c zzsd_cmd.c zzsd_boot.c

write_c_array zzsd.device zzsd-device.h zzsd_device
