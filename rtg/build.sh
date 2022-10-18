#!/bin/bash
# uses https://github.com/bebbo/amiga-gcc
export PATH=$PATH:/opt/amiga/bin

m68k-amigaos-gcc mntgfx-gcc.c -m68020 -mtune=68020-60 -O2 -o ZZ9000.card -noixemul -Wall -Wextra -Wno-unused-parameter -fomit-frame-pointer -nostartfiles -lamiga

