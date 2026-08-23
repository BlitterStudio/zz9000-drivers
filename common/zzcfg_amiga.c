/*
 * Amiga-side client for the ZZ9000.CFG SD-card config file.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <exec/types.h>
#include <stdio.h>
#include <string.h>

#include "zz9000_hw.h"
#include "fwup_client.h"
#include "fwup_amiga.h"
#include "zzcfg_amiga.h"

/* Matches the FWUP client's poll budget: a raw read is one small FatFs
 * read, but a slow card under load can still stall for tens of ms. */
#ifndef ZZCFG_POLL_LIMIT
#define ZZCFG_POLL_LIMIT 2000000UL
#endif

#ifdef ZZCFG_TEST_IO
extern UWORD zzcfg_test_reg_read(ULONG board, ULONG offset);
extern void zzcfg_test_reg_write(ULONG board, ULONG offset, UWORD value);
extern UBYTE zzcfg_test_buffer_read(ULONG board, UWORD offset);
#define ZZCFG_REG_READ(board, offset) \
    zzcfg_test_reg_read((board), (offset))
#define ZZCFG_REG_WRITE(board, offset, value) \
    zzcfg_test_reg_write((board), (offset), (value))
#define ZZCFG_BUFFER_READ(board, offset) \
    zzcfg_test_buffer_read((board), (offset))
#else
#define ZZCFG_REG_READ(board, offset) \
    (*(volatile UWORD *)((board) + (offset)))
#define ZZCFG_REG_WRITE(board, offset, value) \
    (*(volatile UWORD *)((board) + (offset)) = (value))
#define ZZCFG_BUFFER_READ(board, offset) \
    (*(volatile UBYTE *)((board) + ZZ_BUFFER_OFFSET + (offset)))
#endif

UWORD zzcfg_read_raw(ULONG board, char *out, UWORD maxlen, UWORD *outlen)
{
    unsigned long budget = ZZCFG_POLL_LIMIT;
    UWORD status, len, i;

    *outlen = 0;
    if (maxlen == 0) return ZZ_CFG_FILE_IO_ERROR;
    out[0] = '\0';

    /* A previous request may have left a terminal status (usually OK).
     * Wait until firmware has observed RESET and published IDLE before
     * sending READ; otherwise the first status load can accept the
     * previous request and copy its stale shared-buffer snapshot. */
    ZZCFG_REG_WRITE(board, ZZ_REG_CONFIG_FILE, 0);
    do {
        status = ZZCFG_REG_READ(board, ZZ_REG_CONFIG_FILE);
        if (status == ZZ_CFG_FILE_IDLE) break;
    } while (--budget);
    if (status != ZZ_CFG_FILE_IDLE)
        return ZZ_CFG_FILE_IDLE;

    ZZCFG_REG_WRITE(board, ZZ_REG_CONFIG_FILE, 1);
    budget = ZZCFG_POLL_LIMIT;
    do {
        status = ZZCFG_REG_READ(board, ZZ_REG_CONFIG_FILE);
        if (status != ZZ_CFG_FILE_IDLE) break;
    } while (--budget);
    if (status != ZZ_CFG_FILE_OK) return status;

    len = ZZCFG_REG_READ(board, ZZ_REG_CONFIG_FILE_LEN);
    if (len > maxlen - 1) len = maxlen - 1;

    for (i = 0; i < len; i++) {
        out[i] = (char)ZZCFG_BUFFER_READ(board, i);
    }
    out[len] = '\0';
    *outlen = len;
    return ZZ_CFG_FILE_OK;
}

static char zzcfg_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static int zzcfg_is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

int zzcfg_hdf_name_valid(const char *name)
{
    UWORD len = 0;

    if (!name || !*name || *name == '.') return 0;
    for (; *name; name++, len++) {
        char c = *name;
        if (c == '/' || c == '\\' || c == ':' || c < 0x21 || c > 0x7e)
            return 0;
        /* '#' and ';' start comments in the config parser, so a name
         * containing them would save fine and then silently truncate
         * at the next cold-boot parse (hdf = disk#1.hdf -> "disk"). */
        if (c == '#' || c == ';')
            return 0;
    }
    return len <= ZZCFG_HDF_CHARS;
}

static int zzcfg_str_eq_ci(const char *a, const char *b)
{
    while (*a && *b) {
        if (zzcfg_lower(*a) != zzcfg_lower(*b)) return 0;
        a++; b++;
    }
    return *a == *b;
}

static int zzcfg_parse_u12(const char *s, UWORD *out)
{
    ULONG value = 0;

    if (!*s) return 0;
    while (*s) {
        if (*s < '0' || *s > '9') return 0;
        value = value * 10 + (ULONG)(*s - '0');
        if (value > 4095) return 0;
        s++;
    }
    *out = (UWORD)value;
    return 1;
}

static int zzcfg_parse_u16(const char *s, UWORD *out)
{
    ULONG value = 0;

    if (!*s) return 0;
    while (*s) {
        if (*s < '0' || *s > '9') return 0;
        value = value * 10 + (ULONG)(*s - '0');
        if (value > 65535) return 0;
        s++;
    }
    *out = (UWORD)value;
    return 1;
}

/* One audio_scene<N>_<field> key (firmware U5 layout): fields are
 * 0 = lpf, 1..5 = eq01..eq89 pairs, 6 = out, 7 = pan and
 * 8..15 = nm1..nm8 name chunks. The editor round-trips the packed
 * decimal untouched; range validation is the firmware's at cold
 * boot. */
static int zzcfg_audio_scene_key(struct zzcfg_values *v, int scene,
    int field, const char *value)
{
    UWORD packed;

    if (scene < 0 || scene >= ZZCFG_AUDIO_SCENES) return 0;
    if (!zzcfg_parse_u16(value, &packed)) return 0;

    switch (field) {
    case 0: v->audio_scene_lpf[scene] = packed; break;
    case 1: case 2: case 3: case 4: case 5:
        v->audio_scene_eq[scene][field - 1] = packed; break;
    case 6: v->audio_scene_out[scene] = packed; break;
    case 7: v->audio_scene_pan[scene] = packed; break;
    case 8: case 9: case 10: case 11:
    case 12: case 13: case 14: case 15:
        v->audio_scene_nm[scene][field - 8] = packed; break;
    default: return 0;
    }
    v->audio_scene_mask[scene] |= (UWORD)(1u << field);
    return 1;
}

struct zzcfg_profile_desc {
    const char *name;
    UWORD pal_mode;
    UWORD full;
    UWORD vsync;
    UWORD required_capability;
    UWORD fallback_profile;
};

/* One schema drives parsing, rendering and legacy-firmware translation. */
static const struct zzcfg_profile_desc zzcfg_profiles[] = {
    { "full_60",             0, 1, 0, 0, ZZCFG_VCAP_FULL_60 },
    { "full_exact",          0, 1, 1, 0, ZZCFG_VCAP_FULL_EXACT },
    { "filtered_60",         0, 0, 0, 0, ZZCFG_VCAP_FILTERED_60 },
    { "filtered_pal",        1, 0, 0, 0, ZZCFG_VCAP_FILTERED_PAL },
    { "filtered_pal_exact",  1, 0, 1, 0, ZZCFG_VCAP_FILTERED_PAL_EXACT },
    { "filtered_ntsc_exact", 1, 0, 2, 0, ZZCFG_VCAP_FILTERED_NTSC_EXACT },
    { "centered_1080p_60",   0, 1, 0,
      ZZ_FW_CAP_VIDEOCAP_CENTERED_1080P, ZZCFG_VCAP_FULL_60 }
};

typedef char zzcfg_profile_count_matches_enum[
    sizeof(zzcfg_profiles) / sizeof(zzcfg_profiles[0]) ==
        ZZCFG_VCAP_PROFILE_COUNT ? 1 : -1];

/* Mirrors the firmware parser's line rules: `key = value`, `#`/`;`
 * comments, case-insensitive keys and keyword values, last value
 * wins. Only the keys ZZTop edits are decoded; anything else is
 * skipped (the firmware is the validator of record at cold boot). */
UWORD zzcfg_profile_from_legacy(UWORD pal_mode, UWORD full, UWORD vsync)
{
    UWORD i;

    for (i = 0; i < ZZCFG_VCAP_PROFILE_COUNT; i++) {
        const struct zzcfg_profile_desc *p = &zzcfg_profiles[i];
        if (p->full != (full != 0)) continue;
        if (full) {
            if ((p->vsync != 0) == (vsync != 0)) return i;
        } else if (p->pal_mode == (pal_mode != 0) && p->vsync == vsync) {
            return i;
        }
    }
    return ZZCFG_VCAP_FILTERED_60;
}

void zzcfg_profile_to_legacy(UWORD profile, UWORD *pal_mode, UWORD *full,
    UWORD *vsync)
{
    const struct zzcfg_profile_desc *p;

    if (profile >= ZZCFG_VCAP_PROFILE_COUNT)
        profile = ZZCFG_VCAP_FILTERED_60;
    p = &zzcfg_profiles[profile];
    *pal_mode = p->pal_mode;
    *full = p->full;
    *vsync = p->vsync;
}

int zzcfg_profile_supported(UWORD profile, UWORD firmware_capabilities)
{
    const struct zzcfg_profile_desc *p;

    if (profile >= ZZCFG_VCAP_PROFILE_COUNT) return 0;
    p = &zzcfg_profiles[profile];
    return p->required_capability == 0 ||
        (firmware_capabilities & p->required_capability) ==
            p->required_capability;
}

UWORD zzcfg_profile_sanitize(UWORD profile, UWORD firmware_capabilities)
{
    if (profile >= ZZCFG_VCAP_PROFILE_COUNT) return ZZCFG_VCAP_FULL_60;
    if (!zzcfg_profile_supported(profile, firmware_capabilities))
        return zzcfg_profiles[profile].fallback_profile;
    return profile;
}

static int zzcfg_parse_profile(const char *value, UWORD *profile)
{
    UWORD i;

    for (i = 0; i < ZZCFG_VCAP_PROFILE_COUNT; i++) {
        if (zzcfg_str_eq_ci(value, zzcfg_profiles[i].name)) {
            *profile = i;
            return 1;
        }
    }
    return 0;
}

void zzcfg_parse_text(const char *text, UWORD len, struct zzcfg_values *v)
{
    UWORD pos = 0;
    UWORD legacy_pal, legacy_full, legacy_vsync;

    zzcfg_profile_to_legacy(v->videocap_profile, &legacy_pal, &legacy_full,
        &legacy_vsync);

    while (pos < len) {
        char key[24];
        char value[ZZCFG_HDF_CHARS + 2];
        UWORD line_end = pos;
        UWORD n;

        while (line_end < len && text[line_end] != '\n') line_end++;

        /* trim leading whitespace, copy the key up to '=' or space */
        while (pos < line_end && zzcfg_is_space(text[pos])) pos++;
        n = 0;
        while (pos < line_end && n < sizeof(key) - 1 &&
                text[pos] != '=' && text[pos] != '#' && text[pos] != ';' &&
                !zzcfg_is_space(text[pos])) {
            key[n++] = text[pos];
            pos++;
        }
        key[n] = '\0';

        while (pos < line_end && zzcfg_is_space(text[pos])) pos++;
        if (n == 0 || pos >= line_end || text[pos] != '=') {
            pos = line_end + 1;
            continue;
        }
        pos++; /* skip '=' */
        while (pos < line_end && zzcfg_is_space(text[pos])) pos++;

        n = 0;
        while (pos < line_end && n < sizeof(value) - 1 &&
                text[pos] != '#' && text[pos] != ';' &&
                !zzcfg_is_space(text[pos])) {
            value[n++] = text[pos];
            pos++;
        }
        value[n] = '\0';
        pos = line_end + 1;
        if (n == 0) continue;

        if (zzcfg_str_eq_ci(key, "videocap_profile")) {
            UWORD profile;
            if (zzcfg_parse_profile(value, &profile)) {
                v->videocap_profile = profile;
                zzcfg_profile_to_legacy(profile, &legacy_pal, &legacy_full,
                    &legacy_vsync);
            }
        } else if (zzcfg_str_eq_ci(key, "videocap_mode")) {
            if (zzcfg_str_eq_ci(value, "pal") ||
                    zzcfg_str_eq_ci(value, "720x576")) {
                legacy_pal = 1;
                legacy_full = 0;
                v->videocap_profile = zzcfg_profile_from_legacy(legacy_pal,
                    legacy_full, legacy_vsync);
            } else if (zzcfg_str_eq_ci(value, "800x600")) {
                legacy_pal = 0;
                legacy_full = 0;
                v->videocap_profile = zzcfg_profile_from_legacy(legacy_pal,
                    legacy_full, legacy_vsync);
            }
        } else if (zzcfg_str_eq_ci(key, "videocap_sample")) {
            if (zzcfg_str_eq_ci(value, "average"))
                v->videocap_sample = 0;
            else if (zzcfg_str_eq_ci(value, "even"))
                v->videocap_sample = 1;
            else if (zzcfg_str_eq_ci(value, "odd"))
                v->videocap_sample = 2;
        } else if (zzcfg_str_eq_ci(key, "videocap_shres")) {
            if (zzcfg_str_eq_ci(value, "filter")) {
                legacy_full = 0;
                v->videocap_profile = zzcfg_profile_from_legacy(legacy_pal,
                    legacy_full, legacy_vsync);
            } else if (zzcfg_str_eq_ci(value, "full")) {
                legacy_full = 1;
                v->videocap_profile = zzcfg_profile_from_legacy(legacy_pal,
                    legacy_full, legacy_vsync);
            }
        } else if (zzcfg_str_eq_ci(key, "videocap_crop_h")) {
            UWORD crop;
            if (zzcfg_parse_u12(value, &crop)) {
                v->videocap_crop_h = crop;
                v->videocap_crop_h_present = 1;
            }
        } else if (zzcfg_str_eq_ci(key, "videocap_crop_v")) {
            UWORD crop;
            if (zzcfg_parse_u12(value, &crop)) {
                v->videocap_crop_v = crop;
                v->videocap_crop_v_present = 1;
            }
        } else if (zzcfg_str_eq_ci(key, "nonstandard_vsync")) {
            if (zzcfg_str_eq_ci(value, "off")) legacy_vsync = 0;
            else if (zzcfg_str_eq_ci(value, "pal") ||
                     zzcfg_str_eq_ci(value, "on")) legacy_vsync = 1;
            else if (zzcfg_str_eq_ci(value, "ntsc")) legacy_vsync = 2;
            else continue;
            v->videocap_profile = zzcfg_profile_from_legacy(legacy_pal,
                legacy_full, legacy_vsync);
        } else if (zzcfg_str_eq_ci(key, "scanline_mode")) {
            if (value[1] == '\0' && value[0] >= '0' && value[0] <= '3')
                v->scanline_mode = (UWORD)(value[0] - '0');
        } else if (zzcfg_str_eq_ci(key, "scanline_parity")) {
            if (value[1] == '\0' && (value[0] == '0' || value[0] == '1'))
                v->scanline_parity = (UWORD)(value[0] - '0');
        } else if (zzcfg_str_eq_ci(key, "int2")) {
            if (zzcfg_str_eq_ci(value, "on") || zzcfg_str_eq_ci(value, "1"))
                v->int2 = 1;
            else if (zzcfg_str_eq_ci(value, "off") ||
                     zzcfg_str_eq_ci(value, "0"))
                v->int2 = 0;
        } else if (zzcfg_str_eq_ci(key, "offscreen_bitmaps")) {
            if (zzcfg_str_eq_ci(value, "on") || zzcfg_str_eq_ci(value, "1"))
                v->offscreen_bitmaps = 1;
            else if (zzcfg_str_eq_ci(value, "off") ||
                     zzcfg_str_eq_ci(value, "0"))
                v->offscreen_bitmaps = 0;
        } else if (zzcfg_str_eq_ci(key, "video_overlay")) {
            if (zzcfg_str_eq_ci(value, "on") || zzcfg_str_eq_ci(value, "1"))
                v->video_overlay = 1;
            else if (zzcfg_str_eq_ci(value, "off") ||
                     zzcfg_str_eq_ci(value, "0"))
                v->video_overlay = 0;
        } else if (zzcfg_str_eq_ci(key, "mac")) {
            if (strlen(value) < sizeof(v->mac))
                strcpy(v->mac, value);
        } else if (zzcfg_str_eq_ci(key, "hdf")) {
            if (strlen(value) < sizeof(v->hdf))
                strcpy(v->hdf, value);
        } else if (zzcfg_str_eq_ci(key, "audio_active")) {
            UWORD active;
            if (zzcfg_parse_u16(value, &active)) {
                v->audio_active = active;
                v->audio_active_present = 1;
            }
        } else if (zzcfg_str_eq_ci(key, "audio_baseline")) {
            UWORD baseline;
            if (zzcfg_parse_u16(value, &baseline)) {
                v->audio_baseline = baseline;
                v->audio_baseline_present = 1;
            }
        } else if (zzcfg_str_eq_ci(key, "audio_scene0_lpf")) {
            zzcfg_audio_scene_key(v, 0, 0, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene0_eq01")) {
            zzcfg_audio_scene_key(v, 0, 1, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene0_eq23")) {
            zzcfg_audio_scene_key(v, 0, 2, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene0_eq45")) {
            zzcfg_audio_scene_key(v, 0, 3, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene0_eq67")) {
            zzcfg_audio_scene_key(v, 0, 4, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene0_eq89")) {
            zzcfg_audio_scene_key(v, 0, 5, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene0_out")) {
            zzcfg_audio_scene_key(v, 0, 6, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene0_pan")) {
            zzcfg_audio_scene_key(v, 0, 7, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene0_nm1")) {
            zzcfg_audio_scene_key(v, 0, 8, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene0_nm2")) {
            zzcfg_audio_scene_key(v, 0, 9, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene0_nm3")) {
            zzcfg_audio_scene_key(v, 0, 10, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene0_nm4")) {
            zzcfg_audio_scene_key(v, 0, 11, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene0_nm5")) {
            zzcfg_audio_scene_key(v, 0, 12, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene0_nm6")) {
            zzcfg_audio_scene_key(v, 0, 13, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene0_nm7")) {
            zzcfg_audio_scene_key(v, 0, 14, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene0_nm8")) {
            zzcfg_audio_scene_key(v, 0, 15, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene1_lpf")) {
            zzcfg_audio_scene_key(v, 1, 0, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene1_eq01")) {
            zzcfg_audio_scene_key(v, 1, 1, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene1_eq23")) {
            zzcfg_audio_scene_key(v, 1, 2, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene1_eq45")) {
            zzcfg_audio_scene_key(v, 1, 3, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene1_eq67")) {
            zzcfg_audio_scene_key(v, 1, 4, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene1_eq89")) {
            zzcfg_audio_scene_key(v, 1, 5, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene1_out")) {
            zzcfg_audio_scene_key(v, 1, 6, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene1_pan")) {
            zzcfg_audio_scene_key(v, 1, 7, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene1_nm1")) {
            zzcfg_audio_scene_key(v, 1, 8, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene1_nm2")) {
            zzcfg_audio_scene_key(v, 1, 9, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene1_nm3")) {
            zzcfg_audio_scene_key(v, 1, 10, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene1_nm4")) {
            zzcfg_audio_scene_key(v, 1, 11, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene1_nm5")) {
            zzcfg_audio_scene_key(v, 1, 12, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene1_nm6")) {
            zzcfg_audio_scene_key(v, 1, 13, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene1_nm7")) {
            zzcfg_audio_scene_key(v, 1, 14, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene1_nm8")) {
            zzcfg_audio_scene_key(v, 1, 15, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene2_lpf")) {
            zzcfg_audio_scene_key(v, 2, 0, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene2_eq01")) {
            zzcfg_audio_scene_key(v, 2, 1, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene2_eq23")) {
            zzcfg_audio_scene_key(v, 2, 2, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene2_eq45")) {
            zzcfg_audio_scene_key(v, 2, 3, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene2_eq67")) {
            zzcfg_audio_scene_key(v, 2, 4, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene2_eq89")) {
            zzcfg_audio_scene_key(v, 2, 5, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene2_out")) {
            zzcfg_audio_scene_key(v, 2, 6, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene2_pan")) {
            zzcfg_audio_scene_key(v, 2, 7, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene2_nm1")) {
            zzcfg_audio_scene_key(v, 2, 8, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene2_nm2")) {
            zzcfg_audio_scene_key(v, 2, 9, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene2_nm3")) {
            zzcfg_audio_scene_key(v, 2, 10, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene2_nm4")) {
            zzcfg_audio_scene_key(v, 2, 11, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene2_nm5")) {
            zzcfg_audio_scene_key(v, 2, 12, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene2_nm6")) {
            zzcfg_audio_scene_key(v, 2, 13, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene2_nm7")) {
            zzcfg_audio_scene_key(v, 2, 14, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene2_nm8")) {
            zzcfg_audio_scene_key(v, 2, 15, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene3_lpf")) {
            zzcfg_audio_scene_key(v, 3, 0, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene3_eq01")) {
            zzcfg_audio_scene_key(v, 3, 1, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene3_eq23")) {
            zzcfg_audio_scene_key(v, 3, 2, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene3_eq45")) {
            zzcfg_audio_scene_key(v, 3, 3, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene3_eq67")) {
            zzcfg_audio_scene_key(v, 3, 4, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene3_eq89")) {
            zzcfg_audio_scene_key(v, 3, 5, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene3_out")) {
            zzcfg_audio_scene_key(v, 3, 6, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene3_pan")) {
            zzcfg_audio_scene_key(v, 3, 7, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene3_nm1")) {
            zzcfg_audio_scene_key(v, 3, 8, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene3_nm2")) {
            zzcfg_audio_scene_key(v, 3, 9, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene3_nm3")) {
            zzcfg_audio_scene_key(v, 3, 10, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene3_nm4")) {
            zzcfg_audio_scene_key(v, 3, 11, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene3_nm5")) {
            zzcfg_audio_scene_key(v, 3, 12, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene3_nm6")) {
            zzcfg_audio_scene_key(v, 3, 13, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene3_nm7")) {
            zzcfg_audio_scene_key(v, 3, 14, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene3_nm8")) {
            zzcfg_audio_scene_key(v, 3, 15, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene4_lpf")) {
            zzcfg_audio_scene_key(v, 4, 0, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene4_eq01")) {
            zzcfg_audio_scene_key(v, 4, 1, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene4_eq23")) {
            zzcfg_audio_scene_key(v, 4, 2, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene4_eq45")) {
            zzcfg_audio_scene_key(v, 4, 3, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene4_eq67")) {
            zzcfg_audio_scene_key(v, 4, 4, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene4_eq89")) {
            zzcfg_audio_scene_key(v, 4, 5, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene4_out")) {
            zzcfg_audio_scene_key(v, 4, 6, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene4_pan")) {
            zzcfg_audio_scene_key(v, 4, 7, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene4_nm1")) {
            zzcfg_audio_scene_key(v, 4, 8, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene4_nm2")) {
            zzcfg_audio_scene_key(v, 4, 9, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene4_nm3")) {
            zzcfg_audio_scene_key(v, 4, 10, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene4_nm4")) {
            zzcfg_audio_scene_key(v, 4, 11, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene4_nm5")) {
            zzcfg_audio_scene_key(v, 4, 12, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene4_nm6")) {
            zzcfg_audio_scene_key(v, 4, 13, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene4_nm7")) {
            zzcfg_audio_scene_key(v, 4, 14, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene4_nm8")) {
            zzcfg_audio_scene_key(v, 4, 15, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene5_lpf")) {
            zzcfg_audio_scene_key(v, 5, 0, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene5_eq01")) {
            zzcfg_audio_scene_key(v, 5, 1, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene5_eq23")) {
            zzcfg_audio_scene_key(v, 5, 2, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene5_eq45")) {
            zzcfg_audio_scene_key(v, 5, 3, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene5_eq67")) {
            zzcfg_audio_scene_key(v, 5, 4, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene5_eq89")) {
            zzcfg_audio_scene_key(v, 5, 5, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene5_out")) {
            zzcfg_audio_scene_key(v, 5, 6, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene5_pan")) {
            zzcfg_audio_scene_key(v, 5, 7, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene5_nm1")) {
            zzcfg_audio_scene_key(v, 5, 8, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene5_nm2")) {
            zzcfg_audio_scene_key(v, 5, 9, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene5_nm3")) {
            zzcfg_audio_scene_key(v, 5, 10, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene5_nm4")) {
            zzcfg_audio_scene_key(v, 5, 11, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene5_nm5")) {
            zzcfg_audio_scene_key(v, 5, 12, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene5_nm6")) {
            zzcfg_audio_scene_key(v, 5, 13, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene5_nm7")) {
            zzcfg_audio_scene_key(v, 5, 14, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene5_nm8")) {
            zzcfg_audio_scene_key(v, 5, 15, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene6_lpf")) {
            zzcfg_audio_scene_key(v, 6, 0, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene6_eq01")) {
            zzcfg_audio_scene_key(v, 6, 1, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene6_eq23")) {
            zzcfg_audio_scene_key(v, 6, 2, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene6_eq45")) {
            zzcfg_audio_scene_key(v, 6, 3, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene6_eq67")) {
            zzcfg_audio_scene_key(v, 6, 4, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene6_eq89")) {
            zzcfg_audio_scene_key(v, 6, 5, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene6_out")) {
            zzcfg_audio_scene_key(v, 6, 6, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene6_pan")) {
            zzcfg_audio_scene_key(v, 6, 7, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene6_nm1")) {
            zzcfg_audio_scene_key(v, 6, 8, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene6_nm2")) {
            zzcfg_audio_scene_key(v, 6, 9, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene6_nm3")) {
            zzcfg_audio_scene_key(v, 6, 10, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene6_nm4")) {
            zzcfg_audio_scene_key(v, 6, 11, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene6_nm5")) {
            zzcfg_audio_scene_key(v, 6, 12, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene6_nm6")) {
            zzcfg_audio_scene_key(v, 6, 13, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene6_nm7")) {
            zzcfg_audio_scene_key(v, 6, 14, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene6_nm8")) {
            zzcfg_audio_scene_key(v, 6, 15, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene7_lpf")) {
            zzcfg_audio_scene_key(v, 7, 0, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene7_eq01")) {
            zzcfg_audio_scene_key(v, 7, 1, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene7_eq23")) {
            zzcfg_audio_scene_key(v, 7, 2, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene7_eq45")) {
            zzcfg_audio_scene_key(v, 7, 3, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene7_eq67")) {
            zzcfg_audio_scene_key(v, 7, 4, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene7_eq89")) {
            zzcfg_audio_scene_key(v, 7, 5, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene7_out")) {
            zzcfg_audio_scene_key(v, 7, 6, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene7_pan")) {
            zzcfg_audio_scene_key(v, 7, 7, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene7_nm1")) {
            zzcfg_audio_scene_key(v, 7, 8, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene7_nm2")) {
            zzcfg_audio_scene_key(v, 7, 9, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene7_nm3")) {
            zzcfg_audio_scene_key(v, 7, 10, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene7_nm4")) {
            zzcfg_audio_scene_key(v, 7, 11, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene7_nm5")) {
            zzcfg_audio_scene_key(v, 7, 12, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene7_nm6")) {
            zzcfg_audio_scene_key(v, 7, 13, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene7_nm7")) {
            zzcfg_audio_scene_key(v, 7, 14, value);
        } else if (zzcfg_str_eq_ci(key, "audio_scene7_nm8")) {
            zzcfg_audio_scene_key(v, 7, 15, value);
        }
    }
}

static int zzcfg_audio_present(const struct zzcfg_values *v)
{
    int i;

    if (v->audio_active_present || v->audio_baseline_present)
        return 1;
    for (i = 0; i < ZZCFG_AUDIO_SCENES; i++)
        if (v->audio_scene_mask[i])
            return 1;
    return 0;
}

static int zzcfg_append_audio(const struct zzcfg_values *v, char *out,
    UWORD outsz, int off)
{
    int i, n;

    if (off < 0 || (UWORD)off >= outsz) return -1;
    n = snprintf(out + off, outsz - (UWORD)off,
        "\n"
        "# Audio control plane: active scene and operator baseline\n"
        "%saudio_active = %u\n"
        "%saudio_baseline = %u\n",
        v->audio_active_present ? "" : "#", (unsigned)v->audio_active,
        v->audio_baseline_present ? "" : "#",
        (unsigned)v->audio_baseline);
    if (n < 0 || (UWORD)(off + n) >= outsz) return -1;
    off += n;

    for (i = 0; i < ZZCFG_AUDIO_SCENES; i++) {
        /* Mirror the parse side (zzcfg_audio_scene_key): a key is
         * emitted only when its mask bit is set, so a hand-edited
         * partial scene never grows zero-filled siblings on save. */
        const struct { const char *name; unsigned value; } fields[] = {
            { "lpf",  (unsigned)v->audio_scene_lpf[i] },
            { "eq01", (unsigned)v->audio_scene_eq[i][0] },
            { "eq23", (unsigned)v->audio_scene_eq[i][1] },
            { "eq45", (unsigned)v->audio_scene_eq[i][2] },
            { "eq67", (unsigned)v->audio_scene_eq[i][3] },
            { "eq89", (unsigned)v->audio_scene_eq[i][4] },
            { "out",  (unsigned)v->audio_scene_out[i] },
            { "pan",  (unsigned)v->audio_scene_pan[i] },
            { "nm1",  (unsigned)v->audio_scene_nm[i][0] },
            { "nm2",  (unsigned)v->audio_scene_nm[i][1] },
            { "nm3",  (unsigned)v->audio_scene_nm[i][2] },
            { "nm4",  (unsigned)v->audio_scene_nm[i][3] },
            { "nm5",  (unsigned)v->audio_scene_nm[i][4] },
            { "nm6",  (unsigned)v->audio_scene_nm[i][5] },
            { "nm7",  (unsigned)v->audio_scene_nm[i][6] },
            { "nm8",  (unsigned)v->audio_scene_nm[i][7] }
        };
        int f;

        for (f = 0; f < 16; f++) {
            if (!(v->audio_scene_mask[i] & (UWORD)(1u << f))) continue;
            n = snprintf(out + off, outsz - (UWORD)off,
                "audio_scene%d_%s = %u\n",
                i, fields[f].name, fields[f].value);
            if (n < 0 || (UWORD)(off + n) >= outsz) return -1;
            off += n;
        }
    }
    return off;
}

UWORD zzcfg_generate(const struct zzcfg_values *v, char *out, UWORD outsz)
{
    static const char *sample_names[] = { "average", "even", "odd" };
    static const char *vsync_names[] = { "off", "pal", "ntsc" };
    UWORD sample = (v->videocap_sample <= 2) ? v->videocap_sample : 0;
    UWORD profile = zzcfg_profile_sanitize(v->videocap_profile,
        v->firmware_capabilities);
    UWORD legacy_pal, legacy_full, legacy_vsync;
    char video_config[512];
    int n;

    zzcfg_profile_to_legacy(profile, &legacy_pal, &legacy_full, &legacy_vsync);
    if (v->use_videocap_profile_key) {
        snprintf(video_config, sizeof(video_config),
            "# Native video output profile (resolution, detail and refresh)\n"
            "videocap_profile = %s\n", zzcfg_profiles[profile].name);
    } else {
        snprintf(video_config, sizeof(video_config),
            "# Native video output (legacy firmware without profile capability)\n"
            "videocap_mode = %s\n"
            "videocap_shres = %s\n"
            "nonstandard_vsync = %s\n",
            legacy_pal ? "pal" : "800x600",
            legacy_full ? "full" : "filter",
            vsync_names[legacy_vsync]);
    }

    n = snprintf(out, outsz,
        "# ZZ9000.CFG - ZZ9000 firmware configuration file\n"
        "# Written by ZZTop. Read once at cold boot (power-on);\n"
        "# soft resets do not re-read it. See the firmware manual\n"
        "# for all options: https://github.com/BlitterStudio/zz9000-firmware\n"
        "\n"
        "%s\n"
        "# Native capture sampling: average (default), even or odd\n"
        "videocap_sample = %s\n"
        "# Capture framing: commented axes use the automatic board/profile baseline\n"
        "%svideocap_crop_h = %u\n"
        "%svideocap_crop_v = %u\n"
        "\n"
        "# Scanlines: 0=off 1=classic 2=soft 3=gradient; parity 0/1\n"
        "scanline_mode = %u\n"
        "scanline_parity = %u\n"
        "\n"
        "# on = drivers use INT2 instead of INT6 (replaces ENV:ZZ9K_INT2)\n"
        "int2 = %s\n"
        "\n"
        "# Accelerated P96 off-screen bitmaps (replaces ENV:ZZ9000-NO-OFFSCREEN)\n"
        "offscreen_bitmaps = %s\n"
        "\n"
        "# P96 video window / picture-in-picture (replaces ENV:ZZ9000-NO-PIP)\n"
        "video_overlay = %s\n"
        "\n"
        "# Ethernet MAC override (replaces ENV:ZZ9K_MAC)\n"
        "%smac = %s\n"
        "\n"
        "# SD-card boot HDF image in the root of the card (default zz9000.hdf)\n"
        "%shdf = %s\n",
        video_config,
        sample_names[sample],
        v->videocap_crop_h_present ? "" : "#",
        (unsigned)(v->videocap_crop_h & 4095),
        v->videocap_crop_v_present ? "" : "#",
        (unsigned)(v->videocap_crop_v & 4095),
        (unsigned)(v->scanline_mode & 3),
        (unsigned)(v->scanline_parity & 1),
        v->int2 ? "on" : "off",
        v->offscreen_bitmaps ? "on" : "off",
        v->video_overlay ? "on" : "off",
        v->mac[0] ? "" : "#", v->mac[0] ? v->mac : "68:82:F2:00:00:01",
        v->hdf[0] ? "" : "#", v->hdf[0] ? v->hdf : "zz9000.hdf");

    if (n < 0) return 0;
    if ((UWORD)n >= outsz) return (UWORD)(outsz - 1);

    /* Audio control-plane keys (firmware U5): carried through exactly
     * when the parsed file had them, so a Settings save never deletes
     * an operator's saved scenes. */
    if (zzcfg_audio_present(v)) {
        int off = zzcfg_append_audio(v, out, outsz, n);
        if (off < 0) return (UWORD)(outsz - 1);
        n = off;
    }
    return (UWORD)n;
}

UWORD zzcfg_save(ULONG board, const struct zzcfg_values *v)
{
    static char text[ZZCFG_MAX_SIZE];
    struct fwup_io io;
    UWORD len, st;

    len = zzcfg_generate(v, text, sizeof(text));
    if (len == 0) return FWUP_ERR_UNKNOWN;

    fwup_io_init_board(&io, board);

    st = (UWORD)fwup_open(&io, "ZZ9000.CFG");
    if (st != FWUP_OK) return st;

    st = (UWORD)fwup_write_chunk(&io, text, len);
    if (st != FWUP_OK) {
        fwup_abort(&io);
        return st;
    }

    st = (UWORD)fwup_close(&io);
    if (st != FWUP_OK) fwup_abort(&io);
    return st;
}
