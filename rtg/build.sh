export VBCC=../../vbcc
export PATH=$PATH:$VBCC/bin

vc +aos68k -nostdlib -I$VBCC/targets/m68k-amigaos/include2 -c99 -O2 -o ZZ9000.card mntgfx.c -ldebug -lamiga
