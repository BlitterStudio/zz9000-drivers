#!/bin/bash

export PATH=$PATH:/opt/amiga/bin

vasmm68k_mot -quiet -phxass -Fhunk -m68020 -o PREFSFILE.uncut prefsfile.a -I/opt/amiga/m68k-amigaos/ndk-include -I/opt/amiga/m68k-amigaos/include

# remove 0x28 bytes from the start
dd bs=1 skip=40 if=PREFSFILE.uncut of=ZZ9000AX

m68k-amigaos-gcc zz9000ax-ahi.c asmfuncs.s -O3 -o zz9000ax.audio -Wall -Wextra -Wno-unused-parameter -nostartfiles -m68020
