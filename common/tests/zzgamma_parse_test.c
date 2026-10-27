/*
 * Host tests for the ZZGamma argument parser.
 *
 * The parser is a portable header (ZZGamma/zzgamma_parse.h) so the CLI
 * and the host test share one implementation; these checks pin the
 * accepted spellings to the firmware's parse_gamma rules.
 *
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <string.h>

#include "../../ZZGamma/zzgamma_parse.h"

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

int main(void)
{
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

    if (failures) {
        printf("zzgamma_parse_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("zzgamma_parse_test: OK\n");
    return 0;
}
