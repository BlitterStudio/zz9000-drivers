CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra -Werror -std=c11

BUILD_DIR := build
TARGETS := $(BUILD_DIR)/mcast_test $(BUILD_DIR)/rx_test

.PHONY: test clean

test: $(TARGETS)
	$(BUILD_DIR)/mcast_test
	$(BUILD_DIR)/rx_test

$(BUILD_DIR)/mcast_test: mcast_test.c ../mcast.c ../mcast.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -I.. -o $@ mcast_test.c ../mcast.c

$(BUILD_DIR)/rx_test: rx_test.c ../rx.c ../rx.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -I.. -o $@ rx_test.c ../rx.c

clean:
	rm -rf $(BUILD_DIR)
