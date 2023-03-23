#!/bin/bash

export PATH=$PATH:/opt/amiga/bin

m68k-amigaos-gcc StartUp.c LibInit.c mhizz9000.c asmfuncs.s -m68020 -O3 -o mhizz9000.library.debug -g -ggdb -Wall -Wextra -Wno-unused-parameter -Wno-pointer-to-int-cast -Wno-pointer-sign -nostartfiles -ldebug
m68k-amigaos-strip -s -o mhizz9000.library mhizz9000.library.debug

