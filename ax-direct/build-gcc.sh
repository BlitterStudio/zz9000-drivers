#!/bin/bash
# uses https://github.com/bebbo/amiga-gcc
export PATH=$PATH:/opt/amiga/bin

m68k-amigaos-gcc axmp3.c -m68030 -O2 -o axmp3 -Wall -Wextra -Wno-unused-parameter -lamiga -noixemul

#m68k-amigaos-gcc axwav.c -m68030 -O2 -o axwav -Wall -Wextra -Wno-unused-parameter -lamiga -noixemul

