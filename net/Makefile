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

# DEVICEVERSION/DEVICEREVISION/DEVICEDATE come from version.h (HAVE_VERSION_H).
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

$(DEVICEID): $(SOURCES) device.h compiler.h debug.h macros.h sana2.h version.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SOURCES) $(LDLIBS)

clean:
	rm -f $(DEVICEID)

.PHONY: all clean
