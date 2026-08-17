#ifndef ZZ9000_VIDEOCAP_MODE_H
#define ZZ9000_VIDEOCAP_MODE_H

#include "zz9000_hw.h"
#include "zzcfg_query.h"

static inline UWORD zz_vcap_mode_sanitize(UWORD mode, UWORD firmware_capabilities)
{
    if (mode == ZZ_VMODE_720x576) return ZZ_VMODE_720x576;
    if (mode == ZZ_VMODE_CENTERED_1080P_60 &&
        (firmware_capabilities & ZZ_FW_CAP_VIDEOCAP_CENTERED_1080P))
        return ZZ_VMODE_CENTERED_1080P_60;
    return ZZ_VMODE_800x600;
}

static inline int zz_vcap_mode_uses_native_pan(UWORD mode)
{
    return mode == ZZ_VMODE_800x600 || mode == ZZ_VMODE_CENTERED_1080P_60;
}

#endif
