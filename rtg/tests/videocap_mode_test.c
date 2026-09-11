#include <stdio.h>
#include "../videocap_mode.h"

static int failures;
#define CHECK(expr) do { if (!(expr)) { \
    printf("FAIL:%d: %s\n", __LINE__, #expr); failures++; \
} } while (0)

int main(void)
{
    CHECK(zz_vcap_mode_sanitize(ZZ_VMODE_800x600, 0) == ZZ_VMODE_800x600);
    CHECK(zz_vcap_mode_sanitize(ZZ_VMODE_720x576, 0) == ZZ_VMODE_720x576);
    CHECK(zz_vcap_mode_sanitize(ZZ_VMODE_CENTERED_1080P_60, 0) ==
        ZZ_VMODE_800x600);
    CHECK(zz_vcap_mode_sanitize(ZZ_VMODE_CENTERED_1080P_60,
        ZZ_FW_CAP_VIDEOCAP_CENTERED_1080P) == ZZ_VMODE_CENTERED_1080P_60);
    CHECK(zz_vcap_mode_sanitize(99, ZZ_FW_CAP_VIDEOCAP_CENTERED_1080P) ==
        ZZ_VMODE_800x600);
    CHECK(zz_vcap_native_pan_offset(ZZ_VMODE_800x600,
        ZZ_FW_CAP_VIDEOCAP_SCANOUT_ORIGIN) == 0x00e00000UL);
    /* Without the capability bit the 800x600 family keeps the legacy
     * tuned constant: older firmware only corrects the origin inside
     * its mode-change trigger, so the raw base would misplace PAL. */
    CHECK(zz_vcap_native_pan_offset(ZZ_VMODE_800x600, 0) == 0x00dff2f8UL);
    /* 720x576 always used the capture base; keep that on old stacks. */
    CHECK(zz_vcap_native_pan_offset(ZZ_VMODE_720x576, 0) == 0x00e00000UL);
    CHECK(zz_vcap_native_pan_offset(ZZ_VMODE_CENTERED_1080P_60, 0) ==
        0x00dff2f8UL);

    /* 1080p50 needs BOTH centered capability bits: a 60-only stack must
     * not receive mode 7 it would silently run at 60 Hz. */
    CHECK(zz_vcap_mode_sanitize(ZZ_VMODE_CENTERED_1080P_50, 0) ==
        ZZ_VMODE_800x600);
    CHECK(zz_vcap_mode_sanitize(ZZ_VMODE_CENTERED_1080P_50,
        ZZ_FW_CAP_VIDEOCAP_CENTERED_1080P) == ZZ_VMODE_800x600);
    CHECK(zz_vcap_mode_sanitize(ZZ_VMODE_CENTERED_1080P_50,
        ZZ_FW_CAP_VIDEOCAP_CENTERED_1080P_50) == ZZ_VMODE_800x600);
    CHECK(zz_vcap_mode_sanitize(ZZ_VMODE_CENTERED_1080P_50,
        ZZ_FW_CAP_VIDEOCAP_CENTERED_1080P |
        ZZ_FW_CAP_VIDEOCAP_CENTERED_1080P_50) == ZZ_VMODE_CENTERED_1080P_50);
    CHECK(zz_vcap_mode_is_centered(ZZ_VMODE_CENTERED_1080P_50));
    CHECK(zz_vcap_mode_is_centered(ZZ_VMODE_CENTERED_1080P_60));
    CHECK(zz_vcap_native_pan_offset(ZZ_VMODE_CENTERED_1080P_50,
        ZZ_FW_CAP_VIDEOCAP_SCANOUT_ORIGIN) == 0x00dff2f8UL);
    /* The new bit alone grants nothing: 60 Hz still requires bit 3. */
    CHECK(zz_vcap_mode_sanitize(ZZ_VMODE_CENTERED_1080P_60,
        ZZ_FW_CAP_VIDEOCAP_CENTERED_1080P_50) == ZZ_VMODE_800x600);

    /* The virtual source-sync id 0x100 is forwarded only to firmware
     * advertising the whole experimental stack (bits 3|4|5); every
     * partial combination - old stacks included - falls back to the
     * monitor-safe default instead of an id old firmware would
     * misread. */
    CHECK(zz_vcap_mode_sanitize(ZZ_VMODE_CENTERED_1080P_MATCH,
        ZZ_FW_CAP_VIDEOCAP_CENTERED_1080P |
        ZZ_FW_CAP_VIDEOCAP_CENTERED_1080P_50) == ZZ_VMODE_800x600);
    CHECK(zz_vcap_mode_sanitize(ZZ_VMODE_CENTERED_1080P_MATCH,
        ZZ_FW_CAP_VIDEOCAP_CENTERED_1080P |
        ZZ_FW_CAP_VIDEOCAP_SOURCE_SYNC) == ZZ_VMODE_800x600);
    CHECK(zz_vcap_mode_sanitize(ZZ_VMODE_CENTERED_1080P_MATCH,
        ZZ_FW_CAP_VIDEOCAP_CENTERED_1080P_50 |
        ZZ_FW_CAP_VIDEOCAP_SOURCE_SYNC) == ZZ_VMODE_800x600);
    CHECK(zz_vcap_mode_sanitize(ZZ_VMODE_CENTERED_1080P_MATCH,
        ZZ_FW_CAP_VIDEOCAP_CENTERED_1080P |
        ZZ_FW_CAP_VIDEOCAP_CENTERED_1080P_50 |
        ZZ_FW_CAP_VIDEOCAP_SOURCE_SYNC) == ZZ_VMODE_CENTERED_1080P_MATCH);
    CHECK(zz_vcap_mode_is_centered(ZZ_VMODE_CENTERED_1080P_MATCH));
    CHECK(zz_vcap_native_pan_offset(ZZ_VMODE_CENTERED_1080P_MATCH, 0) ==
        0x00dff2f8UL);

    /* The extra capability bit leaves the fixed modes' gating alone. */
    CHECK(zz_vcap_mode_sanitize(ZZ_VMODE_CENTERED_1080P_60,
        ZZ_FW_CAP_VIDEOCAP_CENTERED_1080P |
        ZZ_FW_CAP_VIDEOCAP_CENTERED_1080P_50 |
        ZZ_FW_CAP_VIDEOCAP_SOURCE_SYNC) == ZZ_VMODE_CENTERED_1080P_60);
    CHECK(zz_vcap_mode_sanitize(ZZ_VMODE_CENTERED_1080P_50,
        ZZ_FW_CAP_VIDEOCAP_CENTERED_1080P |
        ZZ_FW_CAP_VIDEOCAP_CENTERED_1080P_50 |
        ZZ_FW_CAP_VIDEOCAP_SOURCE_SYNC) == ZZ_VMODE_CENTERED_1080P_50);
    CHECK(zz_vcap_mode_sanitize(ZZ_VMODE_720x576,
        ZZ_FW_CAP_VIDEOCAP_CENTERED_1080P |
        ZZ_FW_CAP_VIDEOCAP_CENTERED_1080P_50 |
        ZZ_FW_CAP_VIDEOCAP_SOURCE_SYNC) == ZZ_VMODE_720x576);

    if (failures) return 1;
    puts("videocap mode: all checks passed");
    return 0;
}
