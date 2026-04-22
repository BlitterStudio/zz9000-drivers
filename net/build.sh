#!/bin/sh
# Local build wrapper for ZZ9000Net.device
#
# (C) 2026 Dimitris Panokostas <midwan@gmail.com> — GCC-only build
#
# Uses m68k-amigaos-gcc from https://github.com/bebbo/amiga-gcc.
# CI uses the `sacredbanana/amiga-compiler:m68k-amigaos` image; for
# local builds this script puts /opt/amiga/bin on PATH before make.
export PATH=$PATH:/opt/amiga/bin

exec make "$@"
