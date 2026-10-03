/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "zz_vcap_edit.h"

int main(void)
{
    struct zzcfg_values saved, candidate;
    struct zz_vcap_edit edit;
    memset(&saved, 0, sizeof(saved));
    saved.videocap_crop_h = 20;
    saved.videocap_crop_v = 30;
    saved.videocap_width = 640;
    saved.videocap_height = 400;
    saved.videocap_c28_phase = -32;
    saved.videocap_phase = -64;
    saved.videocap_phase_present = 1;
    saved.videocap_crop_h_present = saved.videocap_crop_v_present = 1;
    saved.videocap_width_present = saved.videocap_height_present = 1;
    saved.videocap_c28_phase_present = 1;

    /* Merely displaying unsaved live values must not overwrite staged CFG. */
    zz_vcap_edit_init(&edit, &saved, ZZ_VCAP_PHASE_C28);
    assert(!zz_vcap_edit_stage(&edit, &saved, ZZ_VCAP_PHASE_C28,
        0, 40, 50, 720, 256, 128, &candidate));
    assert(candidate.videocap_c28_phase == -32);
    assert(candidate.videocap_width == 640);

    /* Automatic followed by a phase-only edit must leave four axes automatic. */
    zz_vcap_edit_automatic(&edit);
    zz_vcap_edit_set(&edit, ZZ_VCAP_EDIT_PHASE, 1);
    assert(zz_vcap_edit_stage(&edit, &saved, ZZ_VCAP_PHASE_C28,
        0, 40, 50, 720, 256, 64, &candidate));
    assert(!candidate.videocap_crop_h_present && !candidate.videocap_crop_v_present);
    assert(!candidate.videocap_width_present && !candidate.videocap_height_present);
    assert(candidate.videocap_c28_phase_present && candidate.videocap_c28_phase == 64);
    assert(candidate.videocap_phase_present && candidate.videocap_phase == -64);

    /* Materializing an unchanged numeric phase is still a persistence change. */
    saved.videocap_c28_phase_present = 0;
    zz_vcap_edit_init(&edit, &saved, ZZ_VCAP_PHASE_C28);
    zz_vcap_edit_set(&edit, ZZ_VCAP_EDIT_PHASE, 1);
    assert(zz_vcap_edit_stage(&edit, &saved, ZZ_VCAP_PHASE_C28,
        0, 20, 30, 640, 400, -32, &candidate));
    assert(candidate.videocap_c28_phase_present);

    /* Clearing one dimension must not clear/materialize the other axis. */
    zz_vcap_edit_init(&edit, &saved, ZZ_VCAP_PHASE_C28);
    zz_vcap_edit_set(&edit, ZZ_VCAP_EDIT_WIDTH, 0);
    assert(zz_vcap_edit_stage(&edit, &saved, ZZ_VCAP_PHASE_C28,
        0, 20, 30, 0, 256, -32, &candidate));
    assert(!candidate.videocap_width_present && candidate.videocap_width == 0);
    assert(candidate.videocap_height_present && candidate.videocap_height == 400);

    /* Editing crop at its automatic-resolved number only materializes that axis. */
    saved.videocap_crop_h_present = saved.videocap_crop_v_present = 0;
    zz_vcap_edit_init(&edit, &saved, ZZ_VCAP_PHASE_E7M);
    zz_vcap_edit_set(&edit, ZZ_VCAP_EDIT_CROP_H, 1);
    zz_vcap_edit_set(&edit, ZZ_VCAP_EDIT_PHASE, 1);
    assert(zz_vcap_edit_stage(&edit, &saved, ZZ_VCAP_PHASE_E7M,
        0, 20, 30, 640, 400, -255, &candidate));
    assert(candidate.videocap_crop_h_present && !candidate.videocap_crop_v_present);
    assert(candidate.videocap_phase == -255);
    assert(candidate.videocap_c28_phase == saved.videocap_c28_phase);
    puts("zz_vcap_edit_test: PASS (untouched, Automatic, per-axis, presence, domain isolation)");
    return 0;
}
