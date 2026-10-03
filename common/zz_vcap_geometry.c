/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "zz_vcap_geometry.h"
#include "zzcfg_query.h"
#include "zz9000_hw.h"

#define GEOMETRY_WAIT_TICKS 150U

int zz_vcap_geometry_supported(const struct zz_vcap_io *io)
{
    const unsigned required = ZZ_FW_CAP_VIDEOCAP_GEOMETRY |
        ZZ_FW_CAP_VIDEOCAP_GEOMETRY_ACK;
    return (io->read16(io->ctx, ZZ_REG_FW_CAPABILITIES) & required) == required;
}

static int query(const struct zz_vcap_io *io, uint16_t key, uint16_t *value)
{
    uint32_t raw;
    io->write16(io->ctx, ZZ_REG_CONFIG_KEY, key);
    raw = zz_vcap_read32(io, ZZ_REG_CONFIG_KEY);
    if ((raw & 0xffffU) != 1U) return 0;
    *value = (uint16_t)(raw >> 16);
    return 1;
}

enum zz_vcap_result zz_vcap_geometry_read(const struct zz_vcap_io *io,
    struct zz_vcap_geometry_state *state)
{
    unsigned attempt;
    if (!zz_vcap_geometry_supported(io)) return ZZ_VCAP_UNSUPPORTED;
    for (attempt = 0; attempt < 8; ++attempt) {
        uint16_t request_before, applied_before, status_before;
        if (!query(io, ZZ_CFG_KEY_VCAP_GEOMETRY_REQUEST_SERIAL, &request_before) ||
            !query(io, ZZ_CFG_KEY_VCAP_GEOMETRY_APPLIED_SERIAL, &applied_before) ||
            !query(io, ZZ_CFG_KEY_VCAP_GEOMETRY_STATUS, &status_before) ||
            !query(io, ZZ_CFG_KEY_VCAP_GEOMETRY_REQUEST_WIDTH, &state->requested_width) ||
            !query(io, ZZ_CFG_KEY_VCAP_GEOMETRY_REQUEST_HEIGHT, &state->requested_height) ||
            !query(io, ZZ_CFG_KEY_VCAP_GEOMETRY_APPLIED_WIDTH, &state->applied_width) ||
            !query(io, ZZ_CFG_KEY_VCAP_GEOMETRY_APPLIED_HEIGHT, &state->applied_height) ||
            !query(io, ZZ_CFG_KEY_VCAP_GEOMETRY_REQUEST_SERIAL, &state->request_serial) ||
            !query(io, ZZ_CFG_KEY_VCAP_GEOMETRY_APPLIED_SERIAL, &state->applied_serial) ||
            !query(io, ZZ_CFG_KEY_VCAP_GEOMETRY_STATUS, &state->status))
            return ZZ_VCAP_UNSUPPORTED;
        if (request_before == state->request_serial &&
            applied_before == state->applied_serial && status_before == state->status)
            return ZZ_VCAP_OK;
    }
    return ZZ_VCAP_UNSTABLE;
}

enum zz_vcap_result zz_vcap_geometry_check(const struct zz_vcap_io *io,
    uint16_t width, uint16_t height)
{
    struct zz_vcap_geometry_state state;
    enum zz_vcap_result result = zz_vcap_geometry_read(io, &state);
    if (result != ZZ_VCAP_OK) return result;
    if (state.status & ZZ_VCAP_GEOMETRY_STATUS_REJECTED) return ZZ_VCAP_REJECTED;
    if (!(state.status & ZZ_VCAP_GEOMETRY_STATUS_APPLIED_VALID) ||
        (state.status & ZZ_VCAP_GEOMETRY_STATUS_PENDING) ||
        state.request_serial != state.applied_serial) return ZZ_VCAP_BUSY;
    return state.requested_width == width && state.requested_height == height ?
        ZZ_VCAP_OK : ZZ_VCAP_STATE_CHANGED;
}

enum zz_vcap_result zz_vcap_geometry_apply(const struct zz_vcap_io *io,
    uint16_t width, uint16_t height, unsigned restoring)
{
    struct zz_vcap_geometry_state before, state;
    enum zz_vcap_result result;
    uint16_t expected;
    unsigned tick;
    if ((width && (width < 256 || width > 1280 || (width & 15))) ||
        (height && (height < 100 || height > 1024))) return ZZ_VCAP_INVALID;
    result = zz_vcap_geometry_read(io, &before);
    if (result != ZZ_VCAP_OK) return result;
    if (!restoring && io->keep_running && !io->keep_running(io->control_ctx))
        return ZZ_VCAP_CANCELLED;
    if ((before.status & ZZ_VCAP_GEOMETRY_STATUS_APPLIED_VALID) &&
        !(before.status & (ZZ_VCAP_GEOMETRY_STATUS_PENDING | ZZ_VCAP_GEOMETRY_STATUS_REJECTED)) &&
        before.request_serial == before.applied_serial &&
        before.requested_width == width && before.requested_height == height)
        return ZZ_VCAP_OK;
    expected = (uint16_t)(before.request_serial + 1U);
    io->write16(io->ctx, ZZ_REG_USER2, width);
    io->write16(io->ctx, ZZ_REG_USER1, ZZ_CARD_FEATURE_VIDEOCAP_GEOMETRY);
    io->write16(io->ctx, ZZ_REG_SET_FEATURE, height);
    for (tick = 0; tick < GEOMETRY_WAIT_TICKS; ++tick) {
        if (!restoring && io->keep_running && !io->keep_running(io->control_ctx))
            return ZZ_VCAP_CANCELLED;
        result = zz_vcap_geometry_read(io, &state);
        if (result != ZZ_VCAP_OK) return result;
        if (state.status & ZZ_VCAP_GEOMETRY_STATUS_REJECTED) return ZZ_VCAP_REJECTED;
        if (state.request_serial != before.request_serial && state.request_serial != expected)
            return ZZ_VCAP_STATE_CHANGED;
        if (state.request_serial == expected && state.applied_serial == expected &&
            (state.status & ZZ_VCAP_GEOMETRY_STATUS_APPLIED_VALID) &&
            !(state.status & ZZ_VCAP_GEOMETRY_STATUS_PENDING))
            return state.requested_width == width && state.requested_height == height ?
                ZZ_VCAP_OK : ZZ_VCAP_STATE_CHANGED;
        io->delay(io->ctx, 1);
    }
    return ZZ_VCAP_TIMEOUT;
}
