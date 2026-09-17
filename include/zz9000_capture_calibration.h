/*
 * Raw capture calibration contract and platform-independent measurements.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ZZ9000_CAPTURE_CALIBRATION_H
#define ZZ9000_CAPTURE_CALIBRATION_H

#include <stdint.h>

/* Exact protocol match is mandatory before ANY write to these registers.
 * This ABI belongs to the opt-in C28 capture build, not legacy E7M phase. */
#define ZZ_CAPTURE_CAP_REG           0x1240UL
#define ZZ_CAPTURE_CAP_C28           0x56510206UL
#define ZZ_CAPTURE_PHASE_TARGET_REG  0x1246UL
#define ZZ_CAPTURE_PHASE_COMMIT_REG  0x1248UL
#define ZZ_CAPTURE_PHASE_TOKEN       0xf05aU
#define ZZ_CAPTURE_PHASE_STATUS_REG  0x124cUL
#define ZZ_CAPTURE_CLOCK_STATUS_REG  0x1250UL
#define ZZ_CAPTURE_CLOCK_COUNTS_REG  0x1254UL
#define ZZ_CAPTURE_SNAPSHOT_REG      0x1260UL
#define ZZ_CAPTURE_ARM_REG           0x1264UL
#define ZZ_CAPTURE_ARM_TOKEN         0xca1cU
#define ZZ_CAPTURE_ADDRESS_REG       0x126aUL
#define ZZ_CAPTURE_DATA_REG          0x126cUL
#define ZZ_CAPTURE_GEOMETRY_REG      0x1270UL

#define ZZ_CAPTURE_PHASE_BUSY        (1UL << 12)
#define ZZ_CAPTURE_PHASE_DONE        (1UL << 13)
#define ZZ_CAPTURE_PHASE_ERROR       (1UL << 14)
#define ZZ_CAPTURE_PHASE_READY       (1UL << 15)
#define ZZ_CAPTURE_CLOCK_FREQUENCY   (1UL << 0)
#define ZZ_CAPTURE_CLOCK_LOCKED      (1UL << 1)
#define ZZ_CAPTURE_CLOCK_READY       (1UL << 2)
#define ZZ_CAPTURE_CLOCK_FAULT       (1UL << 3)
#define ZZ_CAPTURE_CLOCK_C28         (1UL << 4)
#define ZZ_CAPTURE_SNAPSHOT_VALID    (1UL << 0)
#define ZZ_CAPTURE_SNAPSHOT_BUSY     (1UL << 1)
#define ZZ_CAPTURE_SNAPSHOT_ARM      (1UL << 2)
#define ZZ_CAPTURE_SNAPSHOT_LACE     (1UL << 3)
#define ZZ_CAPTURE_SNAPSHOT_PARITY   (1UL << 4)
#define ZZ_CAPTURE_SNAPSHOT_NTSC     (1UL << 5)

#define ZZ_CAPTURE_PHASE_STEPS 1792
#define ZZ_CAPTURE_PHASE_MIN   (-896)
#define ZZ_CAPTURE_PHASE_MAX   895
#define ZZ_CAPTURE_BINS        64U
#define ZZ_CAPTURE_BIN_STEPS   28U
#define ZZ_CAPTURE_MIN_EYE     3U
#define ZZ_CAPTURE_COLUMNS    256U
#define ZZ_CAPTURE_ROWS        4U
#define ZZ_CAPTURE_SAMPLES    (ZZ_CAPTURE_COLUMNS * ZZ_CAPTURE_ROWS)

struct zz_capture_eye {
    unsigned start_bin;
    unsigned bins;
    int phase;
    unsigned margin; /* Fine steps from midpoint to nearest tested clean end. */
    int full_circle;
};

/* Offsets from origin preserve interval direction across the signed wrap.
 * Initialize the two endpoints from neighboring tested coarse positions. */
struct zz_capture_boundary {
    int origin;
    unsigned good;
    unsigned bad;
};

static inline int zz_capture_phase_valid(int phase)
{
    return phase >= ZZ_CAPTURE_PHASE_MIN && phase <= ZZ_CAPTURE_PHASE_MAX;
}

static inline unsigned zz_capture_phase_encode(int phase)
{
    /* The write register is signed 16-bit; only readback packs 12 bits. */
    return (uint16_t)(int16_t)phase;
}

static inline int zz_capture_phase_decode(uint32_t status)
{
    unsigned phase = (unsigned)status & 0xfffU;
    return (phase & 0x800U) ? (int)phase - 4096 : (int)phase;
}

static inline int zz_capture_phase_wrap(int phase)
{
    phase %= ZZ_CAPTURE_PHASE_STEPS;
    if (phase < ZZ_CAPTURE_PHASE_MIN) phase += ZZ_CAPTURE_PHASE_STEPS;
    if (phase > ZZ_CAPTURE_PHASE_MAX) phase -= ZZ_CAPTURE_PHASE_STEPS;
    return phase;
}

static inline unsigned zz_capture_phase_distance(int a, int b)
{
    int distance = zz_capture_phase_wrap(a - b);
    return (unsigned)(distance < 0 ? -distance : distance);
}

static inline int zz_capture_boundary_pending(const struct zz_capture_boundary *edge)
{
    return edge->good > edge->bad ? edge->good - edge->bad > 1U :
                                   edge->bad - edge->good > 1U;
}

static inline int zz_capture_boundary_next(const struct zz_capture_boundary *edge)
{
    return zz_capture_phase_wrap(edge->origin + (int)((edge->good + edge->bad) / 2));
}

static inline void zz_capture_boundary_record(struct zz_capture_boundary *edge,
    int clean)
{
    unsigned middle = (edge->good + edge->bad) / 2;
    if (clean) edge->good = middle;
    else edge->bad = middle;
}

static inline int zz_capture_boundary_good_phase(const struct zz_capture_boundary *edge)
{
    return zz_capture_phase_wrap(edge->origin + (int)edge->good);
}

/* The interval is circular: -896 and +895 are neighbors. Equal widths
 * choose the center nearest the entry phase, then the lowest start bin.
 * All-clean is returned explicitly so the caller can reject an unmeasured
 * boundary. A flat or consistently wrong picture must be rejected by the
 * pattern comparator before it is ever added to the clean[] mask. */
static inline int zz_capture_select_eye(const unsigned char *clean,
    int entry_phase, struct zz_capture_eye *eye)
{
    unsigned i, total = 0, best = 0, best_distance = ZZ_CAPTURE_PHASE_STEPS;
    for (i = 0; i < ZZ_CAPTURE_BINS; ++i) total += !!clean[i];
    if (total == ZZ_CAPTURE_BINS) {
        eye->start_bin = 0;
        eye->bins = ZZ_CAPTURE_BINS;
        eye->phase = zz_capture_phase_wrap(entry_phase);
        eye->margin = ZZ_CAPTURE_PHASE_STEPS / 2;
        eye->full_circle = 1;
        return 1;
    }
    eye->full_circle = 0;
    for (i = 0; i < ZZ_CAPTURE_BINS; ++i) {
        unsigned length = 0, distance;
        int center;
        if (!clean[i] || clean[(i + ZZ_CAPTURE_BINS - 1) % ZZ_CAPTURE_BINS])
            continue;
        while (length < ZZ_CAPTURE_BINS && clean[(i + length) % ZZ_CAPTURE_BINS])
            ++length;
        center = zz_capture_phase_wrap(ZZ_CAPTURE_PHASE_MIN +
            (int)(i * ZZ_CAPTURE_BIN_STEPS +
                  (length - 1) * ZZ_CAPTURE_BIN_STEPS / 2));
        distance = zz_capture_phase_distance(center, entry_phase);
        if (length < ZZ_CAPTURE_MIN_EYE || length < best ||
            (length == best && distance >= best_distance))
            continue;
        best = length;
        best_distance = distance;
        eye->start_bin = i;
        eye->bins = length;
        eye->phase = center;
        eye->margin = (length - 1) * ZZ_CAPTURE_BIN_STEPS / 2;
    }
    return best != 0;
}

/* A 256-color permutation. Every red value is unique, allowing the raw
 * capture's unknown horizontal origin to be determined without tolerances.
 * Rotated channel values exercise all 24 RGB lines with many transitions.
 * The screen displays pen (x & 255) on every row. */
static inline uint32_t zz_capture_pattern_rgb(unsigned pen)
{
    unsigned r = (pen * 197U + 101U) & 255U;
    unsigned g = ((r << 3 | r >> 5) & 255U) ^ 0x5aU;
    unsigned b = ((r << 5 | r >> 3) & 255U) ^ 0xa5U;
    return (uint32_t)r << 16 | (uint32_t)g << 8 | b;
}

static inline unsigned zz_capture_pattern_errors(const uint32_t *samples)
{
    unsigned origin, i, errors = 0;
    for (origin = 0; origin < 256U; ++origin)
        if (samples[0] == zz_capture_pattern_rgb(origin)) break;
    if (origin == 256U) return ZZ_CAPTURE_SAMPLES;
    for (i = 0; i < ZZ_CAPTURE_SAMPLES; ++i)
        errors += samples[i] != zz_capture_pattern_rgb(
            origin + i % ZZ_CAPTURE_COLUMNS);
    return errors;
}

static inline unsigned zz_capture_changed_pixels(const uint32_t *a,
    const uint32_t *b)
{
    unsigned i, changed = 0;
    for (i = 0; i < ZZ_CAPTURE_SAMPLES; ++i) changed += a[i] != b[i];
    return changed;
}

#endif
