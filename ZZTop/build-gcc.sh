#!/bin/bash
# uses https://github.com/bebbo/amiga-gcc
export PATH=$PATH:/opt/amiga/bin

m68k-amigaos-gcc Sources/ZZTop.c -m68030 -O2 -o ZZTop -Wall -Wextra -Wno-unused-parameter -lamiga -noixemul


