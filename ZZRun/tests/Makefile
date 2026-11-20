CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra -Werror -std=c11
BUILD_DIR ?= build

# The tests include the AAR contract (aar/transport.h, aar/app.h) and the
# aar.library client headers (libraries/aar.h, libraries/aar_zz9000.h), the
# same headers ZZRun builds against. aar.library itself is not linked: the
# tests that need its behaviour carry a fake of the call. AAR_DIR is an aar
# checkout (the commit the firmware pins in sdk/AAR_REF); FIRMWARE_DIR is the
# zz9000-firmware checkout (ZZ9K_FIRMWARE_DIR as CI sets it, else the sibling
# checkout), run `sh sdk/scripts/fetch-aar.sh` in it for sdk/aar. The host
# stubs of exec/types.h and friends come with the aar client tests (they give
# the headers the 68k word sizes on a 64-bit host).
FIRMWARE_DIR ?= $(if $(ZZ9K_FIRMWARE_DIR),$(ZZ9K_FIRMWARE_DIR),$(abspath ../../../zz9000-firmware))
AAR_DIR ?= $(FIRMWARE_DIR)/sdk/aar
INCLUDES = -I.. -I$(AAR_DIR)/client/tests/stubs -I$(AAR_DIR)/client/include \
    -I$(AAR_DIR)/include

.PHONY: test clean check-headers

test: check-headers $(BUILD_DIR)/zzrun_policy_test $(BUILD_DIR)/zzrun_serve_test
	$(BUILD_DIR)/zzrun_policy_test
	$(BUILD_DIR)/zzrun_serve_test

check-headers:
	@test -f $(AAR_DIR)/include/aar/transport.h || { \
	    echo "ERROR: aar/transport.h not found in AAR_DIR=$(AAR_DIR)" >&2; exit 1; }
	@test -f $(AAR_DIR)/client/include/libraries/aar.h || { \
	    echo "ERROR: libraries/aar.h not found in AAR_DIR=$(AAR_DIR); the aar checkout is older than aar.library" >&2; exit 1; }
	@test -f $(AAR_DIR)/client/tests/stubs/exec/types.h || { \
	    echo "ERROR: client/tests/stubs not found in AAR_DIR=$(AAR_DIR)" >&2; exit 1; }

$(BUILD_DIR)/zzrun_policy_test: zzrun_policy_test.c ../zzrun_policy.h ../zzrun_policy.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ zzrun_policy_test.c ../zzrun_policy.c

$(BUILD_DIR)/zzrun_serve_test: zzrun_serve_test.c ../zzrun_serve.h ../zzrun_serve.c \
    ../zzrun_policy.h ../zzrun_policy.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ zzrun_serve_test.c ../zzrun_serve.c \
	    ../zzrun_policy.c

clean:
	rm -rf $(BUILD_DIR)
