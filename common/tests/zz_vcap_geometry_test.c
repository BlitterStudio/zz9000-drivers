/* Exercises the firmware geometry transaction ABI through ordered 16-bit MMIO.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <string.h>
#include "zz_vcap_geometry.h"
#include "zzcfg_query.h"
#include "zz9000_hw.h"

static unsigned checks, failures;
#define CHECK(expr) do { ++checks; if (!(expr)) { ++failures; \
    printf("FAIL %u: %s\n", (unsigned)__LINE__, #expr); } } while (0)

enum completion {
    COMPLETE_DONE,
    COMPLETE_PENDING,
    COMPLETE_REJECT,
    COMPLETE_STALE,
    COMPLETE_CONFLICT
};

struct geometry_model {
    uint16_t value[35];
    uint16_t key, staged_width, feature;
    uint16_t resolved_width, resolved_height;
    unsigned queries, writes, delays, keep_calls, keep_until;
    unsigned present, completion;
    uint16_t capabilities;
};

static void model_complete(struct geometry_model *model)
{
    switch (model->completion) {
    case COMPLETE_DONE:
        model->value[ZZ_CFG_KEY_VCAP_GEOMETRY_APPLIED_WIDTH] =
            model->resolved_width;
        model->value[ZZ_CFG_KEY_VCAP_GEOMETRY_APPLIED_HEIGHT] =
            model->resolved_height;
        model->value[ZZ_CFG_KEY_VCAP_GEOMETRY_APPLIED_SERIAL] =
            model->value[ZZ_CFG_KEY_VCAP_GEOMETRY_REQUEST_SERIAL];
        model->value[ZZ_CFG_KEY_VCAP_GEOMETRY_STATUS] =
            ZZ_VCAP_GEOMETRY_STATUS_APPLIED_VALID;
        break;
    case COMPLETE_REJECT:
        model->value[ZZ_CFG_KEY_VCAP_GEOMETRY_STATUS] =
            ZZ_VCAP_GEOMETRY_STATUS_REJECTED;
        break;
    case COMPLETE_STALE:
        ++model->value[ZZ_CFG_KEY_VCAP_GEOMETRY_REQUEST_SERIAL];
        break;
    case COMPLETE_CONFLICT:
        model->value[ZZ_CFG_KEY_VCAP_GEOMETRY_REQUEST_WIDTH] += 16;
        model->value[ZZ_CFG_KEY_VCAP_GEOMETRY_APPLIED_SERIAL] =
            model->value[ZZ_CFG_KEY_VCAP_GEOMETRY_REQUEST_SERIAL];
        model->value[ZZ_CFG_KEY_VCAP_GEOMETRY_STATUS] =
            ZZ_VCAP_GEOMETRY_STATUS_APPLIED_VALID;
        break;
    default:
        break;
    }
}

static uint16_t model_read16(void *ctx, uint32_t offset)
{
    struct geometry_model *model = ctx;

    if (offset == ZZ_REG_FW_CAPABILITIES)
        return model->capabilities;
    if (offset == ZZ_REG_CONFIG_KEY)
        return model->value[model->key];
    if (offset == ZZ_REG_CONFIG_KEY + 2U)
        return model->present ? 1 : 0;
    return 0;
}

static void model_write16(void *ctx, uint32_t offset, uint16_t value)
{
    struct geometry_model *model = ctx;

    if (offset == ZZ_REG_CONFIG_KEY) {
        model->key = value;
        if (value >= ZZ_CFG_KEY_VCAP_GEOMETRY_REQUEST_WIDTH &&
            value <= ZZ_CFG_KEY_VCAP_GEOMETRY_STATUS)
            model->queries |= 1U << (value - ZZ_CFG_KEY_VCAP_GEOMETRY_REQUEST_WIDTH);
    } else if (offset == ZZ_REG_USER2) {
        model->staged_width = value;
    } else if (offset == ZZ_REG_USER1) {
        model->feature = value;
    } else if (offset == ZZ_REG_SET_FEATURE &&
        model->feature == ZZ_CARD_FEATURE_VIDEOCAP_GEOMETRY) {
        ++model->writes;
        model->value[ZZ_CFG_KEY_VCAP_GEOMETRY_REQUEST_WIDTH] = model->staged_width;
        model->value[ZZ_CFG_KEY_VCAP_GEOMETRY_REQUEST_HEIGHT] = value;
        ++model->value[ZZ_CFG_KEY_VCAP_GEOMETRY_REQUEST_SERIAL];
        /* Firmware accepts a fresh request before it is applied, which clears
         * any previous sticky rejection. */
        model->value[ZZ_CFG_KEY_VCAP_GEOMETRY_STATUS] =
            ZZ_VCAP_GEOMETRY_STATUS_PENDING;
    }
}

static void model_delay(void *ctx, unsigned ticks)
{
    struct geometry_model *model = ctx;
    (void)ticks;
    ++model->delays;
    if ((model->value[ZZ_CFG_KEY_VCAP_GEOMETRY_STATUS] &
            ZZ_VCAP_GEOMETRY_STATUS_PENDING) &&
        model->completion != COMPLETE_PENDING)
        model_complete(model);
}

static int model_keep_running(void *ctx)
{
    struct geometry_model *model = ctx;
    return ++model->keep_calls <= model->keep_until;
}

static struct zz_vcap_io model_io(struct geometry_model *model)
{
    struct zz_vcap_io io;
    io.ctx = model;
    io.read16 = model_read16;
    io.write16 = model_write16;
    io.delay = model_delay;
    io.control_ctx = model;
    io.keep_running = model_keep_running;
    return io;
}

static void model_init(struct geometry_model *model)
{
    memset(model, 0, sizeof(*model));
    model->capabilities = ZZ_FW_CAP_VIDEOCAP_GEOMETRY |
        ZZ_FW_CAP_VIDEOCAP_GEOMETRY_ACK;
    model->present = 1;
    model->completion = COMPLETE_DONE;
    model->keep_until = ~0U;
    model->value[ZZ_CFG_KEY_VCAP_GEOMETRY_STATUS] =
        ZZ_VCAP_GEOMETRY_STATUS_APPLIED_VALID;
}

static void model_state(struct geometry_model *model, uint16_t requested_width,
    uint16_t requested_height, uint16_t applied_width, uint16_t applied_height,
    uint16_t serial, uint16_t status)
{
    model->value[ZZ_CFG_KEY_VCAP_GEOMETRY_REQUEST_WIDTH] = requested_width;
    model->value[ZZ_CFG_KEY_VCAP_GEOMETRY_REQUEST_HEIGHT] = requested_height;
    model->value[ZZ_CFG_KEY_VCAP_GEOMETRY_APPLIED_WIDTH] = applied_width;
    model->value[ZZ_CFG_KEY_VCAP_GEOMETRY_APPLIED_HEIGHT] = applied_height;
    model->value[ZZ_CFG_KEY_VCAP_GEOMETRY_REQUEST_SERIAL] = serial;
    model->value[ZZ_CFG_KEY_VCAP_GEOMETRY_APPLIED_SERIAL] = serial;
    model->value[ZZ_CFG_KEY_VCAP_GEOMETRY_STATUS] = status;
}

static void test_query_abi_and_actual_readback(void)
{
    struct geometry_model model;
    struct zz_vcap_io io;
    struct zz_vcap_geometry_state state;

    model_init(&model);
    model_state(&model, 0, 0, 720, 576, 7,
        ZZ_VCAP_GEOMETRY_STATUS_APPLIED_VALID);
    io = model_io(&model);
    CHECK(zz_vcap_geometry_supported(&io));
    CHECK(zz_vcap_geometry_read(&io, &state) == ZZ_VCAP_OK);
    CHECK(model.queries == 0x7fU);
    CHECK(state.requested_width == 0 && state.requested_height == 0);
    CHECK(state.applied_width == 720 && state.applied_height == 576);
    CHECK(zz_vcap_geometry_check(&io, 0, 0) == ZZ_VCAP_OK);
    CHECK(zz_vcap_geometry_check(&io, 720, 576) == ZZ_VCAP_STATE_CHANGED);
    model.present = 0;
    CHECK(zz_vcap_geometry_read(&io, &state) == ZZ_VCAP_UNSUPPORTED);
    model.present = 1;
    model.capabilities &= ~ZZ_FW_CAP_VIDEOCAP_GEOMETRY_ACK;
    CHECK(!zz_vcap_geometry_supported(&io));
}

static void test_requested_pair_fastpath_and_resolved_apply(void)
{
    struct geometry_model model;
    struct zz_vcap_io io;

    model_init(&model);
    model_state(&model, 640, 0, 640, 576, 5,
        ZZ_VCAP_GEOMETRY_STATUS_APPLIED_VALID);
    io = model_io(&model);
    CHECK(zz_vcap_geometry_check(&io, 640, 0) == ZZ_VCAP_OK);
    CHECK(zz_vcap_geometry_apply(&io, 640, 0, 0) == ZZ_VCAP_OK);
    CHECK(model.writes == 0);

    model.resolved_width = 960;
    model.resolved_height = 480;
    CHECK(zz_vcap_geometry_apply(&io, 1280, 1024, 0) == ZZ_VCAP_OK);
    CHECK(model.writes == 1);
    CHECK(model.value[ZZ_CFG_KEY_VCAP_GEOMETRY_REQUEST_WIDTH] == 1280);
    CHECK(model.value[ZZ_CFG_KEY_VCAP_GEOMETRY_REQUEST_HEIGHT] == 1024);
    CHECK(model.value[ZZ_CFG_KEY_VCAP_GEOMETRY_APPLIED_WIDTH] == 960);
    CHECK(model.value[ZZ_CFG_KEY_VCAP_GEOMETRY_APPLIED_HEIGHT] == 480);
    CHECK(zz_vcap_geometry_check(&io, 1280, 1024) == ZZ_VCAP_OK);
}

static void test_pending_rejection_and_restore(void)
{
    struct geometry_model model;
    struct zz_vcap_io io;

    model_init(&model);
    model_state(&model, 640, 0, 640, 576, 4,
        ZZ_VCAP_GEOMETRY_STATUS_APPLIED_VALID | ZZ_VCAP_GEOMETRY_STATUS_PENDING);
    io = model_io(&model);
    CHECK(zz_vcap_geometry_check(&io, 640, 0) == ZZ_VCAP_BUSY);

    model.value[ZZ_CFG_KEY_VCAP_GEOMETRY_STATUS] =
        ZZ_VCAP_GEOMETRY_STATUS_REJECTED;
    CHECK(zz_vcap_geometry_check(&io, 640, 0) == ZZ_VCAP_REJECTED);
    model.resolved_width = 720;
    model.resolved_height = 576;
    CHECK(zz_vcap_geometry_apply(&io, 0, 0, 1) == ZZ_VCAP_OK);
    CHECK(model.writes == 1);
    CHECK(!(model.value[ZZ_CFG_KEY_VCAP_GEOMETRY_STATUS] &
        ZZ_VCAP_GEOMETRY_STATUS_REJECTED));

    model.completion = COMPLETE_REJECT;
    CHECK(zz_vcap_geometry_apply(&io, 640, 0, 0) == ZZ_VCAP_REJECTED);
}

static void test_timeout_cancellation_and_sequences(void)
{
    struct geometry_model model;
    struct zz_vcap_io io;

    model_init(&model);
    io = model_io(&model);
    model.completion = COMPLETE_PENDING;
    CHECK(zz_vcap_geometry_apply(&io, 640, 0, 0) == ZZ_VCAP_TIMEOUT);
    CHECK(model.writes == 1 && model.delays == 150);

    model.completion = COMPLETE_DONE;
    model.resolved_width = 720;
    model.resolved_height = 576;
    CHECK(zz_vcap_geometry_apply(&io, 0, 0, 1) == ZZ_VCAP_OK);
    CHECK(model.writes == 2);

    model_init(&model);
    io = model_io(&model);
    model.keep_until = 1;
    CHECK(zz_vcap_geometry_apply(&io, 640, 0, 0) == ZZ_VCAP_CANCELLED);
    CHECK(model.writes == 1);

    model_init(&model);
    io = model_io(&model);
    model.completion = COMPLETE_STALE;
    CHECK(zz_vcap_geometry_apply(&io, 640, 0, 0) == ZZ_VCAP_STATE_CHANGED);

    model_init(&model);
    io = model_io(&model);
    model.completion = COMPLETE_CONFLICT;
    CHECK(zz_vcap_geometry_apply(&io, 640, 0, 0) == ZZ_VCAP_STATE_CHANGED);

    model_init(&model);
    model_state(&model, 640, 0, 640, 576, 0xffffU,
        ZZ_VCAP_GEOMETRY_STATUS_APPLIED_VALID);
    model.resolved_width = 720;
    model.resolved_height = 576;
    io = model_io(&model);
    CHECK(zz_vcap_geometry_apply(&io, 0, 0, 0) == ZZ_VCAP_OK);
    CHECK(model.value[ZZ_CFG_KEY_VCAP_GEOMETRY_REQUEST_SERIAL] == 0);
    CHECK(model.value[ZZ_CFG_KEY_VCAP_GEOMETRY_APPLIED_SERIAL] == 0);
}

static void test_request_bounds(void)
{
    struct geometry_model model;
    struct zz_vcap_io io;

    model_init(&model);
    io = model_io(&model);
    CHECK(zz_vcap_geometry_apply(&io, 255, 0, 0) == ZZ_VCAP_INVALID);
    CHECK(zz_vcap_geometry_apply(&io, 257, 0, 0) == ZZ_VCAP_INVALID);
    CHECK(zz_vcap_geometry_apply(&io, 1296, 0, 0) == ZZ_VCAP_INVALID);
    CHECK(zz_vcap_geometry_apply(&io, 0, 99, 0) == ZZ_VCAP_INVALID);
    CHECK(zz_vcap_geometry_apply(&io, 0, 1025, 0) == ZZ_VCAP_INVALID);
    CHECK(model.writes == 0);
}

int main(void)
{
    test_query_abi_and_actual_readback();
    test_requested_pair_fastpath_and_resolved_apply();
    test_pending_rejection_and_restore();
    test_timeout_cancellation_and_sequences();
    test_request_bounds();
    if (failures) {
        printf("%u of %u checks failed\n", failures, checks);
        return 1;
    }
    printf("zz_vcap_geometry_test: %u checks passed\n", checks);
    return 0;
}
