# Makefile for ZZ9000Net.device (SANA-II)
#
# (C) 2026 Dimitris Panokostas <midwan@gmail.com> — GCC-only build
#
# Builds with m68k-amigaos-gcc from https://github.com/bebbo/amiga-gcc.
# CI uses the `sacredbanana/amiga-compiler:m68k-amigaos` image, which has
# the Bebbo toolchain plus the Roadshow SDK pre-installed under
# /opt/amiga/m68k-amigaos. For local builds, run ./build.sh which puts
# /opt/amiga/bin on PATH before invoking make.

DEVICEID = ZZ9000Net.device

CC      = m68k-amigaos-gcc
CPU     = 68020

SOURCES = deviceheader.c deviceinit.c device.c

# U7 flag matrix (KTD9): the shipped set must build on the CI image
# (sacredbanana/amiga-compiler, GCC 6.5, no -mlra). The candidates below
# all build on amigadev/crosstools:m68k-amigaos-gcc10 and are A/B-measured
# on the target before any ships (docs/plans/...-plan.md U7):
#   -O3 -fomit-frame-pointer
#   -O2 -mlra -fomit-frame-pointer          (gcc10 only)
#   -O3 -mlra -fomit-frame-pointer -mtune=68060  (gcc10 only)

CFLAGS  = -m$(CPU) -mtune=68020-60 -msoft-float -O2 \
          -Wall -Wextra -Wno-unused-parameter -Wno-pointer-sign \
          -fomit-frame-pointer \
          -I../include \
          -DDEVICENAME=$(DEVICEID) \
          -DHAVE_VERSION_H

LDFLAGS = -nostdlib -nostartfiles -s
LDLIBS  = -lgcc -lc

ifeq ($(DEBUG),1)
CFLAGS  += -DDEBUG -g
LDLIBS  += -ldebug
endif

all: $(DEVICEID)

$(DEVICEID): $(SOURCES) device.h compiler.h debug.h macros.h sana2.h version.h anxs2ext.h zznet_ext.h zznet_mmio.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SOURCES) $(LDLIBS)

clean:
	rm -f $(DEVICEID)

.PHONY: all clean
