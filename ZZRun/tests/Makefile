CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra -Werror -std=c11
BUILD_DIR ?= build

.PHONY: test clean

test: $(BUILD_DIR)/zzrun_policy_test
	$(BUILD_DIR)/zzrun_policy_test

$(BUILD_DIR)/zzrun_policy_test: zzrun_policy_test.c ../zzrun_policy.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -I.. -o $@ zzrun_policy_test.c

clean:
	rm -rf $(BUILD_DIR)
