#!/bin/bash
# uses https://github.com/bebbo/amiga-gcc
export PATH=$PATH:/opt/amiga/bin

m68k-amigaos-gcc axtest.c -m68000 -O2 -o axtest -Wall -Wextra -Wno-unused-parameter -lamiga -noixemul
