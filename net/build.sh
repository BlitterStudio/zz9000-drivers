#!/bin/sh
# Local build wrapper for ZZ9000Net.device
#
# (C) 2026 Dimitris Panokostas <midwan@gmail.com> — GCC-only build
#
# Runs make with the local m68k-amigaos-gcc, or in the toolchain image
# through tools/amiga-docker.sh when there is none on PATH.
set -eu

if ! command -v m68k-amigaos-gcc >/dev/null 2>&1; then
  script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
  exec "$script_dir/../tools/amiga-docker.sh" net make "$@"
fi

exec make "$@"
