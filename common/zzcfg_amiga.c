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
#define ZZCFG_POLL_LIMIT 2000000UL

static volatile UWORD *reg16(ULONG board, ULONG offset)
{
    return (volatile UWORD *)(board + offset);
}

UWORD zzcfg_read_raw(ULONG board, char *out, UWORD maxlen, UWORD *outlen)
{
    volatile UBYTE *buf = (volatile UBYTE *)(board + ZZ_BUFFER_OFFSET);
    unsigned long budget = ZZCFG_POLL_LIMIT;
    UWORD status, len, i;

    *outlen = 0;
    if (maxlen == 0) return ZZ_CFG_FILE_IO_ERROR;
    out[0] = '\0';

    /* Reset the handshake, then issue the read. The firmware processes
     * register writes in bus order, so no wait is needed in between. */
    *reg16(board, ZZ_REG_CONFIG_FILE) = 0;
    *reg16(board, ZZ_REG_CONFIG_FILE) = 1;

    do {
        status = *reg16(board, ZZ_REG_CONFIG_FILE);
        if (status != ZZ_CFG_FILE_IDLE) break;
    } while (--budget);

    if (status != ZZ_CFG_FILE_OK) return status;

    len = *reg16(board, ZZ_REG_CONFIG_FILE_LEN);
    if (len > maxlen - 1) len = maxlen - 1;

    for (i = 0; i < len; i++) {
        out[i] = (char)buf[i];
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

struct zzcfg_profile_desc {
    const char *name;
    UWORD pal_mode;
    UWORD full;
    UWORD vsync;
};

/* One schema drives parsing, rendering and legacy-firmware translation. */
static const struct zzcfg_profile_desc zzcfg_profiles[] = {
    { "full_60",             0, 1, 0 },
    { "full_exact",          0, 1, 1 },
    { "filtered_60",         0, 0, 0 },
    { "filtered_pal",        1, 0, 0 },
    { "filtered_pal_exact",  1, 0, 1 },
    { "filtered_ntsc_exact", 1, 0, 2 }
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
                v->videocap_profile = zzcfg_profile_from_legacy(legacy_pal,
                    legacy_full, legacy_vsync);
            } else if (zzcfg_str_eq_ci(value, "800x600")) {
                legacy_pal = 0;
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
        }
    }
}

UWORD zzcfg_generate(const struct zzcfg_values *v, char *out, UWORD outsz)
{
    static const char *sample_names[] = { "average", "even", "odd" };
    static const char *vsync_names[] = { "off", "pal", "ntsc" };
    UWORD sample = (v->videocap_sample <= 2) ? v->videocap_sample : 0;
    UWORD profile = (v->videocap_profile < ZZCFG_VCAP_PROFILE_COUNT) ?
        v->videocap_profile : ZZCFG_VCAP_FULL_60;
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
            "# Native video output (legacy firmware before ABI 2.9)\n"
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
