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
#include "zzcfg_query.h"   /* key ids, statuses, inline zzcfg_query() */

/* Matches the firmware's boot-time parse cap (ZZ_CONFIG_MAX_SIZE). */
#define ZZCFG_MAX_SIZE   4096
#define ZZCFG_MAC_CHARS  17          /* aa:bb:cc:dd:ee:ff */
#define ZZCFG_HDF_CHARS  63

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

/* Fetch the raw file contents into out (NUL-terminated, maxlen must be
 * >= 1). Returns a ZZ_CFG_FILE_* status; *outlen is the byte count.
 * ZZ_CFG_FILE_IDLE means the firmware never answered (no support). */
UWORD zzcfg_read_raw(ULONG board, char *out, UWORD maxlen, UWORD *outlen);

/* Is `name` a valid `hdf = ...` value? Mirrors the firmware's
 * hdf_name_valid rules (zz_config.c), which differ from the FWUP
 * destination-name rules: printable ASCII except '/', '\' and ':',
 * no leading '.', 1..ZZCFG_HDF_CHARS characters. Validate with this
 * before saving or the firmware will silently ignore the key at the
 * next cold boot. */
int zzcfg_hdf_name_valid(const char *name);

/* Decode the ZZTop-editable keys from raw config text into v, using
 * the firmware parser's line rules (comments, case-insensitive keys,
 * last value wins). Keys absent from the text leave v untouched, so
 * pre-fill v with the desired defaults. This is what makes the raw
 * SD file — not the firmware's boot-time parse — the editor's source
 * of truth: values saved or externally edited after boot survive a
 * Reload instead of being reverted to the cold-boot state. */
void zzcfg_parse_text(const char *text, UWORD len, struct zzcfg_values *v);

/* Render a fresh, fully commented ZZ9000.CFG from v. Returns the byte
 * count (always < ZZCFG_MAX_SIZE for any input). */
UWORD zzcfg_generate(const struct zzcfg_values *v, char *out, UWORD outsz);

/* Generate and push the file to the SD card as ZZ9000.CFG via FWUP.
 * Returns an FWUP status (FWUP_OK on success). The previous file is
 * kept as ZZ9000.bak by the firmware. */
UWORD zzcfg_save(ULONG board, const struct zzcfg_values *v);

#endif /* ZZCFG_AMIGA_H */
