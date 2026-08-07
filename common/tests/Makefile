CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra

COMMON_DIR := ..
BUILD_DIR := build
TARGET := $(BUILD_DIR)/fwup_client_test
CFG_TARGET := $(BUILD_DIR)/zzcfg_test

CPPFLAGS += -I$(COMMON_DIR)

SOURCES := fwup_client_test.c $(COMMON_DIR)/fwup_client.c
DEPS := $(COMMON_DIR)/fwup_client.h

# zzcfg_amiga.c is Amiga source; stubs/ supplies the few AmigaOS headers it
# includes so the pure parse/generate model can be tested on the host.
CFG_SOURCES := zzcfg_test.c $(COMMON_DIR)/zzcfg_amiga.c stubs/fwup_stub.c
CFG_DEPS := $(COMMON_DIR)/zzcfg_amiga.h $(COMMON_DIR)/../include/zzcfg_query.h
CFG_CPPFLAGS := -Istubs -I$(COMMON_DIR) -I$(COMMON_DIR)/../include

.PHONY: test clean

test: $(TARGET) $(CFG_TARGET)
	$(TARGET)
	$(CFG_TARGET)

$(TARGET): $(SOURCES) $(DEPS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(SOURCES)

# -w: the shared Amiga source casts board addresses through integers, which
# is correct on m68k and noisy on a 64-bit host.
$(CFG_TARGET): $(CFG_SOURCES) $(CFG_DEPS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFG_CPPFLAGS) $(CFLAGS) -w -o $@ $(CFG_SOURCES)

clean:
	rm -rf $(BUILD_DIR)
