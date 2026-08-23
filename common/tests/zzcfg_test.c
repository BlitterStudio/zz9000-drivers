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

/* Register-script seam for zzcfg_read_raw(). Firmware services Zorro
 * writes asynchronously: a reset command is not acknowledged until a
 * later status read. A read command issued before that acknowledgement
 * is deliberately ignored, reproducing the stale-OK hardware race. */
static UWORD raw_status;
static UWORD raw_len;
static int raw_reset_pending;
static int raw_read_pending;
static int raw_started_before_reset;
static char raw_buffer[64];
static const char *raw_next_text;

static void raw_io_reset(const char *old_text, const char *next_text)
{
    size_t n = strlen(old_text);

    memset(raw_buffer, 0, sizeof(raw_buffer));
    memcpy(raw_buffer, old_text, n);
    raw_status = ZZ_CFG_FILE_OK;
    raw_len = (UWORD)n;
    raw_reset_pending = 0;
    raw_read_pending = 0;
    raw_started_before_reset = 0;
    raw_next_text = next_text;
}

UWORD zzcfg_test_reg_read(ULONG board, ULONG offset)
{
    (void)board;
    if (offset == ZZ_REG_CONFIG_FILE) {
        if (raw_reset_pending) {
            raw_reset_pending = 0;
            raw_status = ZZ_CFG_FILE_IDLE;
        } else if (raw_read_pending) {
            size_t n = strlen(raw_next_text);

            memset(raw_buffer, 0, sizeof(raw_buffer));
            memcpy(raw_buffer, raw_next_text, n);
            raw_len = (UWORD)n;
            raw_status = ZZ_CFG_FILE_OK;
            raw_read_pending = 0;
        }
        return raw_status;
    }
    if (offset == ZZ_REG_CONFIG_FILE_LEN)
        return raw_len;
    return 0;
}

void zzcfg_test_reg_write(ULONG board, ULONG offset, UWORD value)
{
    (void)board;
    if (offset != ZZ_REG_CONFIG_FILE)
        return;
    if (value == 0) {
        raw_reset_pending = 1;
    } else if (value == 1) {
        if (raw_reset_pending)
            raw_started_before_reset = 1;
        else
            raw_read_pending = 1;
    }
}

UBYTE zzcfg_test_buffer_read(ULONG board, UWORD offset)
{
    (void)board;
    return (UBYTE)raw_buffer[offset];
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

    /* Audio control-plane keys (firmware U5): absent from pre-audio
     * files' output, parsed and regenerated exactly when present. */
    defaults(&a);
    n = zzcfg_generate(&a, text, sizeof(text));
    check(strstr(text, "audio_") == NULL,
          "no audio keys generated without presence");

    defaults(&b);
    {
        const char *audio_file =
            "audio_active = 3\n"
            "audio_baseline = 35910\n"
            "audio_scene2_lpf = 16000\n"
            "audio_scene2_eq01 = 12800\n"
            "audio_scene2_eq23 = 6450\n"
            "audio_scene2_eq45 = 12900\n"
            "audio_scene2_eq67 = 100\n"
            "audio_scene2_eq89 = 51\n"
            "audio_scene2_out = 6500\n"
            "audio_scene2_pan = 75\n"
            "audio_scene2_out = 6999\n";   /* last value wins */
        zzcfg_parse_text(audio_file, (UWORD)strlen(audio_file), &b);
    }
    check(b.audio_active == 3 && b.audio_active_present,
          "audio_active parses with presence");
    check(b.audio_baseline == 35910 && b.audio_baseline_present,
          "audio_baseline parses with presence");
    check(b.audio_scene_mask[2] == 0xff && b.audio_scene_mask[0] == 0,
          "scene key group masks complete");
    check(b.audio_scene_lpf[2] == 16000 && b.audio_scene_out[2] == 6999,
          "scene values round-trip packed");

    n = zzcfg_generate(&b, text, sizeof(text));
    check(has_exact_line(text, "audio_active = 3") &&
          has_exact_line(text, "audio_baseline = 35910") &&
          has_exact_line(text, "audio_scene2_lpf = 16000") &&
          has_exact_line(text, "audio_scene2_eq01 = 12800") &&
          has_exact_line(text, "audio_scene2_out = 6999") &&
          has_exact_line(text, "audio_scene2_pan = 75"),
          "audio keys regenerate exactly");
    check(strstr(text, "audio_scene0_") == NULL,
          "absent scenes stay absent");

    defaults(&a);
    zzcfg_parse_text(text, n, &a);
    check(a.audio_active == 3 && a.audio_baseline == 35910 &&
          a.audio_scene_mask[2] == 0xff && a.audio_scene_out[2] == 6999,
          "generated audio block reparses identically");

    /* A hand-edited partial scene stays partial: only present keys are
     * regenerated, so one Settings save never zero-fills the absent
     * fields (the firmware would fold them into a silent scene). */
    defaults(&b);
    {
        const char *partial = "audio_scene2_lpf = 16000\n";
        zzcfg_parse_text(partial, (UWORD)strlen(partial), &b);
    }
    check(b.audio_scene_mask[2] == 0x01 && b.audio_scene_lpf[2] == 16000,
          "partial scene parses as lpf-only");
    n = zzcfg_generate(&b, text, sizeof(text));
    check(has_exact_line(text, "audio_scene2_lpf = 16000"),
          "partial scene regenerates its one key");
    check(strstr(text, "audio_scene2_eq") == NULL &&
          strstr(text, "audio_scene2_out") == NULL &&
          strstr(text, "audio_scene2_pan") == NULL,
          "partial scene grows no sibling keys on save");

    /* Scene names (firmware nm keys): per-chunk presence like every
     * other audio key, so a partial name group round-trips exactly as
     * parsed -- one Settings save never zero-pads or drops chunks. */
    defaults(&b);
    {
        const char *partial_name = "audio_scene2_nm1 = 21347\n";
        zzcfg_parse_text(partial_name, (UWORD)strlen(partial_name), &b);
    }
    check(b.audio_scene_mask[2] == 0x0100 && b.audio_scene_nm[2][0] == 21347,
          "single nm1 key parses with its own mask bit");
    n = zzcfg_generate(&b, text, sizeof(text));
    check(has_exact_line(text, "audio_scene2_nm1 = 21347"),
          "single nm1 key regenerates exactly");
    check(strstr(text, "audio_scene2_nm2") == NULL &&
          strstr(text, "audio_scene2_lpf") == NULL &&
          strstr(text, "audio_scene0_") == NULL,
          "partial name grows no sibling keys on save");

    defaults(&b);
    {
        /* "Tag 9" as chunks: 'T''a' 'g'' ' '9'0 terminator. */
        const char *full_name =
            "audio_scene5_nm1 = 21601\n"
            "audio_scene5_nm2 = 26400\n"
            "audio_scene5_nm3 = 14592\n"
            "audio_scene5_nm4 = 0\n";
        zzcfg_parse_text(full_name, (UWORD)strlen(full_name), &b);
    }
    check(b.audio_scene_mask[5] == 0x0f00 &&
          b.audio_scene_nm[5][0] == 21601 &&
          b.audio_scene_nm[5][1] == 26400 &&
          b.audio_scene_nm[5][2] == 14592,
          "name chunk group parses packed");
    n = zzcfg_generate(&b, text, sizeof(text));
    check(has_exact_line(text, "audio_scene5_nm1 = 21601") &&
          has_exact_line(text, "audio_scene5_nm2 = 26400") &&
          has_exact_line(text, "audio_scene5_nm3 = 14592") &&
          has_exact_line(text, "audio_scene5_nm4 = 0"),
          "name chunk group regenerates exactly");
    check(strstr(text, "audio_scene5_nm5") == NULL,
          "absent trailing name chunks stay absent");

    defaults(&b);
    {
        const char *junk = "audio_active = 8\naudio_scene5_eq01 = x\n";
        zzcfg_parse_text(junk, (UWORD)strlen(junk), &b);
    }
    check(b.audio_active == 8 && b.audio_scene_mask[5] == 0,
          "invalid values parse permissively without presence");


    /* A second raw read starts with the previous request's OK status.
     * It must observe the reset acknowledgement before issuing READ,
     * otherwise it returns the previous shared-buffer snapshot. */
    {
        char raw[32];
        UWORD raw_bytes = 0;
        UWORD st;

        raw_io_reset("audio_baseline = 32832\n",
                     "audio_baseline = 32896\n");
        st = zzcfg_read_raw(0, raw, sizeof(raw), &raw_bytes);
        check(raw_started_before_reset == 0,
              "raw read waits for reset acknowledgement");
        check(st == ZZ_CFG_FILE_OK,
              "raw read reaches the new terminal status");
        check(raw_bytes == strlen("audio_baseline = 32896\n") &&
              strcmp(raw, "audio_baseline = 32896\n") == 0,
              "raw read returns the newly staged config");
    }
    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("zzcfg round-trip: all checks passed\n");
    return 0;
}
