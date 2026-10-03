/*
 * Firmware-owned native capture-window readback and acknowledgement.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ZZ_VCAP_GEOMETRY_H
#define ZZ_VCAP_GEOMETRY_H

#include "zz_vcap_client.h"

struct zz_vcap_geometry_state {
    uint16_t requested_width, requested_height;
    uint16_t applied_width, applied_height;
    uint16_t request_serial, applied_serial;
    uint16_t status;
};

int zz_vcap_geometry_supported(const struct zz_vcap_io *io);
enum zz_vcap_result zz_vcap_geometry_read(const struct zz_vcap_io *io,
    struct zz_vcap_geometry_state *state);
enum zz_vcap_result zz_vcap_geometry_check(const struct zz_vcap_io *io,
    uint16_t width, uint16_t height);
enum zz_vcap_result zz_vcap_geometry_apply(const struct zz_vcap_io *io,
    uint16_t width, uint16_t height, unsigned restoring);

#endif
