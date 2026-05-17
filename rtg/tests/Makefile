CC ?= cc
CFLAGS ?= -O2 -g

TARGET := build/register_cache_test

.PHONY: test clean

test: $(TARGET)
	$(TARGET)

$(TARGET): register_cache_test.c ../blitter_cache.h
	@mkdir -p build
	$(CC) -Wall -Wextra $(CFLAGS) -I.. -o $@ register_cache_test.c

clean:
	rm -rf build
