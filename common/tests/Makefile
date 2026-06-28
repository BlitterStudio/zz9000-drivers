CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra

COMMON_DIR := ..
BUILD_DIR := build
TARGET := $(BUILD_DIR)/fwup_client_test

CPPFLAGS += -I$(COMMON_DIR)

SOURCES := fwup_client_test.c $(COMMON_DIR)/fwup_client.c
DEPS := $(COMMON_DIR)/fwup_client.h

.PHONY: test clean

test: $(TARGET)
	$(TARGET)

$(TARGET): $(SOURCES) $(DEPS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(SOURCES)

clean:
	rm -rf $(BUILD_DIR)
