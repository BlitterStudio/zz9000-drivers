CC ?= cc
CFLAGS ?= -O2 -g
BUILD_DIR ?= build

TARGET := $(BUILD_DIR)/register_cache_test
TARGETS := $(TARGET) $(BUILD_DIR)/p96_settings_test \
	$(BUILD_DIR)/offscreen_bitmap_test $(BUILD_DIR)/overlay_feature_test \
	$(BUILD_DIR)/memory_layout_test $(BUILD_DIR)/videocap_mode_test \
	$(BUILD_DIR)/mode_timing_test

.PHONY: test clean

test: $(TARGETS)
	$(TARGET)
	$(BUILD_DIR)/p96_settings_test
	$(BUILD_DIR)/offscreen_bitmap_test
	$(BUILD_DIR)/overlay_feature_test
	$(BUILD_DIR)/memory_layout_test
	$(BUILD_DIR)/videocap_mode_test
	$(BUILD_DIR)/mode_timing_test

$(TARGET): register_cache_test.c ../blitter_cache.h
	@mkdir -p $(BUILD_DIR)
	$(CC) -Wall -Wextra $(CFLAGS) -I.. -o $@ register_cache_test.c

$(BUILD_DIR)/p96_settings_test: p96_settings_test.c ../mode_timing.h
	@mkdir -p $(BUILD_DIR)
	$(CC) -Wall -Wextra $(CFLAGS) -I.. -o $@ p96_settings_test.c

$(BUILD_DIR)/offscreen_bitmap_test: offscreen_bitmap_test.c ../offscreen_bitmap.h
	@mkdir -p $(BUILD_DIR)
	$(CC) -Wall -Wextra $(CFLAGS) -I.. -o $@ offscreen_bitmap_test.c

$(BUILD_DIR)/overlay_feature_test: overlay_feature_test.c ../overlay_feature.h
	@mkdir -p $(BUILD_DIR)
	$(CC) -Wall -Wextra $(CFLAGS) -I.. -o $@ overlay_feature_test.c

$(BUILD_DIR)/memory_layout_test: memory_layout_test.c ../memory_layout.h
	@mkdir -p $(BUILD_DIR)
	$(CC) -Wall -Wextra $(CFLAGS) -I.. -o $@ memory_layout_test.c

$(BUILD_DIR)/videocap_mode_test: videocap_mode_test.c ../videocap_mode.h
	@mkdir -p $(BUILD_DIR)
	$(CC) -Wall -Wextra $(CFLAGS) -I../../common/tests/stubs -I.. \
		-I../../include -o $@ videocap_mode_test.c

$(BUILD_DIR)/mode_timing_test: mode_timing_test.c ../mode_timing.h
	@mkdir -p $(BUILD_DIR)
	$(CC) -Wall -Wextra $(CFLAGS) -I.. -o $@ mode_timing_test.c

clean:
	rm -rf $(BUILD_DIR)
