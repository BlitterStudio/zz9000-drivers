CC ?= cc
CFLAGS ?= -O2 -g

TARGET := build/register_cache_test
TARGETS := $(TARGET) build/p96_settings_test build/offscreen_bitmap_test

.PHONY: test clean

test: $(TARGETS)
	$(TARGET)
	build/p96_settings_test
	build/offscreen_bitmap_test

$(TARGET): register_cache_test.c ../blitter_cache.h
	@mkdir -p build
	$(CC) -Wall -Wextra $(CFLAGS) -I.. -o $@ register_cache_test.c

build/p96_settings_test: p96_settings_test.c
	@mkdir -p build
	$(CC) -Wall -Wextra $(CFLAGS) -o $@ p96_settings_test.c

build/offscreen_bitmap_test: offscreen_bitmap_test.c ../offscreen_bitmap.h
	@mkdir -p build
	$(CC) -Wall -Wextra $(CFLAGS) -I.. -o $@ offscreen_bitmap_test.c

clean:
	rm -rf build
