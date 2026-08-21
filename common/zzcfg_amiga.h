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
#include "zz9000_hw.h"
#include "zzcfg_query.h"   /* key ids, statuses, inline zzcfg_query() */

/* Matches the firmware's boot-time parse cap (ZZ_CONFIG_MAX_SIZE). */
#define ZZCFG_MAX_SIZE   4096
#define ZZCFG_MAC_CHARS  17          /* aa:bb:cc:dd:ee:ff */
#define ZZCFG_HDF_CHARS  63
#define ZZCFG_VIDEOCAP_CROP_H_COMPAT 188
#define ZZCFG_VIDEOCAP_CROP_V_COMPAT 26

enum zzcfg_videocap_profile {
    ZZCFG_VCAP_FULL_60 = 0,
    ZZCFG_VCAP_FULL_EXACT,
    ZZCFG_VCAP_FILTERED_60,
    ZZCFG_VCAP_FILTERED_PAL,
    ZZCFG_VCAP_FILTERED_PAL_EXACT,
    ZZCFG_VCAP_FILTERED_NTSC_EXACT,
    ZZCFG_VCAP_CENTERED_1080P_60,
    ZZCFG_VCAP_PROFILE_COUNT
};

/* Everything ZZTop's Settings window edits. mac/hdf are C strings;
 * an empty string means "not configured" and is emitted as a
 * commented-out example line. */
struct zzcfg_values {
    UWORD videocap_profile;  /* enum zzcfg_videocap_profile */
    UWORD videocap_sample;   /* 0 = average, 1 = even, 2 = odd */
    UWORD videocap_crop_h;   /* 0-4095, 28 MHz samples */
    UWORD videocap_crop_v;   /* 0-4095, captured lines */
    /* Presence is independent per axis. A missing key means Automatic;
     * an explicit value, including 0, 4095 or the old 188/26 defaults,
     * is a literal Custom override. */
    UWORD videocap_crop_h_present;
    UWORD videocap_crop_v_present;
    /* Firmware with the profile capability accepts the atomic key; older
     * firmware gets an equivalent legacy key trio. */
    UWORD use_videocap_profile_key;
    /* Capability snapshot used to hide/sanitize profiles that need a
     * matching FPGA+firmware path. */
    UWORD firmware_capabilities;
    UWORD scanline_mode;     /* 0-3 */
    UWORD scanline_parity;   /* 0-1 */
    UWORD int2;              /* 0-1 */
    /* Feature kill-switches. Both default ON in ZZ9000.card when the key is
     * absent, so these must default to 1 here too: ZZTop writes every
     * supported key on save, and defaulting them to 0 would silently
     * disable accelerated paths for anyone who opens Settings. */
    UWORD offscreen_bitmaps; /* 0-1, default 1 */
    UWORD video_overlay;     /* 0-1, default 1 */
    char  mac[ZZCFG_MAC_CHARS + 3];
    char  hdf[ZZCFG_HDF_CHARS + 5];

    /* Audio control-plane keys (firmware plan U5): audio_active,
     * audio_baseline and one eight-key group per scene slot. Values
     * are kept packed exactly as parsed (band pairs hi*128+lo, out
     * pref*128+vol, baseline paula*256+ax) -- the firmware is the
     * validator of record at cold boot. Presence is tracked per key
     * (audio_scene_mask bits match the firmware layout) so a Settings
     * save carries through exactly the audio keys the file had. */
#define ZZCFG_AUDIO_SCENES 8
    UWORD audio_active;
    UWORD audio_active_present;
    UWORD audio_baseline;
    UWORD audio_baseline_present;
    UWORD audio_scene_lpf[ZZCFG_AUDIO_SCENES];
    UWORD audio_scene_eq[ZZCFG_AUDIO_SCENES][5];
    UWORD audio_scene_out[ZZCFG_AUDIO_SCENES];
    UWORD audio_scene_pan[ZZCFG_AUDIO_SCENES];
    UWORD audio_scene_mask[ZZCFG_AUDIO_SCENES]; /* bit per key */
 };

/* Convert old three-key files and ENV overrides to/from the atomic profile. */
UWORD zzcfg_profile_from_legacy(UWORD pal_mode, UWORD full, UWORD vsync);
void zzcfg_profile_to_legacy(UWORD profile, UWORD *pal_mode, UWORD *full,
    UWORD *vsync);
int zzcfg_profile_supported(UWORD profile, UWORD firmware_capabilities);
UWORD zzcfg_profile_sanitize(UWORD profile, UWORD firmware_capabilities);

/* Fetch the raw file contents into out (NUL-terminated, maxlen must be
 * >= 1). Returns a ZZ_CFG_FILE_* status; *outlen is the byte count.
 * ZZ_CFG_FILE_IDLE means the firmware never answered (no support). */
UWORD zzcfg_read_raw(ULONG board, char *out, UWORD maxlen, UWORD *outlen);

/* Is `name` a valid `hdf = ...` value? Mirrors the firmware's
 * hdf_name_valid rules (zz_config.c): printable ASCII except '/',
 * '\' and ':', no leading '.', 1..ZZCFG_HDF_CHARS characters —
 * additionally rejecting '#' and ';', which the config parsers treat
 * as comment starts and would silently truncate the name at the next
 * cold-boot parse. Validate with this before saving; these rules
 * differ from the FWUP destination-name rules. */
int zzcfg_hdf_name_valid(const char *name);

/* Decode the ZZTop-editable keys from raw config text into v, using
 * the firmware parser's line rules (comments, case-insensitive keys,
 * last value wins). Keys absent from the text leave v untouched; valid
 * crop keys additionally set their axis's *_present flag. Pre-fill v
 * with the desired defaults and clear those flags before parsing a new
 * file. This is what makes the raw
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
