CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra

COMMON_DIR := ..
BUILD_DIR := build
TARGET := $(BUILD_DIR)/fwup_client_test
CFG_TARGET := $(BUILD_DIR)/zzcfg_test
VCAP_TARGET := $(BUILD_DIR)/zz_vcap_live_test
VCAP_CLIENT_TARGET := $(BUILD_DIR)/zz_vcap_client_test
CAPTURE_TARGET := $(BUILD_DIR)/capture_calibration_test
CAPTURE_LIFECYCLE_TARGET := $(BUILD_DIR)/zzcapture_lifecycle_test
SAMPLING_CALIBRATION_TARGET := $(BUILD_DIR)/sampling_calibration_test
EDIT_TARGET := $(BUILD_DIR)/zz_vcap_edit_test
GEOMETRY_TARGET := $(BUILD_DIR)/zz_vcap_geometry_test
VCAP_SAMPLING_SCREEN_TARGET := $(BUILD_DIR)/vcap_sampling_screen_test
GAMMA_TARGET := $(BUILD_DIR)/zz9000_gamma_test

CPPFLAGS += -I$(COMMON_DIR)

SOURCES := fwup_client_test.c $(COMMON_DIR)/fwup_client.c
DEPS := $(COMMON_DIR)/fwup_client.h

# zzcfg_amiga.c uses stubs/ for AmigaOS types and a simulated board;
# parse/generate/save run on the host with the real FWUP protocol client.
CFG_SOURCES := zzcfg_test.c $(COMMON_DIR)/zzcfg_amiga.c \
	$(COMMON_DIR)/fwup_client.c stubs/fwup_stub.c
CFG_DEPS := $(COMMON_DIR)/zzcfg_amiga.h $(COMMON_DIR)/../include/zzcfg_query.h \
	$(COMMON_DIR)/../include/zz9000_gamma.h \
	$(COMMON_DIR)/fwup_client.h $(COMMON_DIR)/fwup_amiga.h
CFG_CPPFLAGS := -Istubs -I$(COMMON_DIR) -I$(COMMON_DIR)/../include \
	-DZZCFG_TEST_IO -DZZCFG_POLL_LIMIT=32UL
VCAP_SOURCES := zz_vcap_live_test.c $(COMMON_DIR)/zz_vcap_live.c
VCAP_CLIENT_SOURCES := zz_vcap_client_test.c $(COMMON_DIR)/zz_vcap_client.c $(COMMON_DIR)/zz_vcap_live.c
EDIT_SOURCES := zz_vcap_edit_test.c
GEOMETRY_SOURCES := zz_vcap_geometry_test.c $(COMMON_DIR)/zz_vcap_geometry.c $(COMMON_DIR)/zz_vcap_client.c $(COMMON_DIR)/zz_vcap_live.c
VCAP_DEPS := $(COMMON_DIR)/zz_vcap_live.h $(COMMON_DIR)/../include/zz9000_hw.h
VCAP_CLIENT_DEPS := $(COMMON_DIR)/zz_vcap_client.h $(VCAP_DEPS) $(COMMON_DIR)/../include/zz9000_capture_calibration.h
EDIT_DEPS := $(COMMON_DIR)/zz_vcap_edit.h $(COMMON_DIR)/zzcfg_amiga.h $(VCAP_DEPS)
GEOMETRY_DEPS := $(COMMON_DIR)/zz_vcap_geometry.h $(COMMON_DIR)/zz_vcap_client.h $(VCAP_DEPS) $(COMMON_DIR)/../include/zzcfg_query.h $(COMMON_DIR)/../include/zz9000_capture_calibration.h
VCAP_CPPFLAGS := -Istubs -I$(COMMON_DIR) -I$(COMMON_DIR)/../include
EDIT_CPPFLAGS := $(VCAP_CPPFLAGS) -DZZCFG_TEST_IO
GEOMETRY_CPPFLAGS := $(VCAP_CPPFLAGS) -DZZCFG_TEST_IO
VCAP_SAMPLING_SCREEN_SOURCES := vcap_sampling_screen_test.c \
	../../ZZTop/Core/vcap_sampling.c ../../common/zz_vcap_client.c \
	../../common/zz_vcap_live.c
VCAP_SAMPLING_SCREEN_DEPS := stubs/vcap_sampling_test_platform.h \
	../../ZZTop/Core/vcap_sampling.h $(VCAP_CLIENT_DEPS) \
	../../include/zz9000_sampling_calibration.h
VCAP_SAMPLING_SCREEN_CPPFLAGS := -Istubs -I../../common -I../../include \
	-I../../ZZTop/Core -DVCAP_SAMPLING_TEST_IO

ZZTOP_SETTINGS_TARGET := $(BUILD_DIR)/zztop_settings_test
ZZTOP_SETTINGS_SOURCES := zztop_settings_test.c \
	../../ZZTop/Core/settings.c $(COMMON_DIR)/zzcfg_amiga.c \
	$(COMMON_DIR)/fwup_client.c stubs/fwup_stub.c
ZZTOP_SETTINGS_DEPS := stubs/zztop_settings_test_platform.h \
	../../ZZTop/Core/settings.h ../../ZZTop/Core/board.h $(CFG_DEPS)
ZZTOP_SETTINGS_CPPFLAGS := $(CFG_CPPFLAGS) -I../../ZZTop/Core \
	-DZZTOP_SETTINGS_TEST

# Online firmware update helpers. Production inflates with the vendored
# miniz tinfl; the test builds its fixture archives with host zlib.
FW_RELEASE_TARGET := $(BUILD_DIR)/fw_release_test
FW_RELEASE_SOURCES := fw_release_test.c $(COMMON_DIR)/fw_release.c \
	$(COMMON_DIR)/miniz/miniz_tinfl.c
FW_RELEASE_DEPS := $(COMMON_DIR)/fw_release.h $(COMMON_DIR)/miniz/miniz.h \
	$(COMMON_DIR)/../include/zz9000_capture_calibration.h

.PHONY: test clean

test: $(TARGET) $(CFG_TARGET) $(VCAP_TARGET) $(VCAP_CLIENT_TARGET) $(EDIT_TARGET) $(GAMMA_TARGET) $(GEOMETRY_TARGET) $(CAPTURE_TARGET) $(CAPTURE_LIFECYCLE_TARGET) $(SAMPLING_CALIBRATION_TARGET) $(VCAP_SAMPLING_SCREEN_TARGET) $(ZZTOP_SETTINGS_TARGET) $(FW_RELEASE_TARGET)
	$(TARGET)
	$(CFG_TARGET)
	$(VCAP_TARGET)
	$(VCAP_CLIENT_TARGET)
	$(EDIT_TARGET)
	$(GAMMA_TARGET)
	$(GEOMETRY_TARGET)
	$(CAPTURE_TARGET)
	$(CAPTURE_LIFECYCLE_TARGET)
	$(SAMPLING_CALIBRATION_TARGET)
	$(VCAP_SAMPLING_SCREEN_TARGET)
	$(ZZTOP_SETTINGS_TARGET)
	$(FW_RELEASE_TARGET)

$(TARGET): $(SOURCES) $(DEPS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(SOURCES)

$(FW_RELEASE_TARGET): $(FW_RELEASE_SOURCES) $(FW_RELEASE_DEPS)
	@mkdir -p $(BUILD_DIR)
	$(CC) -I$(COMMON_DIR) -I$(COMMON_DIR)/../include $(CFLAGS) -std=c99 -Werror \
		-o $@ $(FW_RELEASE_SOURCES) -lz

# -w: the shared Amiga source casts board addresses through integers, which
# is correct on m68k and noisy on a 64-bit host.
$(CFG_TARGET): $(CFG_SOURCES) $(CFG_DEPS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFG_CPPFLAGS) $(CFLAGS) -w -o $@ $(CFG_SOURCES)

# ZZTop's card-file model over the same simulated card and FWUP transport.
$(ZZTOP_SETTINGS_TARGET): $(ZZTOP_SETTINGS_SOURCES) $(ZZTOP_SETTINGS_DEPS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(ZZTOP_SETTINGS_CPPFLAGS) $(CFLAGS) -w -o $@ $(ZZTOP_SETTINGS_SOURCES)

$(VCAP_TARGET): $(VCAP_SOURCES) $(VCAP_DEPS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(VCAP_CPPFLAGS) $(CFLAGS) -o $@ $(VCAP_SOURCES)

$(VCAP_CLIENT_TARGET): $(VCAP_CLIENT_SOURCES) $(VCAP_CLIENT_DEPS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(VCAP_CPPFLAGS) $(CFLAGS) -std=c99 -Werror -o $@ $(VCAP_CLIENT_SOURCES)
$(EDIT_TARGET): $(EDIT_SOURCES) $(EDIT_DEPS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(EDIT_CPPFLAGS) $(CFLAGS) -std=c99 -Werror -o $@ $(EDIT_SOURCES)

# Output-gamma helpers shared by ZZGamma, the CFG editor model and ZZTop's
# Display page: parser (pinned to the firmware's parse_gamma), slider
# level mapping and readout text.
$(GAMMA_TARGET): zz9000_gamma_test.c $(COMMON_DIR)/../include/zz9000_gamma.h
	@mkdir -p $(BUILD_DIR)
	$(CC) -I$(COMMON_DIR)/../include $(CFLAGS) -std=c99 -Werror -o $@ \
		zz9000_gamma_test.c

$(GEOMETRY_TARGET): $(GEOMETRY_SOURCES) $(GEOMETRY_DEPS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(GEOMETRY_CPPFLAGS) $(CFLAGS) -std=c99 -Werror -o $@ $(GEOMETRY_SOURCES)

$(CAPTURE_TARGET): ../../tests/capture_calibration_test.c ../../include/zz9000_capture_calibration.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -std=c99 -Werror -I../../include -o $@ $<

$(CAPTURE_LIFECYCLE_TARGET): zzcapture_lifecycle_test.c stubs/zzcapture_test_platform.h \
		../../ZZCapture/ZZCapture.c ../../common/zz_vcap_client.c ../../common/zz_vcap_live.c \
		../../include/zz9000_capture_calibration.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -std=c99 -Werror -DZZCAPTURE_TEST_IO -Istubs -I../../common -I../../include -o $@ $< ../../common/zz_vcap_client.c ../../common/zz_vcap_live.c
$(SAMPLING_CALIBRATION_TARGET): sampling_calibration_test.c ../../include/zz9000_sampling_calibration.h \
		../../include/zz9000_capture_calibration.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -std=c99 -Werror -I../../include -o $@ $<

$(VCAP_SAMPLING_SCREEN_TARGET): $(VCAP_SAMPLING_SCREEN_SOURCES) $(VCAP_SAMPLING_SCREEN_DEPS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(VCAP_SAMPLING_SCREEN_CPPFLAGS) $(CFLAGS) -std=c99 -Werror \
		-o $@ $(VCAP_SAMPLING_SCREEN_SOURCES)

clean:
	rm -rf $(BUILD_DIR)
