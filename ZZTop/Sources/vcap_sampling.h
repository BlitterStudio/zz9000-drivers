/*
 * Interactive C28 capture-phase sampling calibration.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ZZTOP_VCAP_SAMPLING_H
#define ZZTOP_VCAP_SAMPLING_H

#include "zz9000_capture_calibration.h"
#include "zz_vcap_client.h"

/* Returns 1 after Enter leaves the selected phase live, 0 after Esc/Ctrl-C
 * restores the entry phase, -1 on a rejected/failed run, or -2 when the
 * required restoration did not complete. quality is written only on 1. */
int vcap_sampling_run(const struct zz_vcap_io *card_io, int ntsc, int lace,
    int entry_phase, int *selected_phase,
    unsigned char quality[ZZ_CAPTURE_BINS], char *failure,
    unsigned failure_size);

#endif /* ZZTOP_VCAP_SAMPLING_H */
