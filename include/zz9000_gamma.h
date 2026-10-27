/*
 * ZZ9000 output gamma helpers - portable and host-testable.
 *
 * Gamma is held in hundredths, 1.8 is 180, exactly like the firmware's
 * gamma.h. The accepted spellings mirror the firmware's parse_gamma
 * (zz_config.c): one optional fraction of at most two digits, so "1.8",
 * "1.80", "0.75" and "2" are valid while "1." and "1.805" are not.
 *
 * Shared by the ZZGamma CLI, the ZZ9000.CFG editor model
 * (common/zzcfg_amiga.c) and ZZTop's Display page slider.
 *
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ZZ9000_GAMMA_H
#define ZZ9000_GAMMA_H

#define ZZGAMMA_HUNDREDTHS_MIN      20   /* 0.20 */
#define ZZGAMMA_HUNDREDTHS_MAX     500   /* 5.00 */
#define ZZGAMMA_HUNDREDTHS_IDENTITY 100  /* 1.00, filter off */

/* Slider resolution: 0.05 per level, so levels 0..96 cover 0.20..5.00
 * and level 16 is exactly 1.00. */
#define ZZGAMMA_SLIDER_STEP       5
#define ZZGAMMA_SLIDER_MAX_LEVEL \
    ((ZZGAMMA_HUNDREDTHS_MAX - ZZGAMMA_HUNDREDTHS_MIN) / ZZGAMMA_SLIDER_STEP)

/* Readout buffer: "5.00" or "Off" plus the terminator. */
#define ZZGAMMA_TEXT_SIZE 8

/* Parse a numeric gamma value into hundredths (20..500). Returns 1 and
 * sets *out on success; 0 on garbage or out of range, *out untouched. */
static inline int zzgamma_parse_value(const char *s, int *out)
{
    long whole = 0, frac = 0, value;
    int digits = 0, seen_dot = 0;

    if (!s || !*s) return 0;
    while (*s) {
        if (*s == '.') {
            if (seen_dot) return 0;
            seen_dot = 1;
        } else if (*s >= '0' && *s <= '9') {
            if (seen_dot) {
                if (++digits > 2) return 0;
                frac = frac * 10 + (*s - '0');
            } else {
                whole = whole * 10 + (*s - '0');
                if (whole > ZZGAMMA_HUNDREDTHS_MAX / 100) return 0;
            }
        } else {
            return 0;
        }
        s++;
    }
    if (seen_dot && digits == 0) return 0;
    if (digits == 1) frac *= 10; /* "1.8" and "1.80" agree */
    value = whole * 100 + frac;
    if (value < ZZGAMMA_HUNDREDTHS_MIN || value > ZZGAMMA_HUNDREDTHS_MAX)
        return 0;
    *out = (int)value;
    return 1;
}

/* "OFF" in any letter case. */
static inline int zzgamma_is_off(const char *s)
{
    char c;
    int i;

    for (i = 0; i < 3; i++) {
        c = s[i];
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (c != "off"[i]) return 0;
    }
    return s[3] == '\0';
}

/* CLI argument: a gamma value, or OFF for the identity 100. */
static inline int zzgamma_parse(const char *s, int *out)
{
    if (s && zzgamma_is_off(s)) {
        *out = ZZGAMMA_HUNDREDTHS_IDENTITY;
        return 1;
    }
    return zzgamma_parse_value(s, out);
}

/* Nearest slider level for a gamma in hundredths; out-of-range values
 * clamp to the ends. Values between steps (a hand-written 1.83) round
 * to the nearest step. */
static inline int zzgamma_level_from_hundredths(unsigned hundredths)
{
    if (hundredths < ZZGAMMA_HUNDREDTHS_MIN)
        hundredths = ZZGAMMA_HUNDREDTHS_MIN;
    if (hundredths > ZZGAMMA_HUNDREDTHS_MAX)
        hundredths = ZZGAMMA_HUNDREDTHS_MAX;
    return (int)((hundredths - ZZGAMMA_HUNDREDTHS_MIN +
        ZZGAMMA_SLIDER_STEP / 2) / ZZGAMMA_SLIDER_STEP);
}

/* Gamma in hundredths for a slider level; levels clamp to 0..max. */
static inline unsigned zzgamma_hundredths_from_level(int level)
{
    if (level < 0) level = 0;
    if (level > ZZGAMMA_SLIDER_MAX_LEVEL) level = ZZGAMMA_SLIDER_MAX_LEVEL;
    return (unsigned)(ZZGAMMA_HUNDREDTHS_MIN + level * ZZGAMMA_SLIDER_STEP);
}

/* Readout text: "Off" for the identity, otherwise "1.80". out must hold
 * ZZGAMMA_TEXT_SIZE bytes; out-of-range values clamp like the slider. */
static inline void zzgamma_format(unsigned hundredths, char *out)
{
    if (hundredths == ZZGAMMA_HUNDREDTHS_IDENTITY) {
        out[0] = 'O'; out[1] = 'f'; out[2] = 'f'; out[3] = '\0';
        return;
    }
    if (hundredths < ZZGAMMA_HUNDREDTHS_MIN)
        hundredths = ZZGAMMA_HUNDREDTHS_MIN;
    if (hundredths > ZZGAMMA_HUNDREDTHS_MAX)
        hundredths = ZZGAMMA_HUNDREDTHS_MAX;
    out[0] = (char)('0' + hundredths / 100);
    out[1] = '.';
    out[2] = (char)('0' + (hundredths / 10) % 10);
    out[3] = (char)('0' + hundredths % 10);
    out[4] = '\0';
}

#endif /* ZZ9000_GAMMA_H */
