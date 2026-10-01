/*
 * Pure state model for acknowledged ZZ9000 native-video calibration.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ZZ_VCAP_LIVE_H
#define ZZ_VCAP_LIVE_H

#include <exec/types.h>
#include "zz9000_hw.h"

#define ZZ_VCAP_LIVE_MIN_FW 0x0208
#define ZZ_VCAP_CROP_MAX    4095

enum zz_vcap_move {
    ZZ_VCAP_MOVE_LEFT = 0,
    ZZ_VCAP_MOVE_RIGHT,
    ZZ_VCAP_MOVE_UP,
    ZZ_VCAP_MOVE_DOWN
};

enum zz_vcap_request_result {
    ZZ_VCAP_REQUEST_CONFLICT = -1,
    ZZ_VCAP_REQUEST_PENDING = 0,
    ZZ_VCAP_REQUEST_APPLIED = 1
};

enum zz_vcap_standard {
    ZZ_VCAP_STANDARD_UNKNOWN = 0,
    ZZ_VCAP_STANDARD_PAL,
    ZZ_VCAP_STANDARD_NTSC
};

enum zz_vcap_anchor_owner {
    ZZ_VCAP_ANCHOR_SETTINGS = 0,
    ZZ_VCAP_ANCHOR_ADVANCED,
    ZZ_VCAP_ANCHOR_CALIBRATION,
    ZZ_VCAP_ANCHOR_PREVIEW,
    ZZ_VCAP_ANCHOR_COUNT
};

struct zz_vcap_control {
    UWORD sample;
    UWORD full_width;
    UWORD crop_h;
    UWORD crop_v;
    UWORD crop_h_present;
    UWORD crop_v_present;
};

struct zz_vcap_status {
    UBYTE request_sequence;
    UBYTE applied_sequence;
    UWORD standard_valid;
    UWORD ntsc;
    UWORD rejected;
    UWORD applied_valid;
    UWORD busy;
};

struct zz_vcap_path {
    UWORD sample;
    UWORD full_width;
};

struct zz_vcap_working {
    struct zz_vcap_control control;
    UWORD crop_h;
    UWORD crop_v;
};

struct zz_vcap_snapshot {
    ULONG status;
    ULONG raw;
    UWORD effective_h;
    UWORD effective_v;
};

struct zz_vcap_anchors {
    struct zz_vcap_snapshot value[ZZ_VCAP_ANCHOR_COUNT];
    UBYTE valid[ZZ_VCAP_ANCHOR_COUNT];
};

int zz_vcap_live_supported(UWORD firmware_revision,
    UWORD firmware_capabilities, ULONG rtl_capability);
int zz_vcap_control_valid(const struct zz_vcap_control *control);
ULONG zz_vcap_control_pack(const struct zz_vcap_control *control);
int zz_vcap_control_unpack(ULONG raw, struct zz_vcap_control *control);
void zz_vcap_effective_unpack(ULONG raw, UWORD *crop_h, UWORD *crop_v);
void zz_vcap_status_unpack(ULONG raw, struct zz_vcap_status *status);
UBYTE zz_vcap_next_sequence(UBYTE request_sequence);
int zz_vcap_request_complete(ULONG status, UBYTE expected_sequence);
int zz_vcap_request_result(ULONG status, UBYTE expected_sequence,
    ULONG applied_raw, ULONG expected_raw);
int zz_vcap_snapshot_status_valid(ULONG before, ULONG after);
UWORD zz_vcap_detect_standard(ULONG status, UWORD lines);
UWORD zz_vcap_calibration_standard(ULONG status, UWORD lines,
    UWORD caller_is_foreign);
int zz_vcap_adjust(struct zz_vcap_working *working, UWORD move,
    UWORD coarse);
void zz_vcap_accept(struct zz_vcap_working *working);
int zz_vcap_path_equal(const struct zz_vcap_path *a,
    const struct zz_vcap_path *b);
int zz_vcap_control_equal(const struct zz_vcap_control *a,
    const struct zz_vcap_control *b);
void zz_vcap_anchors_init(struct zz_vcap_anchors *anchors);
void zz_vcap_anchor_store(struct zz_vcap_anchors *anchors, UWORD owner,
    const struct zz_vcap_snapshot *snapshot);
int zz_vcap_anchor_load(const struct zz_vcap_anchors *anchors, UWORD owner,
    struct zz_vcap_snapshot *snapshot);
void zz_vcap_anchor_clear(struct zz_vcap_anchors *anchors, UWORD owner);

/* ---- capture-phase domain model ----
 *
 * The MMCM fine-phase engine is build-specific: a C28 bitstream
 * exposes 1792 steps per capture-clock turn (signed -896..895), a
 * legacy E7M one a quarter circle (448 steps, -255..255). The domain
 * comes from the phase capability register (0x1240); the two unit
 * systems are never converted into each other. */
enum zz_vcap_phase_domain {
    ZZ_VCAP_PHASE_NONE = 0,
    ZZ_VCAP_PHASE_E7M,
    ZZ_VCAP_PHASE_C28
};

enum zz_vcap_phase_domain zz_vcap_phase_domain(ULONG phase_capability);
int zz_vcap_phase_valid(int phase,
    enum zz_vcap_phase_domain domain);
UWORD zz_vcap_phase_encode(int phase);
int zz_vcap_phase_step(int phase, int delta,
    enum zz_vcap_phase_domain domain);

#endif /* ZZ_VCAP_LIVE_H */
