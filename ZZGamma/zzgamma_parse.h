/*
 * ZZGamma argument parsing - portable and host-testable.
 *
 * Gamma is held in hundredths, 1.8 is 180, exactly like the firmware's
 * gamma.h. The accepted spellings mirror the firmware's parse_gamma
 * (zz_config.c): one optional fraction of at most two digits, so "1.8",
 * "1.80", "0.75" and "2" are valid while "1." and "1.805" are not.
 *
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ZZGAMMA_PARSE_H
#define ZZGAMMA_PARSE_H

#define ZZGAMMA_HUNDREDTHS_MIN      20   /* 0.20 */
#define ZZGAMMA_HUNDREDTHS_MAX     500   /* 5.00 */
#define ZZGAMMA_HUNDREDTHS_IDENTITY 100  /* 1.00, filter off */

/* "OFF" in any letter case. */
static int zzgamma_is_off(const char *s)
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

/* Parse a gamma value into hundredths (20..500). OFF and 1.0 both map to
 * the identity 100. Returns 1 and sets *out on success; returns 0 on
 * garbage or an out-of-range value, leaving *out untouched. */
static int zzgamma_parse(const char *s, int *out)
{
    long whole = 0, frac = 0, value;
    int digits = 0, seen_dot = 0;

    if (!s || !*s) return 0;
    if (zzgamma_is_off(s)) {
        *out = ZZGAMMA_HUNDREDTHS_IDENTITY;
        return 1;
    }
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

#endif /* ZZGAMMA_PARSE_H */
