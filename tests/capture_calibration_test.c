/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <string.h>
#include "zz9000_capture_calibration.h"

static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; \
    printf("FAIL line %u: %s\n", (unsigned)__LINE__, #x); } } while (0)

static void test_phase(void)
{
    int phase;
    for (phase = ZZ_CAPTURE_PHASE_MIN; phase <= ZZ_CAPTURE_PHASE_MAX; ++phase) {
        CHECK(zz_capture_phase_valid(phase));
        CHECK(zz_capture_phase_decode(zz_capture_phase_encode(phase)) == phase);
        /* The target register is signed 16-bit; applied status is 12-bit. */
        CHECK((int16_t)(uint16_t)zz_capture_phase_encode(phase) == phase);
        CHECK(zz_capture_phase_wrap(phase + ZZ_CAPTURE_PHASE_STEPS) == phase);
        CHECK(zz_capture_phase_wrap(phase - ZZ_CAPTURE_PHASE_STEPS) == phase);
    }
    CHECK(!zz_capture_phase_valid(-897));
    CHECK(!zz_capture_phase_valid(896));
    CHECK(zz_capture_phase_encode(-896) == 0xfc80);
    CHECK(zz_capture_phase_encode(-1) == 0xffff);
    CHECK(zz_capture_phase_decode(0x8c80) == -896);
    CHECK(zz_capture_phase_decode(0x837f) == 895);
}

static void test_eye(void)
{
    unsigned char clean[ZZ_CAPTURE_BINS];
    struct zz_capture_eye eye;
    memset(clean, 0, sizeof(clean));
    CHECK(!zz_capture_select_eye(clean, 0, &eye));
    clean[3] = clean[4] = 1;
    CHECK(!zz_capture_select_eye(clean, 0, &eye));
    clean[5] = 1;
    CHECK(zz_capture_select_eye(clean, 0, &eye));
    CHECK(eye.start_bin == 3 && eye.bins == 3);
    CHECK(eye.phase == -784 && eye.margin == 28 && !eye.full_circle);

    memset(clean, 0, sizeof(clean));
    clean[62] = clean[63] = clean[0] = clean[1] = clean[2] = 1;
    CHECK(zz_capture_select_eye(clean, 0, &eye));
    CHECK(eye.start_bin == 62 && eye.bins == 5);
    CHECK(eye.phase == -896 && eye.margin == 56);

    memset(clean, 0, sizeof(clean));
    clean[1] = clean[2] = clean[3] = 1;
    clean[31] = clean[32] = clean[33] = 1;
    CHECK(zz_capture_select_eye(clean, 0, &eye));
    CHECK(eye.start_bin == 31 && eye.phase == 0);
    CHECK(zz_capture_select_eye(clean, -840, &eye));
    CHECK(eye.start_bin == 1 && eye.phase == -840);
    /* Equal size and equal distance have a deterministic lowest-start tie. */
    CHECK(zz_capture_select_eye(clean, -420, &eye));
    CHECK(eye.start_bin == 1);

    memset(clean, 1, sizeof(clean));
    CHECK(zz_capture_select_eye(clean, 117, &eye));
    CHECK(eye.full_circle && eye.bins == ZZ_CAPTURE_BINS);
    CHECK(eye.phase == 117 && eye.margin == 896);
}

static void make_pattern(uint32_t *samples, unsigned origin)
{
    unsigned i;
    for (i = 0; i < ZZ_CAPTURE_SAMPLES; ++i)
        samples[i] = zz_capture_pattern_rgb(origin + i % ZZ_CAPTURE_COLUMNS);
}

static void test_refinement(void)
{
    struct zz_capture_boundary boundary;
    unsigned tests;
    /* Lower edge crosses the signed wrap: bad at +868, good at -896.
     * A clean aperture beginning 13 steps from +868 must refine to +881. */
    boundary.origin = 868;
    boundary.good = 28;
    boundary.bad = 0;
    tests = 0;
    while (zz_capture_boundary_pending(&boundary)) {
        unsigned offset = (boundary.good + boundary.bad) / 2;
        CHECK(zz_capture_boundary_next(&boundary) == zz_capture_phase_wrap(868 + (int)offset));
        zz_capture_boundary_record(&boundary, offset >= 13);
        CHECK(++tests <= 5);
    }
    CHECK(boundary.good == 13 && boundary.bad == 12);
    CHECK(zz_capture_boundary_good_phase(&boundary) == 881);
    /* Upper edge with the opposite clean/dirty ordering. */
    boundary.origin = 868;
    boundary.good = 0;
    boundary.bad = 28;
    while (zz_capture_boundary_pending(&boundary)) {
        unsigned offset = (boundary.good + boundary.bad) / 2;
        zz_capture_boundary_record(&boundary, offset <= 27);
    }
    CHECK(boundary.good == 27 && boundary.bad == 28);
    CHECK(zz_capture_boundary_good_phase(&boundary) == 895);
    boundary.origin = -896;
    boundary.good = 0;
    boundary.bad = 28;
    while (zz_capture_boundary_pending(&boundary))
        zz_capture_boundary_record(&boundary, 0);
    CHECK(boundary.good == 0 && boundary.bad == 1);
    CHECK(zz_capture_boundary_good_phase(&boundary) == -896);
}

static void test_pattern(void)
{
    uint32_t samples[ZZ_CAPTURE_SAMPLES], other[ZZ_CAPTURE_SAMPLES];
    unsigned origin, i, bit, transitions;
    for (origin = 0; origin < 256; ++origin) {
        make_pattern(samples, origin);
        CHECK(zz_capture_pattern_errors(samples) == 0);
    }
    make_pattern(samples, 239);
    memcpy(other, samples, sizeof(samples));
    CHECK(zz_capture_changed_pixels(samples, other) == 0);
    other[277] ^= 1;
    CHECK(zz_capture_changed_pixels(samples, other) == 1);
    CHECK(zz_capture_pattern_errors(other) != 0);
    memset(other, 0, sizeof(other));
    CHECK(zz_capture_pattern_errors(other) != 0);
    for (i = 0; i < ZZ_CAPTURE_SAMPLES; ++i)
        other[i] = zz_capture_pattern_rgb(12);
    CHECK(zz_capture_pattern_errors(other) != 0);
    /* Stable hires-like duplicates cannot qualify as SuperHires detail. */
    for (i = 0; i < ZZ_CAPTURE_SAMPLES; ++i)
        other[i] = zz_capture_pattern_rgb(i / 2);
    CHECK(zz_capture_pattern_errors(other) != 0);
    for (i = 0; i < ZZ_CAPTURE_SAMPLES; ++i)
        other[i] = zz_capture_pattern_rgb(i * 2);
    CHECK(zz_capture_pattern_errors(other) != 0);
    make_pattern(other, 240);
    CHECK(zz_capture_pattern_errors(other) == 0);
    CHECK(zz_capture_changed_pixels(samples, other) == ZZ_CAPTURE_SAMPLES);
    memcpy(other, samples, sizeof(samples));
    other[0] |= 0x01000000UL;
    CHECK(zz_capture_pattern_errors(other) != 0);
    /* The actual palette exercises every physical RGB bit repeatedly. */
    for (bit = 0; bit < 24; ++bit) {
        transitions = 0;
        for (i = 0; i < 256; ++i)
            transitions += !!((zz_capture_pattern_rgb(i) ^
                zz_capture_pattern_rgb(i + 1)) & (1UL << bit));
        CHECK(transitions >= 32);
    }
}

int main(void)
{
    test_phase();
    test_eye();
    test_refinement();
    test_pattern();
    printf("capture calibration: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
