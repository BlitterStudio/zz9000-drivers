export VBCC=../../vbcc
export PATH=$PATH:$VBCC/bin

#vc +aos68k -nostdlib -I$VBCC/targets/m68k-amigaos/include2 -c99 -O2 -o ZZ9DMA00.card mntgfx.c -ldebug -lamiga -cpu=68000
vc +aos68k -nostdlib -I$VBCC/targets/m68k-amigaos/include2 -c99 -O2 -o ZZ9000.card mntgfx.c -ldebug -lamiga -cpu=68020
#vc +aos68k -nostdlib -I$VBCC/targets/m68k-amigaos/include2 -c99 -O2 -o ZZ9DMA40.card mntgfx.c -ldebug -lamiga -cpu=68040
#vc +aos68k -nostdlib -I$VBCC/targets/m68k-amigaos/include2 -c99 -O2 -o ZZ9DMA60.card mntgfx.c -ldebug -lamiga -cpu=68060
#vc +aos68k -nostdlib -I$VBCC/targets/m68k-amigaos/include2 -c99 -O2 -o ZZ9DMA80.card mntgfx.c -ldebug -lamiga -cpu=68080
#vc +aos68k -DDUMMY_CACHE_READ -nostdlib -I$VBCC/targets/m68k-amigaos/include2 -c99 -O2 -o ZZ9DME00.card mntgfx.c -ldebug -lamiga -cpu=68000
#vc +aos68k -DDUMMY_CACHE_READ -nostdlib -I$VBCC/targets/m68k-amigaos/include2 -c99 -O2 -o ZZ9DME30.card mntgfx.c -ldebug -lamiga -cpu=68030
#vc +aos68k -DDUMMY_CACHE_READ -nostdlib -I$VBCC/targets/m68k-amigaos/include2 -c99 -O2 -o ZZ9DME40.card mntgfx.c -ldebug -lamiga -cpu=68040
#vc +aos68k -DDUMMY_CACHE_READ -nostdlib -I$VBCC/targets/m68k-amigaos/include2 -c99 -O2 -o ZZ9DME60.card mntgfx.c -ldebug -lamiga -cpu=68060
#vc +aos68k -DDUMMY_CACHE_READ -nostdlib -I$VBCC/targets/m68k-amigaos/include2 -c99 -O2 -o ZZ9DME80.card mntgfx.c -ldebug -lamiga -cpu=68080
#vc +aos68k -DDISABLE_DMA_RTG -nostdlib -I$VBCC/targets/m68k-amigaos/include2 -c99 -O2 -o ZZ9000.card mntgfx.c -ldebug -lamiga -cpu=68000
#vc +aos68k -DDISABLE_DMA_RTG -nostdlib -I$VBCC/targets/m68k-amigaos/include2 -c99 -O2 -o ZZ9030.card mntgfx.c -ldebug -lamiga -cpu=68030
#vc +aos68k -DDISABLE_DMA_RTG -nostdlib -I$VBCC/targets/m68k-amigaos/include2 -c99 -O2 -o ZZ9040.card mntgfx.c -ldebug -lamiga -cpu=68040
#vc +aos68k -DDISABLE_DMA_RTG -nostdlib -I$VBCC/targets/m68k-amigaos/include2 -c99 -O2 -o ZZ9060.card mntgfx.c -ldebug -lamiga -cpu=68060
#vc +aos68k -DDISABLE_DMA_RTG -nostdlib -I$VBCC/targets/m68k-amigaos/include2 -c99 -O2 -o ZZ9080.card mntgfx.c -ldebug -lamiga -cpu=68080

