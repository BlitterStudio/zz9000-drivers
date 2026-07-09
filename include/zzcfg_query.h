/*
 * Header-only ZZ9000.CFG value query (firmware ABI >= 2.3, issue #33).
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Self-contained so device drivers (net/ahi/mhi) can consult the
 * SD-card config without linking the full common/zzcfg_amiga.c client
 * or pulling in proto headers. On firmware older than 2.3 the register
 * group reads as zero, so every key reports "absent" and callers fall
 * back to their ENV: variables - no version check needed.
 */
#ifndef ZZCFG_QUERY_H
#define ZZCFG_QUERY_H

#include <exec/types.h>

#ifndef ZZ_REG_CONFIG_KEY
#define ZZ_REG_CONFIG_KEY        0xE8
#define ZZ_REG_CONFIG_PRESENT    0xEA
#define ZZ_REG_CONFIG_FILE       0xEC
#define ZZ_REG_CONFIG_FILE_LEN   0xEE
#endif

/* Key ids (mirror firmware zz_config.h). */
#define ZZ_CFG_KEY_LOADED          0
#define ZZ_CFG_KEY_VIDEOCAP_MODE   1
#define ZZ_CFG_KEY_NS_VSYNC        2
#define ZZ_CFG_KEY_SCANLINE_MODE   3
#define ZZ_CFG_KEY_SCANLINE_PARITY 4
#define ZZ_CFG_KEY_INT2            5
#define ZZ_CFG_KEY_MAC_HI          6
#define ZZ_CFG_KEY_MAC_MID         7
#define ZZ_CFG_KEY_MAC_LO          8
#define ZZ_CFG_KEY_OFFSCREEN_BITMAPS 9

/* ZZ_REG_CONFIG_FILE statuses (mirror firmware zz_config_file_status). */
#define ZZ_CFG_FILE_OK           0
#define ZZ_CFG_FILE_NO_FILE      1
#define ZZ_CFG_FILE_IO_ERROR     2
#define ZZ_CFG_FILE_IDLE         0xFFFF

/* enum zz_video_modes values the config interface deals in. */
#define ZZ_VMODE_800x600         1
#define ZZ_VMODE_720x576         6

/* Query one parsed ZZ9000.CFG value. *present is set to 1 if the key
 * was given in the config file (for ZZ_CFG_KEY_LOADED: whether the
 * file was found at cold boot); the value reads as 0 when absent. */
static inline UWORD zzcfg_query(ULONG board, UWORD key, UWORD *present)
{
    volatile UWORD *key_reg =
        (volatile UWORD *)(board + ZZ_REG_CONFIG_KEY);
    volatile UWORD *present_reg =
        (volatile UWORD *)(board + ZZ_REG_CONFIG_PRESENT);
    UWORD value, p;

    *key_reg = key;
    value = *key_reg;
    p = *present_reg;

    if (present) *present = p;
    return value;
}

#endif /* ZZCFG_QUERY_H */
