export PATH=$PATH:/opt/amiga/bin

#m68k-amigaos-gcc -m68020 -mtune=68020-60 -s -Wall -Wextra -Wno-unused-parameter -fomit-frame-pointer -nostdlib -nostartfiles -O2 -o zzusb.device mntsd_device.c mntsd_cmd.c rdb_partitions.c
m68k-amigaos-gcc -m68000 -s -Wall -Wextra -Wno-unused-parameter -fomit-frame-pointer -nostdlib -nostartfiles -O2 -o zzusb.device mntsd_device.c mntsd_cmd.c rdb_partitions.c

xxd -i -a zzusb.device > zzusb-device.h

#cp zzusb-device.h ~/code/ZZ9000_proto/ZZ9000_proto.sdk/ZZ9000OS/src/
