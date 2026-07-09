/*
 * Amiga-side client for the ZZ9000.CFG SD-card config file.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Reads the firmware's parsed config values (ZZ_REG_CONFIG_KEY query),
 * fetches the raw file (ZZ_REG_CONFIG_FILE + shared buffer), generates
 * a fresh commented config from a value set, and writes it back over
 * the FWUP file-push path. Firmware >= 2.2 (issue #33). Used by ZZTop's
 * Settings window.
 */
#ifndef ZZCFG_AMIGA_H
#define ZZCFG_AMIGA_H

#include <exec/types.h>
#include <stdint.h>

/* Matches the firmware's boot-time parse cap (ZZ_CONFIG_MAX_SIZE). */
#define ZZCFG_MAX_SIZE   4096
#define ZZCFG_MAC_CHARS  17          /* aa:bb:cc:dd:ee:ff */
#define ZZCFG_HDF_CHARS  63

/* Key ids for zzcfg_query (mirror firmware zz_config.h). */
#define ZZ_CFG_KEY_LOADED          0
#define ZZ_CFG_KEY_VIDEOCAP_MODE   1
#define ZZ_CFG_KEY_NS_VSYNC        2
#define ZZ_CFG_KEY_SCANLINE_MODE   3
#define ZZ_CFG_KEY_SCANLINE_PARITY 4
#define ZZ_CFG_KEY_INT2            5
#define ZZ_CFG_KEY_MAC_HI          6
#define ZZ_CFG_KEY_MAC_MID         7
#define ZZ_CFG_KEY_MAC_LO          8

/* zzcfg_read_raw statuses (mirror firmware zz_config_file_status). */
#define ZZ_CFG_FILE_OK           0
#define ZZ_CFG_FILE_NO_FILE      1
#define ZZ_CFG_FILE_IO_ERROR     2
#define ZZ_CFG_FILE_IDLE         0xFFFF

/* enum zz_video_modes values the config interface deals in. */
#define ZZ_VMODE_800x600         1
#define ZZ_VMODE_720x576         6

/* Everything ZZTop's Settings window edits. mac/hdf are C strings;
 * an empty string means "not configured" and is emitted as a
 * commented-out example line. */
struct zzcfg_values {
    UWORD videocap_pal;      /* 0 = 800x600 60Hz, 1 = PAL 720x576 50Hz */
    UWORD vsync;             /* 0 = off, 1 = pal, 2 = ntsc */
    UWORD scanline_mode;     /* 0-3 */
    UWORD scanline_parity;   /* 0-1 */
    UWORD int2;              /* 0-1 */
    char  mac[ZZCFG_MAC_CHARS + 3];
    char  hdf[ZZCFG_HDF_CHARS + 5];
};

/* Query one parsed config value from the firmware. *present is set to
 * 1 if the key was given in ZZ9000.CFG (for ZZ_CFG_KEY_LOADED: whether
 * the file was found at cold boot). */
UWORD zzcfg_query(ULONG board, UWORD key, UWORD *present);

/* Fetch the raw file contents into out (NUL-terminated, maxlen must be
 * >= 1). Returns a ZZ_CFG_FILE_* status; *outlen is the byte count.
 * ZZ_CFG_FILE_IDLE means the firmware never answered (no support). */
UWORD zzcfg_read_raw(ULONG board, char *out, UWORD maxlen, UWORD *outlen);

/* Scan raw config text for a `hdf = name` line (comments stripped,
 * case-insensitive key). Returns 1 and copies the name if found. */
int zzcfg_extract_hdf(const char *text, UWORD len, char *out, UWORD outsz);

/* Render a fresh, fully commented ZZ9000.CFG from v. Returns the byte
 * count (always < ZZCFG_MAX_SIZE for any input). */
UWORD zzcfg_generate(const struct zzcfg_values *v, char *out, UWORD outsz);

/* Generate and push the file to the SD card as ZZ9000.CFG via FWUP.
 * Returns an FWUP status (FWUP_OK on success). The previous file is
 * kept as ZZ9000.bak by the firmware. */
UWORD zzcfg_save(ULONG board, const struct zzcfg_values *v);

#endif /* ZZCFG_AMIGA_H */
