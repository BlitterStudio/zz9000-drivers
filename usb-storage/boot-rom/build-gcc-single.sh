#!/bin/bash
# uses https://github.com/bebbo/amiga-gcc
export PATH=$PATH:/opt/amiga/bin

set -e

m68k-amigaos-gcc boot.S -s -m68020 -mtune=68020-60 -O2 -o boot.rom -noixemul -Wall -fomit-frame-pointer -nostartfiles -fpic

m68k-amigaos-objdump -S --disassemble boot.rom

xxd -i -a boot.rom > zzbootrom.h

cp zzbootrom.h ~/code/ZZ9000_proto/ZZ9000_proto.sdk/ZZ9000OS/src/
