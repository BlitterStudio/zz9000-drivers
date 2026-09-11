#!/bin/sh
# Local build wrapper for ZZ9000Net.device
#
# (C) 2026 Dimitris Panokostas <midwan@gmail.com> — GCC-only build
#
# Uses m68k-amigaos-gcc from https://github.com/bebbo/amiga-gcc.
# Builds run inside the `amigadev/crosstools:m68k-amigaos-gcc10` image
# (via make net or tools/amiga-docker.sh), which has the toolchain on
# its default PATH.

exec make "$@"
