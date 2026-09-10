#ifndef ZZ9000_VIDEOCAP_MODE_H
#define ZZ9000_VIDEOCAP_MODE_H

#include "zz9000_hw.h"
#include "zzcfg_query.h"

/* Both centered modes reuse the firmware's centered-output path: the
 * 50 Hz variant additionally needs capability bit 4, so a 60-only
 * stack never receives a mode id it would silently run at 60 Hz. */
static inline int zz_vcap_mode_is_centered(UWORD mode)
{
    return mode == ZZ_VMODE_CENTERED_1080P_60 ||
        mode == ZZ_VMODE_CENTERED_1080P_50;
}

static inline UWORD zz_vcap_mode_sanitize(UWORD mode, UWORD firmware_capabilities)
{
    if (mode == ZZ_VMODE_720x576) return ZZ_VMODE_720x576;
    if (mode == ZZ_VMODE_CENTERED_1080P_60 &&
        (firmware_capabilities & ZZ_FW_CAP_VIDEOCAP_CENTERED_1080P))
        return ZZ_VMODE_CENTERED_1080P_60;
    if (mode == ZZ_VMODE_CENTERED_1080P_50 &&
        (firmware_capabilities & (ZZ_FW_CAP_VIDEOCAP_CENTERED_1080P |
            ZZ_FW_CAP_VIDEOCAP_CENTERED_1080P_50)) ==
            (ZZ_FW_CAP_VIDEOCAP_CENTERED_1080P |
             ZZ_FW_CAP_VIDEOCAP_CENTERED_1080P_50))
        return ZZ_VMODE_CENTERED_1080P_50;
    return ZZ_VMODE_800x600;
}

static inline int zz_vcap_mode_uses_native_pan(UWORD mode)
{
    return mode == ZZ_VMODE_800x600 || zz_vcap_mode_is_centered(mode);
}

#endif
