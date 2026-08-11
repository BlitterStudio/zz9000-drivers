/* Host tests for the pure live video-capture calibration model. */
#include <stdio.h>

#include "zz_vcap_live.h"

static int checks;
static int failures;

#define CHECK(expr, message) do { \
    checks++; \
    if (!(expr)) { \
        failures++; \
        printf("  FAIL: %s (%s:%d)\n", message, __FILE__, __LINE__); \
    } \
} while (0)

static struct zz_vcap_control mixed_control(void)
{
    struct zz_vcap_control control;

    control.sample = 2;
    control.full_width = 1;
    control.crop_h = 0;
    control.crop_v = 4095;
    control.crop_h_present = 1;
    control.crop_v_present = 0;
    return control;
}

static void test_capability(void)
{
    CHECK(zz_vcap_live_supported(0x020a, ZZ_VCAP_LIVE_CAPABILITY_VALUE),
        "firmware 2.10 and exact capability are supported");
    CHECK(zz_vcap_live_supported(0x020b, ZZ_VCAP_LIVE_CAPABILITY_VALUE),
        "newer compatible firmware is supported");
    CHECK(!zz_vcap_live_supported(0x0209, ZZ_VCAP_LIVE_CAPABILITY_VALUE),
        "old firmware is rejected");
    CHECK(!zz_vcap_live_supported(0x020a, 0x564d010fUL),
        "wrong magic is rejected");
    CHECK(!zz_vcap_live_supported(0x020a, 0x564c020fUL),
        "wrong protocol is rejected");
    CHECK(!zz_vcap_live_supported(0x020a, 0x564c0107UL),
        "missing feature is rejected");
    CHECK(!zz_vcap_live_supported(0x020a, 0x564c011fUL),
        "reserved feature is rejected");
}

static void test_pack_unpack(void)
{
    struct zz_vcap_control input = mixed_control();
    struct zz_vcap_control output;
    ULONG raw;
    UWORD h;
    UWORD v;

    raw = zz_vcap_control_pack(&input);
    CHECK(raw == (ZZ_VCAP_CROP_V_AUTO_FLAG |
        (ZZ_VCAP_CROP_V_COMPAT << 16) | 2UL | (1UL << 2)),
        "mixed Automatic/Custom control packs exactly");
    CHECK(zz_vcap_control_unpack(raw, &output), "valid raw control unpacks");
    CHECK(output.sample == 2 && output.full_width == 1,
        "sample and full-width unpack");
    CHECK(output.crop_h == 0 && output.crop_h_present == 1,
        "explicit horizontal zero survives unpack");
    CHECK(output.crop_v == ZZ_VCAP_CROP_V_COMPAT &&
        output.crop_v_present == 0,
        "vertical Automatic flag survives unpack");

    input.crop_v_present = 1;
    input.crop_v = 4095;
    raw = zz_vcap_control_pack(&input);
    CHECK(zz_vcap_control_unpack(raw, &output), "literal maximum unpacks");
    CHECK(output.crop_v == 4095 && output.crop_v_present == 1,
        "explicit vertical 4095 survives unpack");
    CHECK(!zz_vcap_control_unpack(raw | (1UL << 30), &output),
        "reserved raw bits are rejected");
    input.sample = 3;
    CHECK(!zz_vcap_control_valid(&input), "sample value 3 is rejected");

    zz_vcap_effective_unpack((40UL << 16) | 279UL, &h, &v);
    CHECK(h == 279 && v == 40, "effective crop unpacks without mirroring RTL");
}

static void test_status_and_sequences(void)
{
    struct zz_vcap_status status;
    ULONG raw;

    raw = (255UL << 24) | (254UL << 16) |
        ZZ_VCAP_STATUS_STANDARD_VALID | ZZ_VCAP_STATUS_NTSC |
        ZZ_VCAP_STATUS_APPLIED_VALID | ZZ_VCAP_STATUS_BUSY;
    zz_vcap_status_unpack(raw, &status);
    CHECK(status.request_sequence == 255 && status.applied_sequence == 254,
        "status sequences unpack");
    CHECK(status.standard_valid && status.ntsc && status.applied_valid && status.busy,
        "status flags unpack");
    CHECK(zz_vcap_next_sequence(255) == 0, "request sequence wraps modulo 256");
    CHECK(!zz_vcap_request_complete(raw, 255), "busy request is incomplete");
    raw = ZZ_VCAP_STATUS_STANDARD_VALID | ZZ_VCAP_STATUS_APPLIED_VALID;
    CHECK(zz_vcap_request_complete(raw, 0), "wrapped idle request completes");
    CHECK(zz_vcap_request_complete(raw | ZZ_VCAP_STATUS_REJECTED, 0),
        "sticky rejection does not hide a completed request");
    CHECK(zz_vcap_request_result(raw | ZZ_VCAP_STATUS_REJECTED, 0,
        0x12345678UL, 0x12345678UL) == ZZ_VCAP_REQUEST_APPLIED,
        "matching acknowledged raw wins over sticky rejection");
    CHECK(zz_vcap_request_result(raw | ZZ_VCAP_STATUS_REJECTED, 0,
        0x87654321UL, 0x12345678UL) == ZZ_VCAP_REQUEST_CONFLICT,
        "matching sequence with another raw value is a conflict");
    CHECK(zz_vcap_request_result(raw | (1UL << 24), 0,
        0x12345678UL, 0x12345678UL) == ZZ_VCAP_REQUEST_PENDING,
        "a different sequence remains pending");
    CHECK(!zz_vcap_request_complete(raw | (1UL << 24), 0),
        "wrong request sequence cannot complete");
    CHECK(!zz_vcap_request_complete(raw | (1UL << 16), 0),
        "wrong applied sequence cannot complete");
    CHECK(zz_vcap_snapshot_status_valid(raw, raw),
        "stable valid idle status accepts a snapshot");
    CHECK(zz_vcap_snapshot_status_valid(raw | ZZ_VCAP_STATUS_REJECTED,
        raw | ZZ_VCAP_STATUS_REJECTED),
        "a rejected prior commit does not invalidate applied state");
    CHECK(!zz_vcap_snapshot_status_valid(raw, raw | ZZ_VCAP_STATUS_NTSC),
        "outer status change rejects a snapshot");
    CHECK(!zz_vcap_snapshot_status_valid(raw | ZZ_VCAP_STATUS_BUSY,
        raw | ZZ_VCAP_STATUS_BUSY), "busy snapshot is rejected");
    CHECK(!zz_vcap_snapshot_status_valid(0, 0),
        "invalid applied and standard state is rejected");
}

static void test_movement(void)
{
    struct zz_vcap_working work;

    work.control = mixed_control();
    work.crop_h = 279;
    work.crop_v = 40;
    CHECK(zz_vcap_adjust(&work, ZZ_VCAP_MOVE_LEFT, 0),
        "left fine adjustment changes state");
    CHECK(work.crop_h == 280 && work.crop_v == 40,
        "left moves picture left by increasing horizontal crop");
    CHECK(work.control.crop_h_present && work.control.crop_v_present,
        "first effective adjustment makes both axes Custom");
    CHECK(work.control.crop_h == 280 && work.control.crop_v == 40,
        "Custom raw values follow displayed effective values");
    CHECK(zz_vcap_adjust(&work, ZZ_VCAP_MOVE_RIGHT, 1),
        "right coarse adjustment changes state");
    CHECK(work.crop_h == 264, "Shift+Right subtracts 16");
    CHECK(zz_vcap_adjust(&work, ZZ_VCAP_MOVE_UP, 1),
        "up coarse adjustment changes state");
    CHECK(work.crop_v == 56, "Shift+Up adds 16");
    CHECK(zz_vcap_adjust(&work, ZZ_VCAP_MOVE_DOWN, 0),
        "down fine adjustment changes state");
    CHECK(work.crop_v == 55, "Down subtracts one");

    work.control.crop_h_present = 0;
    work.control.crop_v_present = 0;
    work.crop_h = 4095;
    work.crop_v = 0;
    CHECK(!zz_vcap_adjust(&work, ZZ_VCAP_MOVE_LEFT, 1),
        "clamped left boundary is a no-op");
    CHECK(!work.control.crop_h_present && !work.control.crop_v_present,
        "boundary no-op does not convert Automatic to Custom");
    CHECK(!zz_vcap_adjust(&work, ZZ_VCAP_MOVE_DOWN, 1),
        "clamped down boundary is a no-op");
    CHECK(!work.control.crop_h_present && !work.control.crop_v_present,
        "second boundary no-op preserves Automatic");
    zz_vcap_accept(&work);
    CHECK(work.control.crop_h_present && work.control.crop_v_present,
        "Enter without movement makes both axes Custom");
    CHECK(work.control.crop_h == 4095 && work.control.crop_v == 0,
        "Enter materializes displayed effective values without a jump");
}

static void test_path_preview_and_anchors(void)
{
    struct zz_vcap_control a = mixed_control();
    struct zz_vcap_control b = a;
    struct zz_vcap_path path_a;
    struct zz_vcap_path path_b;
    struct zz_vcap_snapshot snapshot;
    struct zz_vcap_snapshot restored;
    struct zz_vcap_anchors anchors;

    path_a.sample = 2;
    path_a.full_width = 1;
    path_b = path_a;
    CHECK(zz_vcap_path_equal(&path_a, &path_b),
        "same sample/full-width path matches");
    path_b.sample = 1;
    CHECK(!zz_vcap_path_equal(&path_a, &path_b),
        "sample change invalidates path signature");
    path_b = path_a;
    path_b.full_width = 0;
    CHECK(!zz_vcap_path_equal(&path_a, &path_b),
        "full-width change invalidates path signature");

    CHECK(zz_vcap_control_equal(&a, &b), "identical preview matches controls");
    b.crop_v_present = 1;
    CHECK(!zz_vcap_control_equal(&a, &b),
        "preview equality includes independent presence flags");
    b = a;
    b.sample = 1;
    CHECK(!zz_vcap_control_equal(&a, &b),
        "preview equality includes sample mode");

    snapshot.raw = zz_vcap_control_pack(&a);
    snapshot.effective_h = 279;
    snapshot.effective_v = 40;
    snapshot.status = ZZ_VCAP_STATUS_STANDARD_VALID |
        ZZ_VCAP_STATUS_APPLIED_VALID;
    zz_vcap_anchors_init(&anchors);
    CHECK(!zz_vcap_anchor_load(&anchors, ZZ_VCAP_ANCHOR_SETTINGS, &restored),
        "unset anchor cannot be restored");
    zz_vcap_anchor_store(&anchors, ZZ_VCAP_ANCHOR_SETTINGS, &snapshot);
    zz_vcap_anchor_store(&anchors, ZZ_VCAP_ANCHOR_ADVANCED, &snapshot);
    zz_vcap_anchor_store(&anchors, ZZ_VCAP_ANCHOR_CALIBRATION, &snapshot);
    zz_vcap_anchor_store(&anchors, ZZ_VCAP_ANCHOR_PREVIEW, &snapshot);
    snapshot.raw = 0;
    CHECK(zz_vcap_anchor_load(&anchors, ZZ_VCAP_ANCHOR_CALIBRATION, &restored),
        "calibration anchor restores");
    CHECK(restored.raw == zz_vcap_control_pack(&a),
        "mixed-axis calibration snapshot restores exactly");
    CHECK(zz_vcap_anchor_load(&anchors, ZZ_VCAP_ANCHOR_ADVANCED, &restored),
        "Advanced boundary owns a restorable snapshot");
    CHECK(zz_vcap_anchor_load(&anchors, ZZ_VCAP_ANCHOR_SETTINGS, &restored),
        "Settings boundary owns a restorable snapshot");
    CHECK(zz_vcap_anchor_load(&anchors, ZZ_VCAP_ANCHOR_PREVIEW, &restored),
        "accepted preview owns an acknowledged snapshot");
    zz_vcap_anchor_clear(&anchors, ZZ_VCAP_ANCHOR_PREVIEW);
    CHECK(!zz_vcap_anchor_load(&anchors, ZZ_VCAP_ANCHOR_PREVIEW, &restored),
        "invalidated preview cannot be restored");
}

int main(void)
{
    test_capability();
    test_pack_unpack();
    test_status_and_sequences();
    test_movement();
    test_path_preview_and_anchors();
    printf("zz_vcap_live: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
