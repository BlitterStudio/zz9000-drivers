/*
 * Acknowledged native-video calibration register client.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ZZ_VCAP_CLIENT_H
#define ZZ_VCAP_CLIENT_H

#include <stdint.h>
#include "zz_vcap_live.h"

/* Ordered 16-bit MMIO, matching the existing FWUP injected-IO convention.
 * keep_running may service GUI cancellation/source ownership; restoration
 * ignores it so a cancelled experiment can still restore its entry state. */
struct zz_vcap_io {
    void *ctx;
    uint16_t (*read16)(void *ctx, uint32_t offset);
    void (*write16)(void *ctx, uint32_t offset, uint16_t value);
    void (*delay)(void *ctx, unsigned ticks);
    void *control_ctx;
    int (*keep_running)(void *control_ctx);
};

enum zz_vcap_result {
    ZZ_VCAP_OK = 0,
    ZZ_VCAP_UNSUPPORTED,
    ZZ_VCAP_INVALID,
    ZZ_VCAP_CLOCK_UNQUALIFIED,
    ZZ_VCAP_BUSY,
    ZZ_VCAP_ENGINE_ERROR,
    ZZ_VCAP_TIMEOUT,
    ZZ_VCAP_CANCELLED,
    ZZ_VCAP_STATE_CHANGED,
    ZZ_VCAP_UNSTABLE,
    ZZ_VCAP_REJECTED
};

struct zz_vcap_phase_state {
    enum zz_vcap_phase_domain domain;
    int applied;
    unsigned ready, busy, done, error;
    uint32_t clock_raw, phase_raw;
};

struct zz_vcap_capture_context {
    int phase;
    unsigned require_phase;
    unsigned ntsc, lace;
    unsigned have_geometry;
    uint32_t geometry;
};

/* Caller owns buffers; NULL skips the corresponding payload read. */
struct zz_vcap_capture_data {
    uint32_t status, geometry;
    uint32_t *pixels;
    uint32_t *metadata;
};

uint32_t zz_vcap_read32(const struct zz_vcap_io *io, uint32_t offset);
enum zz_vcap_result zz_vcap_read_stable(const struct zz_vcap_io *io,
    uint32_t offset, uint32_t *value);
enum zz_vcap_result zz_vcap_phase_read(const struct zz_vcap_io *io,
    struct zz_vcap_phase_state *state);
enum zz_vcap_result zz_vcap_phase_check(const struct zz_vcap_io *io,
    int target);
enum zz_vcap_result zz_vcap_phase_apply(const struct zz_vcap_io *io,
    int target, unsigned restoring);
enum zz_vcap_result zz_vcap_capture(const struct zz_vcap_io *io,
    struct zz_vcap_capture_context *context,
    struct zz_vcap_capture_data *data);
const char *zz_vcap_result_text(enum zz_vcap_result result);

#endif
