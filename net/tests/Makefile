# Host-side tests for ZZ9000Net.device logic that builds without the
# Amiga SDK (extension negotiation, claim gating, drain sum, verification).
# Follows the rtg/tests convention: plain host cc, exit code is pass/fail.
#
# SPDX-License-Identifier: GPL-3.0-or-later

CC ?= cc
CFLAGS ?= -O2 -g
BUILD_DIR ?= build

STUBS := ../../common/tests/stubs

TARGETS := $(BUILD_DIR)/claim_test

.PHONY: test clean

test: $(TARGETS)
	$(BUILD_DIR)/claim_test

$(BUILD_DIR)/claim_test: claim_test.c ../zznet_ext.h ../anxs2ext.h $(STUBS)/exec/types.h
	@mkdir -p $(BUILD_DIR)
	$(CC) -Wall -Wextra $(CFLAGS) -DZZNET_HOST_TEST -I$(STUBS) -I.. -o $@ claim_test.c

clean:
	rm -rf $(BUILD_DIR)
