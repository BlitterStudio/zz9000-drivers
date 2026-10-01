/*
 * Capture-window edits: live display values never become saved overrides
 * until that particular field is edited.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ZZ_VCAP_EDIT_H
#define ZZ_VCAP_EDIT_H

#include "zzcfg_amiga.h"
#include "zz_vcap_live.h"

#define ZZ_VCAP_EDIT_CROP_H  (1U << 0)
#define ZZ_VCAP_EDIT_CROP_V  (1U << 1)
#define ZZ_VCAP_EDIT_WIDTH   (1U << 2)
#define ZZ_VCAP_EDIT_HEIGHT  (1U << 3)
#define ZZ_VCAP_EDIT_PHASE   (1U << 4)
#define ZZ_VCAP_EDIT_ALL     31U

struct zz_vcap_edit {
    unsigned present;
    unsigned edited;
};

static inline void zz_vcap_edit_init(struct zz_vcap_edit *edit,
    const struct zzcfg_values *saved, enum zz_vcap_phase_domain domain)
{
    edit->present = (saved->videocap_crop_h_present ? ZZ_VCAP_EDIT_CROP_H : 0) |
        (saved->videocap_crop_v_present ? ZZ_VCAP_EDIT_CROP_V : 0) |
        (saved->videocap_width_present ? ZZ_VCAP_EDIT_WIDTH : 0) |
        (saved->videocap_height_present ? ZZ_VCAP_EDIT_HEIGHT : 0) |
        ((domain == ZZ_VCAP_PHASE_C28 ? saved->videocap_c28_phase_present :
            domain == ZZ_VCAP_PHASE_E7M ? saved->videocap_phase_present : 0) ?
            ZZ_VCAP_EDIT_PHASE : 0);
    edit->edited = 0;
}

static inline void zz_vcap_edit_set(struct zz_vcap_edit *edit,
    unsigned field, unsigned present)
{
    edit->edited |= field;
    if (present) edit->present |= field;
    else edit->present &= ~field;
}

static inline void zz_vcap_edit_automatic(struct zz_vcap_edit *edit)
{
    edit->edited = ZZ_VCAP_EDIT_ALL;
    edit->present = 0;
}

/* The baseline is the immutable staged state at window entry, not a copy
 * reseeded from live readback. Untouched values and the other clock domain
 * remain byte-for-byte intact. Presence changes count even at the same value. */
static inline int zz_vcap_edit_stage(const struct zz_vcap_edit *edit,
    const struct zzcfg_values *base, enum zz_vcap_phase_domain domain,
    UWORD sample, UWORD crop_h, UWORD crop_v, UWORD width, UWORD height,
    int phase, struct zzcfg_values *candidate)
{
    *candidate = *base;
    candidate->videocap_sample = sample;
    if (edit->edited & ZZ_VCAP_EDIT_CROP_H) {
        candidate->videocap_crop_h_present = !!(edit->present & ZZ_VCAP_EDIT_CROP_H);
        if (candidate->videocap_crop_h_present) candidate->videocap_crop_h = crop_h;
    }
    if (edit->edited & ZZ_VCAP_EDIT_CROP_V) {
        candidate->videocap_crop_v_present = !!(edit->present & ZZ_VCAP_EDIT_CROP_V);
        if (candidate->videocap_crop_v_present) candidate->videocap_crop_v = crop_v;
    }
    if (edit->edited & ZZ_VCAP_EDIT_WIDTH) {
        candidate->videocap_width_present = !!(edit->present & ZZ_VCAP_EDIT_WIDTH);
        candidate->videocap_width = candidate->videocap_width_present ? width : 0;
    }
    if (edit->edited & ZZ_VCAP_EDIT_HEIGHT) {
        candidate->videocap_height_present = !!(edit->present & ZZ_VCAP_EDIT_HEIGHT);
        candidate->videocap_height = candidate->videocap_height_present ? height : 0;
    }
    if ((edit->edited & ZZ_VCAP_EDIT_PHASE) && domain == ZZ_VCAP_PHASE_C28) {
        candidate->videocap_c28_phase_present = !!(edit->present & ZZ_VCAP_EDIT_PHASE);
        candidate->videocap_c28_phase = candidate->videocap_c28_phase_present ? phase : 0;
    } else if ((edit->edited & ZZ_VCAP_EDIT_PHASE) && domain == ZZ_VCAP_PHASE_E7M) {
        candidate->videocap_phase_present = !!(edit->present & ZZ_VCAP_EDIT_PHASE);
        candidate->videocap_phase = candidate->videocap_phase_present ? phase : 0;
    }
    return candidate->videocap_sample != base->videocap_sample ||
        candidate->videocap_crop_h_present != base->videocap_crop_h_present ||
        (candidate->videocap_crop_h_present && candidate->videocap_crop_h != base->videocap_crop_h) ||
        candidate->videocap_crop_v_present != base->videocap_crop_v_present ||
        (candidate->videocap_crop_v_present && candidate->videocap_crop_v != base->videocap_crop_v) ||
        candidate->videocap_width_present != base->videocap_width_present ||
        (candidate->videocap_width_present && candidate->videocap_width != base->videocap_width) ||
        candidate->videocap_height_present != base->videocap_height_present ||
        (candidate->videocap_height_present && candidate->videocap_height != base->videocap_height) ||
        candidate->videocap_c28_phase_present != base->videocap_c28_phase_present ||
        (candidate->videocap_c28_phase_present && candidate->videocap_c28_phase != base->videocap_c28_phase) ||
        candidate->videocap_phase_present != base->videocap_phase_present ||
        (candidate->videocap_phase_present && candidate->videocap_phase != base->videocap_phase);
}

#endif
