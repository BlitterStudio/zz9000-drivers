#ifndef ZZ9000_VIDEOCAP_MODE_H
#define ZZ9000_VIDEOCAP_MODE_H

#include "zz9000_hw.h"
#include "zzcfg_query.h"

/* Fixed centered modes reuse the firmware's centered-output path: the
 * 50 Hz variant additionally needs capability bit 4, so a 60-only
 * stack never receives a mode id it would silently run at 60 Hz.
 * ZZ_VMODE_CENTERED_1080P_MATCH is a virtual id (never a preset row):
 * it drives the centered output with source-synced refresh and needs
 * all three capability bits, so mixed stacks never see 0x100. */
static inline int zz_vcap_mode_is_centered(UWORD mode)
{
    return mode == ZZ_VMODE_CENTERED_1080P_60 ||
        mode == ZZ_VMODE_CENTERED_1080P_50 ||
        mode == ZZ_VMODE_CENTERED_1080P_MATCH;
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
    if (mode == ZZ_VMODE_CENTERED_1080P_MATCH &&
        (firmware_capabilities & (ZZ_FW_CAP_VIDEOCAP_CENTERED_1080P |
            ZZ_FW_CAP_VIDEOCAP_CENTERED_1080P_50 |
            ZZ_FW_CAP_VIDEOCAP_SOURCE_SYNC)) ==
            (ZZ_FW_CAP_VIDEOCAP_CENTERED_1080P |
             ZZ_FW_CAP_VIDEOCAP_CENTERED_1080P_50 |
             ZZ_FW_CAP_VIDEOCAP_SOURCE_SYNC))
        return ZZ_VMODE_CENTERED_1080P_MATCH;
    return ZZ_VMODE_800x600;
}

/* Capture-area pan written at native-screen activation. Only the
 * centered profiles keep the legacy tuned origin 0x00dff2f8: there the
 * firmware never overrides the driver-provided canvas base. Every
 * other path is firmware-owned geometry — the firmware re-derives the
 * scanout origin from the detected standard at each capture restart —
 * so the driver just points at the plain capture base (issue #84). */
static inline ULONG zz_vcap_native_pan_offset(UWORD mode)
{
    return zz_vcap_mode_is_centered(mode) ? 0x00dff2f8UL : 0x00e00000UL;
}

#endif
