export CROSSPATH="$(pwd)/../../amiga-gcc/m68k-amigaos"
export GCCLIBPATH="$CROSSPATH/lib/gcc-lib/m68k-amigaos/2.95.3"
export PATH="$CROSSPATH/bin:$CROSSPATH/m68k-amigaos/bin:$GCCLIBPATH:$PATH"
export GCC_EXEC_PREFIX=m68k-amigaos
export LIBS="$CROSSPATH/lib"

m68k-amigaos-gcc -m68000 -O2 -o ZZ9000USBStorage.device -ramiga-dev -noixemul -fbaserel -I$CROSSPATH/m68k-amigaos/sys-include -I$CROSSPATH/m68k-amigaos/ndk/include -L$LIBS -L$LIBS/gcc-lib/m68k-amigaos/2.95.3 -L$CROSSPATH/m68k-amigaos/lib mntsd_device.c mntsd_cmd.c -ldebug
