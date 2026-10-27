/*
 * Host tests for the shared output-gamma helpers (include/zz9000_gamma.h):
 * the ZZGamma argument parser, ZZTop's slider level mapping and the
 * readout text. The parser rules are pinned to the firmware's
 * parse_gamma.
 *
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <string.h>

#include "zz9000_gamma.h"

static int failures;

static void check(int cond, const char *what)
{
    if (!cond) { printf("FAIL: %s\n", what); failures++; }
}

static void check_parse(const char *in, int expect_ok, int expect_value)
{
    int value = -1;
    char what[80];

    if (expect_ok) {
        snprintf(what, sizeof(what), "'%s' parses to %d", in, expect_value);
        check(zzgamma_parse(in, &value) && value == expect_value, what);
    } else {
        snprintf(what, sizeof(what), "'%s' is rejected", in);
        check(!zzgamma_parse(in, &value), what);
    }
}

static void test_parse(void)
{
    int value = -1;

    /* decimals: one or two fraction digits, plain integers */
    check_parse("1.8", 1, 180);
    check_parse("1.80", 1, 180);
    check_parse("0.75", 1, 75);
    check_parse("0.2", 1, 20);
    check_parse("2", 1, 200);
    check_parse("1.0", 1, 100);
    check_parse("5.00", 1, 500);
    check_parse(".5", 1, 50);

    /* OFF spellings map to the identity */
    check_parse("OFF", 1, 100);
    check_parse("off", 1, 100);
    check_parse("Off", 1, 100);

    /* range 0.20..5.00 */
    check_parse("0.20", 1, 20);
    check_parse("0.19", 0, 0);
    check_parse("0.1", 0, 0);
    check_parse("0", 0, 0);
    check_parse("5.01", 0, 0);
    check_parse("6", 0, 0);
    check_parse("50", 0, 0);

    /* garbage and malformed numbers */
    check_parse("", 0, 0);
    check_parse("1.", 0, 0);
    check_parse("1.805", 0, 0);
    check_parse("1..8", 0, 0);
    check_parse("-1.8", 0, 0);
    check_parse("+1.8", 0, 0);
    check_parse("1,8", 0, 0);
    check_parse("1.8x", 0, 0);
    check_parse("0x20", 0, 0);
    check_parse("high", 0, 0);
    check_parse("o", 0, 0);
    check_parse("offx", 0, 0);
    check_parse("of", 0, 0);

    /* The config-file form takes numbers only: firmware rejects "off". */
    check(!zzgamma_parse_value("off", &value), "config value rejects OFF");
    check(zzgamma_parse_value("1.25", &value) && value == 125,
        "config value parses 1.25");
}

static void test_slider_mapping(void)
{
    int level;
    unsigned h;

    check(ZZGAMMA_SLIDER_MAX_LEVEL == 96, "96 steps of 0.05 span 0.20..5.00");
    check(zzgamma_hundredths_from_level(0) == 20, "level 0 is 0.20");
    check(zzgamma_hundredths_from_level(16) == 100, "level 16 is 1.00");
    check(zzgamma_hundredths_from_level(32) == 180, "level 32 is 1.80");
    check(zzgamma_hundredths_from_level(96) == 500, "level 96 is 5.00");
    check(zzgamma_hundredths_from_level(-3) == 20, "negative level clamps");
    check(zzgamma_hundredths_from_level(200) == 500, "high level clamps");

    check(zzgamma_level_from_hundredths(100) == 16, "1.00 is level 16");
    check(zzgamma_level_from_hundredths(180) == 32, "1.80 is level 32");
    check(zzgamma_level_from_hundredths(182) == 32, "1.82 rounds down");
    check(zzgamma_level_from_hundredths(183) == 33, "1.83 rounds up");
    check(zzgamma_level_from_hundredths(0) == 0, "0 (unsupported) clamps low");
    check(zzgamma_level_from_hundredths(900) == 96, "above 5.00 clamps high");

    /* Every level round-trips, so a slider position never drifts. */
    for (level = 0; level <= ZZGAMMA_SLIDER_MAX_LEVEL; level++) {
        h = zzgamma_hundredths_from_level(level);
        if (zzgamma_level_from_hundredths(h) != level) {
            check(0, "level round trip");
            break;
        }
    }
}

static void test_format(void)
{
    char text[ZZGAMMA_TEXT_SIZE];

    zzgamma_format(100, text);
    check(strcmp(text, "Off") == 0, "1.00 reads Off");
    zzgamma_format(180, text);
    check(strcmp(text, "1.80") == 0, "1.80 readout");
    zzgamma_format(75, text);
    check(strcmp(text, "0.75") == 0, "0.75 readout");
    zzgamma_format(20, text);
    check(strcmp(text, "0.20") == 0, "0.20 readout");
    zzgamma_format(500, text);
    check(strcmp(text, "5.00") == 0, "5.00 readout");
    zzgamma_format(105, text);
    check(strcmp(text, "1.05") == 0, "1.05 readout keeps the leading zero");
    zzgamma_format(999, text);
    check(strcmp(text, "5.00") == 0, "out-of-range readout clamps");
}

int main(void)
{
    test_parse();
    test_slider_mapping();
    test_format();

    if (failures) {
        printf("zz9000_gamma_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("zz9000_gamma_test: OK\n");
    return 0;
}
