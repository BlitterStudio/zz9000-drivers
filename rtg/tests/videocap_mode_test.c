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
    CHECK(zz_vcap_mode_uses_native_pan(ZZ_VMODE_800x600));
    CHECK(zz_vcap_mode_uses_native_pan(ZZ_VMODE_CENTERED_1080P_60));
    CHECK(!zz_vcap_mode_uses_native_pan(ZZ_VMODE_720x576));

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
    CHECK(!zz_vcap_mode_is_centered(ZZ_VMODE_800x600));
    CHECK(zz_vcap_mode_uses_native_pan(ZZ_VMODE_CENTERED_1080P_50));
    /* The new bit alone grants nothing: 60 Hz still requires bit 3. */
    CHECK(zz_vcap_mode_sanitize(ZZ_VMODE_CENTERED_1080P_60,
        ZZ_FW_CAP_VIDEOCAP_CENTERED_1080P_50) == ZZ_VMODE_800x600);

    if (failures) return 1;
    puts("videocap mode: all checks passed");
    return 0;
}
