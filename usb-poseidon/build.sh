export PATH=$PATH:/opt/amiga/bin

m68k-amigaos-gcc -m68020 -mtune=68020-60 -msoft-float -s -Wall -Wextra -Wno-unused-parameter -fomit-frame-pointer -nostdlib -nostartfiles -O2 -I. -o zzusbhw.device zzusbhw_device.c -lgcc -lc
