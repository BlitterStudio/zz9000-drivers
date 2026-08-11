/*
 * Pure state model for acknowledged ZZ9000 native-video calibration.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "zz_vcap_live.h"

int zz_vcap_live_supported(UWORD firmware_revision, ULONG capability)
{
    return firmware_revision >= ZZ_VCAP_LIVE_MIN_FW &&
        capability == ZZ_VCAP_LIVE_CAPABILITY_VALUE;
}

int zz_vcap_control_valid(const struct zz_vcap_control *control)
{
    return control != 0 && control->sample <= 2 &&
        control->full_width <= 1 && control->crop_h <= ZZ_VCAP_CROP_MAX &&
        control->crop_v <= ZZ_VCAP_CROP_MAX &&
        control->crop_h_present <= 1 && control->crop_v_present <= 1;
}

ULONG zz_vcap_control_pack(const struct zz_vcap_control *control)
{
    ULONG crop_h;
    ULONG crop_v;
    ULONG raw;

    if (!zz_vcap_control_valid(control))
        return 0;

    crop_h = control->crop_h_present ? control->crop_h :
        ZZ_VCAP_CROP_H_COMPAT;
    crop_v = control->crop_v_present ? control->crop_v :
        ZZ_VCAP_CROP_V_COMPAT;
    raw = (crop_v << 16) | (crop_h << 4) |
        ((ULONG)control->full_width << 2) | control->sample;
    if (!control->crop_h_present)
        raw |= ZZ_VCAP_CROP_H_AUTO_FLAG;
    if (!control->crop_v_present)
        raw |= ZZ_VCAP_CROP_V_AUTO_FLAG;
    return raw;
}

int zz_vcap_control_unpack(ULONG raw, struct zz_vcap_control *control)
{
    if (control == 0 || (raw & 0xc0000000UL) != 0 ||
        (raw & 3UL) > 2)
        return 0;

    control->sample = (UWORD)(raw & 3UL);
    control->full_width = (UWORD)((raw >> 2) & 1UL);
    control->crop_h = (UWORD)((raw >> 4) & 0x0fffUL);
    control->crop_v = (UWORD)((raw >> 16) & 0x0fffUL);
    control->crop_h_present =
        (raw & ZZ_VCAP_CROP_H_AUTO_FLAG) == 0;
    control->crop_v_present =
        (raw & ZZ_VCAP_CROP_V_AUTO_FLAG) == 0;
    return 1;
}

void zz_vcap_effective_unpack(ULONG raw, UWORD *crop_h, UWORD *crop_v)
{
    if (crop_h != 0)
        *crop_h = (UWORD)(raw & 0x0fffUL);
    if (crop_v != 0)
        *crop_v = (UWORD)((raw >> 16) & 0x0fffUL);
}

void zz_vcap_status_unpack(ULONG raw, struct zz_vcap_status *status)
{
    if (status == 0)
        return;
    status->request_sequence =
        (UBYTE)((raw >> ZZ_VCAP_STATUS_REQUEST_SHIFT) & 0xffUL);
    status->applied_sequence =
        (UBYTE)((raw >> ZZ_VCAP_STATUS_APPLIED_SHIFT) & 0xffUL);
    status->standard_valid =
        (raw & ZZ_VCAP_STATUS_STANDARD_VALID) != 0;
    status->ntsc = (raw & ZZ_VCAP_STATUS_NTSC) != 0;
    status->rejected = (raw & ZZ_VCAP_STATUS_REJECTED) != 0;
    status->applied_valid =
        (raw & ZZ_VCAP_STATUS_APPLIED_VALID) != 0;
    status->busy = (raw & ZZ_VCAP_STATUS_BUSY) != 0;
}

UBYTE zz_vcap_next_sequence(UBYTE request_sequence)
{
    return (UBYTE)(request_sequence + 1);
}

int zz_vcap_request_complete(ULONG raw, UBYTE expected_sequence)
{
    struct zz_vcap_status status;

    zz_vcap_status_unpack(raw, &status);
    return status.request_sequence == expected_sequence &&
        status.applied_sequence == expected_sequence &&
        status.applied_valid && !status.busy && !status.rejected;
}

int zz_vcap_snapshot_status_valid(ULONG before, ULONG after)
{
    struct zz_vcap_status status;

    if (before != after)
        return 0;
    zz_vcap_status_unpack(before, &status);
    return status.applied_valid && status.standard_valid && !status.busy &&
        !status.rejected &&
        status.request_sequence == status.applied_sequence;
}

static UWORD zz_vcap_add_clamped(UWORD value, UWORD amount)
{
    if (value >= ZZ_VCAP_CROP_MAX - amount)
        return ZZ_VCAP_CROP_MAX;
    return (UWORD)(value + amount);
}

static UWORD zz_vcap_sub_clamped(UWORD value, UWORD amount)
{
    if (value <= amount)
        return 0;
    return (UWORD)(value - amount);
}

int zz_vcap_adjust(struct zz_vcap_working *working, UWORD move,
    UWORD coarse)
{
    UWORD crop_h;
    UWORD crop_v;
    UWORD amount;

    if (working == 0 || !zz_vcap_control_valid(&working->control) ||
        working->crop_h > ZZ_VCAP_CROP_MAX ||
        working->crop_v > ZZ_VCAP_CROP_MAX)
        return 0;

    crop_h = working->crop_h;
    crop_v = working->crop_v;
    amount = coarse ? 16 : 1;
    switch (move) {
    case ZZ_VCAP_MOVE_LEFT:
        crop_h = zz_vcap_add_clamped(crop_h, amount);
        break;
    case ZZ_VCAP_MOVE_RIGHT:
        crop_h = zz_vcap_sub_clamped(crop_h, amount);
        break;
    case ZZ_VCAP_MOVE_UP:
        crop_v = zz_vcap_add_clamped(crop_v, amount);
        break;
    case ZZ_VCAP_MOVE_DOWN:
        crop_v = zz_vcap_sub_clamped(crop_v, amount);
        break;
    default:
        return 0;
    }
    if (crop_h == working->crop_h && crop_v == working->crop_v)
        return 0;

    working->crop_h = crop_h;
    working->crop_v = crop_v;
    zz_vcap_accept(working);
    return 1;
}

void zz_vcap_accept(struct zz_vcap_working *working)
{
    if (working == 0)
        return;
    working->control.crop_h = working->crop_h;
    working->control.crop_v = working->crop_v;
    working->control.crop_h_present = 1;
    working->control.crop_v_present = 1;
}

int zz_vcap_path_equal(const struct zz_vcap_path *a,
    const struct zz_vcap_path *b)
{
    return a != 0 && b != 0 && a->sample == b->sample &&
        a->full_width == b->full_width;
}

int zz_vcap_control_equal(const struct zz_vcap_control *a,
    const struct zz_vcap_control *b)
{
    return a != 0 && b != 0 && a->sample == b->sample &&
        a->full_width == b->full_width && a->crop_h == b->crop_h &&
        a->crop_v == b->crop_v &&
        a->crop_h_present == b->crop_h_present &&
        a->crop_v_present == b->crop_v_present;
}

void zz_vcap_anchors_init(struct zz_vcap_anchors *anchors)
{
    UWORD owner;

    if (anchors == 0)
        return;
    for (owner = 0; owner < ZZ_VCAP_ANCHOR_COUNT; owner++)
        anchors->valid[owner] = 0;
}

void zz_vcap_anchor_store(struct zz_vcap_anchors *anchors, UWORD owner,
    const struct zz_vcap_snapshot *snapshot)
{
    if (anchors == 0 || snapshot == 0 || owner >= ZZ_VCAP_ANCHOR_COUNT)
        return;
    anchors->value[owner] = *snapshot;
    anchors->valid[owner] = 1;
}

int zz_vcap_anchor_load(const struct zz_vcap_anchors *anchors, UWORD owner,
    struct zz_vcap_snapshot *snapshot)
{
    if (anchors == 0 || snapshot == 0 || owner >= ZZ_VCAP_ANCHOR_COUNT ||
        !anchors->valid[owner])
        return 0;
    *snapshot = anchors->value[owner];
    return 1;
}

void zz_vcap_anchor_clear(struct zz_vcap_anchors *anchors, UWORD owner)
{
    if (anchors != 0 && owner < ZZ_VCAP_ANCHOR_COUNT)
        anchors->valid[owner] = 0;
}
