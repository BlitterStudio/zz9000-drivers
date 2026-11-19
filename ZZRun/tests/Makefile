CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra -Werror -std=c11
BUILD_DIR ?= build

# The tests include the AAR transport contract (aar/transport.h), the
# ZZ9000 private wire (zzapp_proto.h) and the declarations of the client
# transport module (aar_xport_zz9000.h), the same headers ZZRun builds
# against. The module itself is not linked: the tests stub the calls they
# check. AAR_DIR is an aar checkout (the commit the firmware pins in
# sdk/AAR_REF); FIRMWARE_DIR is the zz9000-firmware checkout (ZZ9K_FIRMWARE_DIR
# as CI sets it, else the sibling checkout), run `sh sdk/scripts/fetch-aar.sh`
# in it for sdk/aar.
FIRMWARE_DIR ?= $(if $(ZZ9K_FIRMWARE_DIR),$(ZZ9K_FIRMWARE_DIR),$(abspath ../../../zz9000-firmware))
AAR_DIR ?= $(FIRMWARE_DIR)/sdk/aar
PROTO_DIR ?= $(FIRMWARE_DIR)/ZZ9000_proto.sdk/ZZ9000OS/src
XPORT_DIR ?= $(FIRMWARE_DIR)/sdk/amiga/src
INCLUDES = -I.. -I$(AAR_DIR)/include -I$(PROTO_DIR) -I$(XPORT_DIR)

.PHONY: test clean check-headers

test: check-headers $(BUILD_DIR)/zzrun_policy_test $(BUILD_DIR)/zzrun_serve_test
	$(BUILD_DIR)/zzrun_policy_test
	$(BUILD_DIR)/zzrun_serve_test

check-headers:
	@test -f $(AAR_DIR)/include/aar/transport.h || { \
	    echo "ERROR: aar/transport.h not found in AAR_DIR=$(AAR_DIR)" >&2; exit 1; }
	@test -f $(PROTO_DIR)/zzapp_proto.h || { \
	    echo "ERROR: zzapp_proto.h not found in PROTO_DIR=$(PROTO_DIR)" >&2; exit 1; }
	@test -f $(XPORT_DIR)/aar_xport_zz9000.h || { \
	    echo "ERROR: aar_xport_zz9000.h not found in XPORT_DIR=$(XPORT_DIR)" >&2; exit 1; }

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
