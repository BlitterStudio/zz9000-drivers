export PATH=$PATH:/opt/amiga/bin

m68k-amigaos-gcc -m68000 -s -Wall -Wextra -Wno-unused-parameter -fomit-frame-pointer -nostdlib -nostartfiles -O2 -o zzusb.device mntsd_device.c mntsd_cmd.c

xxd -i -a zzusb.device > zzusb-device.h
