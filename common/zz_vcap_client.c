/*
 * Acknowledged native-video calibration register client.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "zz_vcap_client.h"
#include "zz9000_capture_calibration.h"

#define ZZ_VCAP_PHASE_WAIT_TICKS 250U
#define ZZ_VCAP_CAPTURE_WAIT_TICKS 100U
#define ZZ_VCAP_STABLE_ATTEMPTS 8U

static int zz_vcap_can_read(const struct zz_vcap_io *io)
{
    return io != 0 && io->read16 != 0;
}

static int zz_vcap_can_write(const struct zz_vcap_io *io)
{
    return zz_vcap_can_read(io) && io->write16 != 0;
}

static int zz_vcap_keep_running(const struct zz_vcap_io *io, unsigned restoring)
{
    return restoring || io->keep_running == 0 || io->keep_running(io->control_ctx);
}

static void zz_vcap_delay(const struct zz_vcap_io *io)
{
    if (io->delay != 0)
        io->delay(io->ctx, 1);
}

uint32_t zz_vcap_read32(const struct zz_vcap_io *io, uint32_t offset)
{
    uint32_t high;

    if (!zz_vcap_can_read(io))
        return 0;
    high = io->read16(io->ctx, offset);
    return high << 16 | io->read16(io->ctx, offset + 2U);
}

enum zz_vcap_result zz_vcap_read_stable(const struct zz_vcap_io *io,
    uint32_t offset, uint32_t *value)
{
    unsigned attempt;

    if (!zz_vcap_can_read(io) || value == 0)
        return ZZ_VCAP_INVALID;
    for (attempt = 0; attempt < ZZ_VCAP_STABLE_ATTEMPTS; ++attempt) {
        uint32_t first = zz_vcap_read32(io, offset);
        uint32_t second = zz_vcap_read32(io, offset);
        if (first == second) {
            *value = first;
            return ZZ_VCAP_OK;
        }
    }
    return ZZ_VCAP_UNSTABLE;
}

static int zz_vcap_clock_qualified(enum zz_vcap_phase_domain domain,
    uint32_t clocks)
{
    if (clocks & ZZ_CAPTURE_CLOCK_FAULT)
        return 0;
    if (domain == ZZ_VCAP_PHASE_E7M)
        return (clocks & ZZ_CAPTURE_CLOCK_READY) != 0;
    return (clocks & (ZZ_CAPTURE_CLOCK_FREQUENCY | ZZ_CAPTURE_CLOCK_LOCKED |
        ZZ_CAPTURE_CLOCK_READY | ZZ_CAPTURE_CLOCK_C28)) ==
        (ZZ_CAPTURE_CLOCK_FREQUENCY | ZZ_CAPTURE_CLOCK_LOCKED |
         ZZ_CAPTURE_CLOCK_READY | ZZ_CAPTURE_CLOCK_C28);
}

static int zz_vcap_phase_decode(enum zz_vcap_phase_domain domain,
    uint32_t raw)
{
    unsigned bits = domain == ZZ_VCAP_PHASE_E7M ? 10U : 12U;
    unsigned value = raw & ((1U << bits) - 1U);

    return value & (1U << (bits - 1U)) ?
        (int)value - (int)(1U << bits) : (int)value;
}

static unsigned zz_vcap_phase_busy(enum zz_vcap_phase_domain domain,
    uint32_t raw)
{
    return (raw >> (domain == ZZ_VCAP_PHASE_E7M ? 10U : 12U)) & 1U;
}

static unsigned zz_vcap_phase_done(enum zz_vcap_phase_domain domain,
    uint32_t raw)
{
    return (raw >> (domain == ZZ_VCAP_PHASE_E7M ? 11U : 13U)) & 1U;
}

static unsigned zz_vcap_phase_error(enum zz_vcap_phase_domain domain,
    uint32_t raw)
{
    return (raw >> (domain == ZZ_VCAP_PHASE_E7M ? 12U : 14U)) & 1U;
}

enum zz_vcap_result zz_vcap_phase_read(const struct zz_vcap_io *io,
    struct zz_vcap_phase_state *state)
{
    uint32_t capability;

    if (!zz_vcap_can_read(io) || state == 0)
        return ZZ_VCAP_INVALID;
    capability = zz_vcap_read32(io, ZZ_CAPTURE_CAP_REG);
    state->domain = zz_vcap_phase_domain(capability);
    if (state->domain == ZZ_VCAP_PHASE_NONE)
        return ZZ_VCAP_UNSUPPORTED;
    state->clock_raw = zz_vcap_read32(io, ZZ_CAPTURE_CLOCK_STATUS_REG);
    state->phase_raw = zz_vcap_read32(io, ZZ_CAPTURE_PHASE_STATUS_REG);
    state->applied = zz_vcap_phase_decode(state->domain, state->phase_raw);
    state->busy = zz_vcap_phase_busy(state->domain, state->phase_raw);
    state->done = zz_vcap_phase_done(state->domain, state->phase_raw);
    state->error = zz_vcap_phase_error(state->domain, state->phase_raw);
    state->ready = state->domain == ZZ_VCAP_PHASE_C28 ?
        (state->phase_raw >> 15) & 1U : 1U;
    return ZZ_VCAP_OK;
}

enum zz_vcap_result zz_vcap_phase_check(const struct zz_vcap_io *io,
    int target)
{
    struct zz_vcap_phase_state state;
    enum zz_vcap_result result = zz_vcap_phase_read(io, &state);

    if (result != ZZ_VCAP_OK)
        return result;
    if (!zz_vcap_phase_valid(target, state.domain))
        return ZZ_VCAP_INVALID;
    if (!zz_vcap_clock_qualified(state.domain, state.clock_raw))
        return ZZ_VCAP_CLOCK_UNQUALIFIED;
    if (state.busy)
        return ZZ_VCAP_BUSY;
    if (state.error)
        return ZZ_VCAP_ENGINE_ERROR;
    if (!state.done || !state.ready || state.applied != target)
        return ZZ_VCAP_STATE_CHANGED;
    return ZZ_VCAP_OK;
}

enum zz_vcap_result zz_vcap_phase_apply(const struct zz_vcap_io *io,
    int target, unsigned restoring)
{
    struct zz_vcap_phase_state state;
    enum zz_vcap_result result;
    unsigned tick;

    if (!zz_vcap_can_write(io))
        return ZZ_VCAP_INVALID;
    result = zz_vcap_phase_read(io, &state);
    if (result != ZZ_VCAP_OK)
        return result;
    if (!zz_vcap_phase_valid(target, state.domain))
        return ZZ_VCAP_INVALID;
    for (tick = 0; tick < ZZ_VCAP_PHASE_WAIT_TICKS; ++tick) {
        result = zz_vcap_phase_read(io, &state);
        if (result != ZZ_VCAP_OK)
            return result;
        if (!state.busy)
            break;
        if (!zz_vcap_keep_running(io, restoring))
            return ZZ_VCAP_CANCELLED;
        zz_vcap_delay(io);
    }
    if (tick == ZZ_VCAP_PHASE_WAIT_TICKS)
        return ZZ_VCAP_TIMEOUT;
    if (!zz_vcap_clock_qualified(state.domain, state.clock_raw))
        return ZZ_VCAP_CLOCK_UNQUALIFIED;
    if (state.error && !restoring)
        return ZZ_VCAP_ENGINE_ERROR;
    io->write16(io->ctx, ZZ_CAPTURE_PHASE_TARGET_REG,
        zz_vcap_phase_encode(target));
    io->write16(io->ctx, ZZ_CAPTURE_PHASE_COMMIT_REG, ZZ_CAPTURE_PHASE_TOKEN);
    zz_vcap_delay(io);
    for (tick = 0; tick < ZZ_VCAP_PHASE_WAIT_TICKS; ++tick) {
        result = zz_vcap_phase_read(io, &state);
        if (result != ZZ_VCAP_OK)
            return result;
        if (state.done && state.ready && !state.busy && !state.error &&
            state.applied == target)
            return zz_vcap_clock_qualified(state.domain, state.clock_raw) ?
                ZZ_VCAP_OK : ZZ_VCAP_CLOCK_UNQUALIFIED;
        if (!zz_vcap_keep_running(io, restoring))
            return ZZ_VCAP_CANCELLED;
        zz_vcap_delay(io);
    }
    return ZZ_VCAP_TIMEOUT;
}

static enum zz_vcap_result zz_vcap_capture_capability(const struct zz_vcap_io *io,
    enum zz_vcap_phase_domain *domain)
{
    uint32_t capability;

    if (!zz_vcap_can_read(io))
        return ZZ_VCAP_INVALID;
    capability = zz_vcap_read32(io, ZZ_CAPTURE_CAP_REG);
    *domain = zz_vcap_phase_domain(capability);
    if (*domain == ZZ_VCAP_PHASE_NONE ||
        zz_vcap_read32(io, ZZ_CAPTURE_METADATA_CAP_REG) != ZZ_CAPTURE_METADATA_CAP)
        return ZZ_VCAP_UNSUPPORTED;
    return ZZ_VCAP_OK;
}

enum zz_vcap_result zz_vcap_capture(const struct zz_vcap_io *io,
    struct zz_vcap_capture_context *context,
    struct zz_vcap_capture_data *data)
{
    enum zz_vcap_phase_domain domain;
    enum zz_vcap_result result;
    uint32_t before, status, after, geometry;
    unsigned tick, i;

    if (!zz_vcap_can_write(io) || context == 0 || data == 0)
        return ZZ_VCAP_INVALID;
    result = zz_vcap_capture_capability(io, &domain);
    if (result != ZZ_VCAP_OK)
        return result;
    if (context->require_phase) {
        if (!zz_vcap_phase_valid(context->phase, domain))
            return ZZ_VCAP_INVALID;
        result = zz_vcap_phase_check(io, context->phase);
        if (result != ZZ_VCAP_OK)
            return result;
    }
    result = zz_vcap_read_stable(io, ZZ_CAPTURE_SNAPSHOT_REG, &before);
    if (result != ZZ_VCAP_OK)
        return result;
    if (before & ZZ_CAPTURE_SNAPSHOT_BUSY)
        return ZZ_VCAP_BUSY;

    io->write16(io->ctx, ZZ_CAPTURE_ARM_REG, ZZ_CAPTURE_ARM_TOKEN);
    for (tick = 0; tick < ZZ_VCAP_CAPTURE_WAIT_TICKS; ++tick) {
        if (!zz_vcap_keep_running(io, 0))
            return ZZ_VCAP_CANCELLED;
        if (context->require_phase) {
            result = zz_vcap_phase_check(io, context->phase);
            if (result != ZZ_VCAP_OK)
                return result;
        }
        result = zz_vcap_read_stable(io, ZZ_CAPTURE_SNAPSHOT_REG, &status);
        if (result != ZZ_VCAP_OK)
            return result;
        if ((status & (ZZ_CAPTURE_SNAPSHOT_VALID | ZZ_CAPTURE_SNAPSHOT_BUSY)) ==
                ZZ_CAPTURE_SNAPSHOT_VALID &&
            ((status ^ before) & ZZ_CAPTURE_SNAPSHOT_ARM) != 0 &&
            (!(before & ZZ_CAPTURE_SNAPSHOT_VALID) || (status >> 16) != (before >> 16)))
            break;
        zz_vcap_delay(io);
    }
    if (tick == ZZ_VCAP_CAPTURE_WAIT_TICKS)
        return ZZ_VCAP_TIMEOUT;

    geometry = zz_vcap_read32(io, ZZ_CAPTURE_GEOMETRY_REG);
    if (context->require_phase &&
        (!!(status & ZZ_CAPTURE_SNAPSHOT_NTSC) != context->ntsc ||
         !!(status & ZZ_CAPTURE_SNAPSHOT_LACE) != context->lace))
        return ZZ_VCAP_STATE_CHANGED;
    if (context->have_geometry && geometry != context->geometry)
        return ZZ_VCAP_STATE_CHANGED;

    data->status = status;
    data->geometry = geometry;
    if (data->pixels != 0) {
        for (i = 0; i < ZZ_CAPTURE_SAMPLES; ++i) {
            io->write16(io->ctx, ZZ_CAPTURE_ADDRESS_REG, (uint16_t)i);
            data->pixels[i] = zz_vcap_read32(io, ZZ_CAPTURE_DATA_REG);
        }
    }
    if (data->metadata != 0) {
        for (i = 0; i < ZZ_CAPTURE_METADATA_WORDS; ++i) {
            io->write16(io->ctx, ZZ_CAPTURE_METADATA_ADDR_REG, (uint16_t)i);
            data->metadata[i] = zz_vcap_read32(io, ZZ_CAPTURE_METADATA_DATA_REG);
        }
    }
    result = zz_vcap_read_stable(io, ZZ_CAPTURE_SNAPSHOT_REG, &after);
    if (result != ZZ_VCAP_OK)
        return result;
    if (context->require_phase) {
        result = zz_vcap_phase_check(io, context->phase);
        if (result != ZZ_VCAP_OK)
            return result;
    }
    if (after != status || zz_vcap_read32(io, ZZ_CAPTURE_GEOMETRY_REG) != geometry)
        return ZZ_VCAP_STATE_CHANGED;
    if (!context->have_geometry) {
        context->geometry = geometry;
        context->have_geometry = 1;
    }
    return ZZ_VCAP_OK;
}

const char *zz_vcap_result_text(enum zz_vcap_result result)
{
    switch (result) {
    case ZZ_VCAP_OK: return "ok";
    case ZZ_VCAP_UNSUPPORTED: return "unsupported capture protocol";
    case ZZ_VCAP_INVALID: return "invalid capture request";
    case ZZ_VCAP_CLOCK_UNQUALIFIED: return "capture clock is not qualified";
    case ZZ_VCAP_BUSY: return "capture controller is busy";
    case ZZ_VCAP_ENGINE_ERROR: return "capture controller reported an error";
    case ZZ_VCAP_TIMEOUT: return "capture controller timed out";
    case ZZ_VCAP_CANCELLED: return "capture operation cancelled";
    case ZZ_VCAP_STATE_CHANGED: return "capture source state changed";
    case ZZ_VCAP_UNSTABLE: return "capture register read was unstable";
    case ZZ_VCAP_REJECTED: return "capture request was rejected";
    }
    return "unknown capture error";
}
