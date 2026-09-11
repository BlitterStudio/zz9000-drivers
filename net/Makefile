# Makefile for ZZ9000Net.device (SANA-II)
#
# (C) 2026 Dimitris Panokostas <midwan@gmail.com> — GCC-only build
#
# Builds with m68k-amigaos-gcc from https://github.com/bebbo/amiga-gcc.
# Builds run inside the `amigadev/crosstools:m68k-amigaos-gcc10` image
# (Bebbo GCC 16.2.0b plus the NDK and the Roadshow SDK under
# /opt/m68k-amigaos, already on the image's default PATH). Locally,
# `make net` or ./build.sh wrap that in tools/amiga-docker.sh.

# LRA (-mlra) exists on Bebbo GCC 10+ only; the older 6.5.0b rejects it.
LRA := $(shell m68k-amigaos-gcc -mlra -x c -fsyntax-only /dev/null 2>/dev/null && echo -mlra)

DEVICEID = ZZ9000Net.device

CC      = m68k-amigaos-gcc
CPU     = 68020

SOURCES = deviceheader.c deviceinit.c device.c

# DEVICEVERSION/DEVICEREVISION/DEVICEDATE come from version.h (HAVE_VERSION_H).
CFLAGS  = -m$(CPU) -mtune=68020-60 -msoft-float -O2 $(LRA) \
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

$(DEVICEID): $(SOURCES) device.h compiler.h debug.h macros.h sana2.h version.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SOURCES) $(LDLIBS)

clean:
	rm -f $(DEVICEID)

.PHONY: all clean
