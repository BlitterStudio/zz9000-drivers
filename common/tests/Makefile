CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra

COMMON_DIR := ..
BUILD_DIR := build
TARGET := $(BUILD_DIR)/fwup_client_test
CFG_TARGET := $(BUILD_DIR)/zzcfg_test
VCAP_TARGET := $(BUILD_DIR)/zz_vcap_live_test
CAPTURE_TARGET := $(BUILD_DIR)/capture_calibration_test
CAPTURE_LIFECYCLE_TARGET := $(BUILD_DIR)/zzcapture_lifecycle_test

CPPFLAGS += -I$(COMMON_DIR)

SOURCES := fwup_client_test.c $(COMMON_DIR)/fwup_client.c
DEPS := $(COMMON_DIR)/fwup_client.h

# zzcfg_amiga.c uses stubs/ for AmigaOS types and a simulated board;
# parse/generate/save run on the host with the real FWUP protocol client.
CFG_SOURCES := zzcfg_test.c $(COMMON_DIR)/zzcfg_amiga.c \
	$(COMMON_DIR)/fwup_client.c stubs/fwup_stub.c
CFG_DEPS := $(COMMON_DIR)/zzcfg_amiga.h $(COMMON_DIR)/../include/zzcfg_query.h \
	$(COMMON_DIR)/fwup_client.h $(COMMON_DIR)/fwup_amiga.h
CFG_CPPFLAGS := -Istubs -I$(COMMON_DIR) -I$(COMMON_DIR)/../include \
	-DZZCFG_TEST_IO -DZZCFG_POLL_LIMIT=32UL
VCAP_SOURCES := zz_vcap_live_test.c $(COMMON_DIR)/zz_vcap_live.c
VCAP_DEPS := $(COMMON_DIR)/zz_vcap_live.h $(COMMON_DIR)/../include/zz9000_hw.h
VCAP_CPPFLAGS := -Istubs -I$(COMMON_DIR) -I$(COMMON_DIR)/../include

.PHONY: test clean

test: $(TARGET) $(CFG_TARGET) $(VCAP_TARGET) $(CAPTURE_TARGET) $(CAPTURE_LIFECYCLE_TARGET)
	$(TARGET)
	$(CFG_TARGET)
	$(VCAP_TARGET)
	$(CAPTURE_TARGET)
	$(CAPTURE_LIFECYCLE_TARGET)

$(TARGET): $(SOURCES) $(DEPS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(SOURCES)

# -w: the shared Amiga source casts board addresses through integers, which
# is correct on m68k and noisy on a 64-bit host.
$(CFG_TARGET): $(CFG_SOURCES) $(CFG_DEPS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFG_CPPFLAGS) $(CFLAGS) -w -o $@ $(CFG_SOURCES)

$(VCAP_TARGET): $(VCAP_SOURCES) $(VCAP_DEPS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(VCAP_CPPFLAGS) $(CFLAGS) -o $@ $(VCAP_SOURCES)

$(CAPTURE_TARGET): ../../tests/capture_calibration_test.c ../../include/zz9000_capture_calibration.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -std=c99 -Werror -I../../include -o $@ $<

$(CAPTURE_LIFECYCLE_TARGET): zzcapture_lifecycle_test.c stubs/zzcapture_test_platform.h \
		../../ZZCapture/ZZCapture.c ../../include/zz9000_capture_calibration.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -std=c99 -Werror -DZZCAPTURE_TEST_IO -Istubs -I../../include -o $@ $<

clean:
	rm -rf $(BUILD_DIR)
