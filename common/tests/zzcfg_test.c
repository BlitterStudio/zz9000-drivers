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

/* Every canonical key ZZTop writes for firmware ABI 2.9+. */
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
    v->videocap_crop_h = 188;
    v->videocap_crop_v = 26;
    v->offscreen_bitmaps = 1;
    v->video_overlay = 1;
}

int main(void)
{
    struct zzcfg_values a, b;
    char text[ZZCFG_MAX_SIZE];
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

    /* 8. ZZTop can still save coherent settings for pre-2.9 firmware. */
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

    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("zzcfg round-trip: all checks passed\n");
    return 0;
}
