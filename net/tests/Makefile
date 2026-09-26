CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra -Werror -std=c11

BUILD_DIR := build
TARGET := $(BUILD_DIR)/mcast_test

.PHONY: test clean

test: $(TARGET)
	$(TARGET)

$(TARGET): mcast_test.c ../mcast.c ../mcast.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -I.. -o $@ mcast_test.c ../mcast.c

clean:
	rm -rf $(BUILD_DIR)
