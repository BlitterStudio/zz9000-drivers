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

SOURCES = deviceheader.c deviceinit.c device.c mcast.c rx.c tx.c

# DEVICEVERSION/DEVICEREVISION/DEVICEDATE come from version.h (HAVE_VERSION_H).
CFLAGS  = -m$(CPU) -mtune=68020-60 -msoft-float -O2 \
          -Wall -Wextra -Wno-unused-parameter -Wno-pointer-sign \
          -fomit-frame-pointer \
          -I../include \
          -DDEVICENAME=$(DEVICEID) \
          -DHAVE_VERSION_H

# The map is checked after linking: a __LIB_LIST__ entry is a libnix
# auto-open library base, which only libnix startup code opens. This device
# has no startup code, so such a base stays unset and the first call through
# it crashes (GCC 16 varargs inlines reading a global DOSBase: Guru 80000003).
LDFLAGS = -nostdlib -nostartfiles -s -Wl,-Map,$(DEVICEID).map
LDLIBS  = -lgcc -lc

ifeq ($(DEBUG),1)
CFLAGS  += -DDEBUG -g
LDLIBS  += -ldebug
endif

all: $(DEVICEID)

$(DEVICEID): $(SOURCES) device.h compiler.h debug.h macros.h sana2.h version.h mcast.h rx.h tx.h \
              ../include/zzcfg_query.h ../include/zz9000_hw.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SOURCES) $(LDLIBS)
	@if grep -q __LIB_LIST__ $(DEVICEID).map; then \
	  echo "$(DEVICEID): links a libnix auto-open library base that nothing opens:" >&2; \
	  grep -B1 -A2 __LIB_LIST__ $(DEVICEID).map >&2; \
	  rm -f $@; exit 1; \
	fi

clean:
	rm -f $(DEVICEID) $(DEVICEID).map

.PHONY: all clean
