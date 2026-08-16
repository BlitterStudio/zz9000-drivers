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

    if (failures) return 1;
    puts("videocap mode: all checks passed");
    return 0;
}
