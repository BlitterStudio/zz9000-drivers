/*
 * Small pure helpers shared by the sampling-calibration UI and host tests.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ZZ9000_SAMPLING_CALIBRATION_H
#define ZZ9000_SAMPLING_CALIBRATION_H

#include "zz9000_capture_calibration.h"

/* A bin is clean only when its expected-pixel and temporal scores agree. */
static inline unsigned char zz_sampling_score_clean(unsigned long wrong,
    unsigned long changed)
{
    return wrong == 0 && changed == 0;
}

/* The sweep bins begin at the C28 minimum and cover the full circle. */
static inline unsigned zz_sampling_phase_bin(int phase)
{
    unsigned offset = (unsigned)(zz_capture_phase_wrap(phase) -
        ZZ_CAPTURE_PHASE_MIN);

    return (offset / ZZ_CAPTURE_BIN_STEPS) % ZZ_CAPTURE_BINS;
}

#endif /* ZZ9000_SAMPLING_CALIBRATION_H */
