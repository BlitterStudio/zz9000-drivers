CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra -Werror -std=c11

BUILD_DIR := build
TARGETS := $(BUILD_DIR)/zzusb_legacy_model_test \
	$(BUILD_DIR)/zzusb_engine_test \
	$(BUILD_DIR)/zzusb_error_test \
	$(BUILD_DIR)/zzusb_diag_test \
	$(BUILD_DIR)/zzusb_interrupt_test \
	$(BUILD_DIR)/zzusb_iso_test

.PHONY: test clean

test: $(TARGETS)
	$(BUILD_DIR)/zzusb_legacy_model_test
	$(BUILD_DIR)/zzusb_engine_test
	$(BUILD_DIR)/zzusb_error_test
	$(BUILD_DIR)/zzusb_diag_test
	$(BUILD_DIR)/zzusb_interrupt_test
	$(BUILD_DIR)/zzusb_iso_test

$(BUILD_DIR)/zzusb_legacy_model_test: zzusb_legacy_model_test.c fixtures/legacy_mailbox.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -Ifixtures -o $@ zzusb_legacy_model_test.c

$(BUILD_DIR)/zzusb_engine_test: zzusb_engine_test.c ../zzusb_engine.c ../zzusb_engine.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -I.. -o $@ zzusb_engine_test.c ../zzusb_engine.c

$(BUILD_DIR)/zzusb_error_test: zzusb_error_test.c ../zzusb_engine.c ../zzusb_engine.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -I.. -o $@ zzusb_error_test.c ../zzusb_engine.c

$(BUILD_DIR)/zzusb_diag_test: zzusb_diag_test.c ../zzusb_engine.c ../zzusb_engine.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -I.. -o $@ zzusb_diag_test.c ../zzusb_engine.c

$(BUILD_DIR)/zzusb_interrupt_test: zzusb_interrupt_test.c ../zzusb_interrupt.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -I.. -o $@ zzusb_interrupt_test.c

$(BUILD_DIR)/zzusb_iso_test: zzusb_iso_test.c ../zzusb_iso.c ../zzusb_iso.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -I.. -o $@ zzusb_iso_test.c ../zzusb_iso.c

clean:
	rm -rf $(BUILD_DIR)
