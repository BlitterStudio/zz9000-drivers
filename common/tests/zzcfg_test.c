/*
 * Host round-trip checks for the ZZ9000.CFG editor model.
 *
 * ZZTop replaces the file wholesale with the keys it knows, so the generated
 * text must contain every key the firmware parses and every value must
 * survive a generate->parse cycle unchanged. tools/check-cfg-keys.sh keeps
 * the key *set* honest across both repos; this keeps the parse and render
 * behaviour honest.
 *
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <string.h>
#include "zzcfg_amiga.h"

static int failures;

static void check(int cond, const char *what)
{
    if (!cond) { printf("FAIL: %s\n", what); failures++; }
}

/* Every canonical key ZZTop writes with the firmware profile capability. */
static const char *firmware_keys[] = {
    "videocap_profile", "videocap_sample", "videocap_crop_h",
    "videocap_crop_v",
    "scanline_mode", "scanline_parity", "int2", "mac",
    "offscreen_bitmaps", "video_overlay", "hdf", NULL
};

static void defaults(struct zzcfg_values *v)
{
    memset(v, 0, sizeof(*v));
    v->videocap_profile = ZZCFG_VCAP_FULL_60;
    v->use_videocap_profile_key = 1;
    v->videocap_crop_h = ZZCFG_VIDEOCAP_CROP_H_COMPAT;
    v->videocap_crop_v = ZZCFG_VIDEOCAP_CROP_V_COMPAT;
    v->offscreen_bitmaps = 1;
    v->video_overlay = 1;
}

static int has_exact_line(const char *text, const char *line)
{
    const char *p = text;
    size_t len = strlen(line);

    while ((p = strstr(p, line)) != NULL) {
        if ((p == text || p[-1] == '\n') && p[len] == '\n') return 1;
        p += len;
    }
    return 0;
}

static void check_crop_render(const char *text, const char *axis,
    unsigned value, int present, const char *what)
{
    char active[64];
    char inactive[64];

    snprintf(active, sizeof(active), "videocap_crop_%s = %u", axis, value);
    snprintf(inactive, sizeof(inactive), "#videocap_crop_%s = %u", axis, value);
    check(has_exact_line(text, present ? active : inactive), what);
}

int main(void)
{
    struct zzcfg_values a, b;
    char text[ZZCFG_MAX_SIZE];
    const char *mixed;
    UWORD n;
    int i;

    /* 1. every firmware key appears in generated output */
    defaults(&a);
    n = zzcfg_generate(&a, text, sizeof(text));
    check(n > 0, "generate produced output");
    for (i = 0; firmware_keys[i]; i++) {
        char needle[64];
        snprintf(needle, sizeof(needle), "%s = ", firmware_keys[i]);
        if (!strstr(text, needle)) {
            printf("FAIL: generated file is missing key '%s'\n",
                   firmware_keys[i]);
            failures++;
        }
    }

    /* 2. the removed key must NOT be written any more */
    check(strstr(text, "yuv_rect") == NULL, "yuv_rect is no longer emitted");

    /* 3. non-default values survive generate -> parse */
    defaults(&a);
    a.videocap_profile = ZZCFG_VCAP_FILTERED_NTSC_EXACT;
    a.videocap_sample = 2; a.videocap_crop_h = 200;
    a.videocap_crop_v = 30;
    a.videocap_crop_h_present = 1;
    a.videocap_crop_v_present = 1;
    a.scanline_mode = 3;
    a.scanline_parity = 1; a.int2 = 1;
    a.offscreen_bitmaps = 0; a.video_overlay = 0;
    strcpy(a.mac, "68:82:F2:12:34:56");
    strcpy(a.hdf, "games.hdf");

    n = zzcfg_generate(&a, text, sizeof(text));
    defaults(&b);
    zzcfg_parse_text(text, n, &b);

    check(b.videocap_profile == ZZCFG_VCAP_FILTERED_NTSC_EXACT,
          "videocap_profile round-trips");
    check(b.videocap_sample == 2, "videocap_sample=odd round-trips");
    check(b.videocap_crop_h == 200, "videocap_crop_h round-trips");
    check(b.videocap_crop_v == 30, "videocap_crop_v round-trips");
    check(b.videocap_crop_h_present == 1,
          "videocap_crop_h presence round-trips");
    check(b.videocap_crop_v_present == 1,
          "videocap_crop_v presence round-trips");
    check(b.scanline_mode == 3, "scanline_mode round-trips");
    check(b.scanline_parity == 1, "scanline_parity round-trips");
    check(b.int2 == 1, "int2 round-trips");
    check(b.offscreen_bitmaps == 0, "offscreen_bitmaps=off round-trips");
    check(b.video_overlay == 0, "video_overlay=off round-trips");
    check(strcmp(b.mac, "68:82:F2:12:34:56") == 0, "mac round-trips");
    check(strcmp(b.hdf, "games.hdf") == 0, "hdf round-trips");

    /* 4. the on state round-trips too - the dangerous direction is a saved
     * "on" coming back as off and silently disabling an accelerated path. */
    defaults(&a);
    n = zzcfg_generate(&a, text, sizeof(text));
    memset(&b, 0, sizeof(b));   /* deliberately zeroed, not pre-defaulted */
    zzcfg_parse_text(text, n, &b);
    check(b.offscreen_bitmaps == 1, "offscreen_bitmaps=on round-trips");
    check(b.video_overlay == 1, "video_overlay=on round-trips");

    /* 5. all capture sample spellings parse, case-insensitively, and an
     * invalid value leaves the previous selection untouched. */
    defaults(&b);
    b.videocap_sample = 2;
    {
        const char *samples =
            "videocap_sample = average\n"
            "videocap_sample = ODD\n"
            "videocap_sample = EvEn\n"
            "videocap_sample = sideways\n";
        zzcfg_parse_text(samples, (UWORD)strlen(samples), &b);
    }
    check(b.videocap_sample == 1, "videocap_sample values parse safely");

    /* 6. New profiles, legacy profile components, and 12-bit crop values
     * parse strictly. Invalid values leave the previous selection alone. */
    defaults(&b);
    {
        const char *capture =
            "videocap_profile = FILTERED_PAL_EXACT\n"
            "videocap_profile = unclear\n"
            "videocap_shres = FILTER\n"
            "videocap_shres = full\n"
            "videocap_shres = maybe\n"
            "videocap_crop_h = 4095\n"
            "videocap_crop_h = 4096\n"
            "videocap_crop_v = 0\n"
            "videocap_crop_v = -1\n";
        zzcfg_parse_text(capture, (UWORD)strlen(capture), &b);
    }
    check(b.videocap_profile == ZZCFG_VCAP_FULL_EXACT,
          "new and legacy videocap values parse safely");
    check(b.videocap_crop_h == 4095, "videocap_crop_h rejects overflow");
    check(b.videocap_crop_v == 0, "videocap_crop_v rejects negative values");
    check(b.videocap_crop_h_present == 1,
          "valid videocap_crop_h marks the axis present");
    check(b.videocap_crop_v_present == 1,
          "valid videocap_crop_v marks the axis present");

    /* 7. a file that still contains the retired key parses without
     * disturbing anything else */
    defaults(&b);
    {
        const char *legacy =
            "yuv_rect = off\n"
            "video_overlay = off\n";
        zzcfg_parse_text(legacy, (UWORD)strlen(legacy), &b);
    }
    check(b.video_overlay == 0, "retired key does not block later keys");

    /* 8. ZZTop can still save coherently without the profile capability. */
    defaults(&a);
    a.use_videocap_profile_key = 0;
    a.videocap_profile = ZZCFG_VCAP_FILTERED_PAL_EXACT;
    n = zzcfg_generate(&a, text, sizeof(text));
    check(strstr(text, "videocap_profile = ") == NULL,
          "legacy firmware does not receive the new key");
    check(strstr(text, "videocap_mode = pal") != NULL,
          "legacy mode is emitted");
    check(strstr(text, "videocap_shres = filter") != NULL,
          "legacy width is emitted");
    check(strstr(text, "nonstandard_vsync = pal") != NULL,
          "legacy refresh is emitted");

    /* 9. Crop-key presence is independent. Missing axes remain Automatic,
     * render as inactive examples, and survive a generate -> parse cycle. */
    defaults(&a);
    n = zzcfg_generate(&a, text, sizeof(text));
    check_crop_render(text, "h", 188, 0,
          "absent Crop H renders inactive");
    check_crop_render(text, "v", 26, 0,
          "absent Crop V renders inactive");
    defaults(&b);
    zzcfg_parse_text(text, n, &b);
    check(b.videocap_crop_h_present == 0 &&
          b.videocap_crop_v_present == 0,
          "both absent crop axes round-trip as Automatic");

    defaults(&b);
    {
        const char *h_only = "videocap_crop_h = 279\n";
        zzcfg_parse_text(h_only, (UWORD)strlen(h_only), &b);
    }
    check(b.videocap_crop_h == 279 && b.videocap_crop_h_present == 1,
          "H-only crop parses as present");
    check(b.videocap_crop_v == 26 && b.videocap_crop_v_present == 0,
          "H-only crop leaves V absent");
    n = zzcfg_generate(&b, text, sizeof(text));
    check_crop_render(text, "h", 279, 1,
          "H-only crop renders H active");
    check_crop_render(text, "v", 26, 0,
          "H-only crop renders V inactive");

    defaults(&b);
    {
        const char *v_only = "videocap_crop_v = 41\n";
        zzcfg_parse_text(v_only, (UWORD)strlen(v_only), &b);
    }
    check(b.videocap_crop_h == 188 && b.videocap_crop_h_present == 0,
          "V-only crop leaves H absent");
    check(b.videocap_crop_v == 41 && b.videocap_crop_v_present == 1,
          "V-only crop parses as present");
    n = zzcfg_generate(&b, text, sizeof(text));
    check_crop_render(text, "h", 188, 0,
          "V-only crop renders H inactive");
    check_crop_render(text, "v", 41, 1,
          "V-only crop renders V active");

    defaults(&b);
    {
        const char *both_custom =
            "videocap_crop_h = 279\n"
            "videocap_crop_v = 40\n";
        zzcfg_parse_text(both_custom, (UWORD)strlen(both_custom), &b);
    }
    check(b.videocap_crop_h_present == 1 &&
          b.videocap_crop_v_present == 1,
          "both custom crop axes parse as present");
    n = zzcfg_generate(&b, text, sizeof(text));
    check_crop_render(text, "h", 279, 1,
          "both-custom Crop H renders active");
    check_crop_render(text, "v", 40, 1,
          "both-custom Crop V renders active");

    /* Explicit historical defaults are still Custom, never Automatic. */
    defaults(&b);
    {
        const char *legacy_crop =
            "videocap_crop_h = 188\n"
            "videocap_crop_v = 26\n";
        zzcfg_parse_text(legacy_crop, (UWORD)strlen(legacy_crop), &b);
    }
    check(b.videocap_crop_h_present == 1 &&
          b.videocap_crop_v_present == 1,
          "explicit legacy 188/26 remains Custom");
    n = zzcfg_generate(&b, text, sizeof(text));
    check_crop_render(text, "h", 188, 1,
          "legacy Crop H remains active");
    check_crop_render(text, "v", 26, 1,
          "legacy Crop V remains active");

    /* 0 and 4095 are literal boundary values, not Automatic sentinels. */
    defaults(&b);
    {
        const char *boundaries =
            "videocap_crop_h = 0\n"
            "videocap_crop_v = 4095\n";
        zzcfg_parse_text(boundaries, (UWORD)strlen(boundaries), &b);
    }
    check(b.videocap_crop_h == 0 && b.videocap_crop_h_present == 1,
          "Crop H zero remains explicit");
    check(b.videocap_crop_v == 4095 && b.videocap_crop_v_present == 1,
          "Crop V 4095 remains explicit");
    n = zzcfg_generate(&b, text, sizeof(text));
    check_crop_render(text, "h", 0, 1,
          "Crop H zero renders active");
    check_crop_render(text, "v", 4095, 1,
          "Crop V 4095 renders active");

    /* Invalid-only axes leave both their values and presence untouched. */
    defaults(&b);
    {
        const char *invalid_crops =
            "videocap_crop_h = 4096\n"
            "videocap_crop_v = -1\n";
        zzcfg_parse_text(invalid_crops, (UWORD)strlen(invalid_crops), &b);
    }
    check(b.videocap_crop_h == 188 && b.videocap_crop_h_present == 0,
          "invalid Crop H remains absent");
    check(b.videocap_crop_v == 26 && b.videocap_crop_v_present == 0,
          "invalid Crop V remains absent");

    /* Centered output is an appended, capability-gated profile.  A new
     * stack preserves it exactly; mixed/old stacks render the documented
     * full_60 fallback and never emit an unsupported profile value. */
    defaults(&a);
    a.firmware_capabilities = ZZ_FW_CAP_VIDEOCAP_CENTERED_1080P;
    a.videocap_profile = ZZCFG_VCAP_CENTERED_1080P_60;
    n = zzcfg_generate(&a, text, sizeof(text));
    check(strstr(text, "videocap_profile = centered_1080p_60") != NULL,
          "supported centered profile is generated");
    defaults(&b);
    b.firmware_capabilities = ZZ_FW_CAP_VIDEOCAP_CENTERED_1080P;
    zzcfg_parse_text(text, n, &b);
    check(b.videocap_profile == ZZCFG_VCAP_CENTERED_1080P_60,
          "supported centered profile round-trips");

    defaults(&a);
    a.videocap_profile = ZZCFG_VCAP_CENTERED_1080P_60;
    n = zzcfg_generate(&a, text, sizeof(text));
    check(strstr(text, "videocap_profile = full_60") != NULL,
          "unsupported centered profile falls back on save");
    check(zzcfg_profile_sanitize(ZZCFG_VCAP_CENTERED_1080P_60, 0) ==
          ZZCFG_VCAP_FULL_60, "unsupported centered profile sanitizes");
    check(zzcfg_profile_sanitize(ZZCFG_VCAP_CENTERED_1080P_60,
          ZZ_FW_CAP_VIDEOCAP_CENTERED_1080P) ==
          ZZCFG_VCAP_CENTERED_1080P_60,
          "dynamic profile capability preserves centered output");

    defaults(&b);
    b.firmware_capabilities = ZZ_FW_CAP_VIDEOCAP_CENTERED_1080P;
    mixed = "videocap_profile = centered_1080p_60\n"
            "videocap_mode = 800x600\n";
    zzcfg_parse_text(mixed, (UWORD)strlen(mixed), &b);
    check(b.videocap_profile == ZZCFG_VCAP_FILTERED_60,
          "legacy mode after centered resets full-width component");

    defaults(&b);
    b.firmware_capabilities = ZZ_FW_CAP_VIDEOCAP_CENTERED_1080P;
    mixed = "videocap_profile = centered_1080p_60\n"
            "videocap_mode = 800x600\n"
            "videocap_shres = full\n";
    zzcfg_parse_text(mixed, (UWORD)strlen(mixed), &b);
    check(b.videocap_profile == ZZCFG_VCAP_FULL_60,
          "later legacy full-width key still wins");

    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("zzcfg round-trip: all checks passed\n");
    return 0;
}
