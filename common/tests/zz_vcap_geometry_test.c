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
    uint16_t value[ZZ_CFG_KEY_VCAP_GEOMETRY_REGS + 1];
    uint16_t key, staged_width, feature;
    uint16_t resolved_width, resolved_height;
    unsigned queries, writes, delays, keep_calls, keep_until;
    unsigned present, completion;
    uint16_t capabilities;
    /* ZZ_CFG_KEY_VCAP_GEOMETRY_REGS answer; 0 models 2.9.0 (key absent). */
    uint16_t regs_abi;
    unsigned reg_staged;
    uint16_t reg_width;
    unsigned locked, locks, unlocks, unlocked_writes;
    /* Every write to a geometry command register, in order. */
    uint32_t log_offset[8];
    uint16_t log_value[8];
    unsigned log_len;
};

/* Firmware generation the suite runs against; main() runs it for both. */
static uint16_t firmware_regs_abi;

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
    if (offset == ZZ_REG_CONFIG_KEY + 2U) {
        if (model->key == ZZ_CFG_KEY_VCAP_GEOMETRY_REGS)
            return model->regs_abi ? 1 : 0;
        return model->present ? 1 : 0;
    }
    return 0;
}

static int command_register(uint32_t offset)
{
    return offset == ZZ_REG_USER1 || offset == ZZ_REG_USER2 ||
        offset == ZZ_REG_SET_FEATURE || offset == ZZ_REG_VCAP_GEOMETRY_WIDTH ||
        offset == ZZ_REG_VCAP_GEOMETRY_COMMIT;
}

static void model_accept(struct geometry_model *model, uint16_t width,
    uint16_t height)
{
    ++model->writes;
    model->value[ZZ_CFG_KEY_VCAP_GEOMETRY_REQUEST_WIDTH] = width;
    model->value[ZZ_CFG_KEY_VCAP_GEOMETRY_REQUEST_HEIGHT] = height;
    ++model->value[ZZ_CFG_KEY_VCAP_GEOMETRY_REQUEST_SERIAL];
    /* Firmware accepts a fresh request before it is applied, which clears
     * any previous sticky rejection. */
    model->value[ZZ_CFG_KEY_VCAP_GEOMETRY_STATUS] =
        ZZ_VCAP_GEOMETRY_STATUS_PENDING;
}

static void model_write16(void *ctx, uint32_t offset, uint16_t value)
{
    struct geometry_model *model = ctx;

    if (command_register(offset)) {
        if (!model->locked)
            ++model->unlocked_writes;
        if (model->log_len < 8U) {
            model->log_offset[model->log_len] = offset;
            model->log_value[model->log_len] = value;
        }
        ++model->log_len;
    }
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
        model_accept(model, model->staged_width, value);
    } else if (offset == ZZ_REG_VCAP_GEOMETRY_WIDTH && model->regs_abi) {
        model->reg_width = value;
        model->reg_staged = 1;
    } else if (offset == ZZ_REG_VCAP_GEOMETRY_COMMIT && model->regs_abi) {
        /* A staged width serves one commit; a bare commit is rejected. */
        if (model->reg_staged)
            model_accept(model, model->reg_width, value);
        else
            model->value[ZZ_CFG_KEY_VCAP_GEOMETRY_STATUS] |=
                ZZ_VCAP_GEOMETRY_STATUS_REJECTED;
        model->reg_staged = 0;
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

static void model_lock(void *ctx)
{
    struct geometry_model *model = ctx;
    CHECK(!model->locked);
    model->locked = 1;
    ++model->locks;
}

static void model_unlock(void *ctx)
{
    struct geometry_model *model = ctx;
    CHECK(model->locked);
    model->locked = 0;
    ++model->unlocks;
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
    io.lock = model_lock;
    io.unlock = model_unlock;
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
    model->regs_abi = firmware_regs_abi;
    model->value[ZZ_CFG_KEY_VCAP_GEOMETRY_REGS] = firmware_regs_abi;
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

static void check_write(const struct geometry_model *model, unsigned at,
    uint32_t offset, uint16_t value)
{
    CHECK(model->log_len > at && at < 8U);
    if (model->log_len > at && at < 8U)
        CHECK(model->log_offset[at] == offset && model->log_value[at] == value);
}

/* Pins the bus sequence for each firmware generation: the dedicated registers
 * when the firmware reports them, the 2.9.0 feature sequence otherwise, each
 * request under one lock. */
static void test_write_sequence(void)
{
    struct geometry_model model;
    struct zz_vcap_io io;
    const int registers = firmware_regs_abi == ZZ_CFG_VCAP_GEOMETRY_REGS_ABI;

    model_init(&model);
    model.resolved_width = 640;
    model.resolved_height = 512;
    io = model_io(&model);
    CHECK(zz_vcap_geometry_apply(&io, 640, 512, 0) == ZZ_VCAP_OK);
    CHECK(model.writes == 1);
    CHECK(model.locks == 1 && model.unlocks == 1 && !model.locked);
    CHECK(model.unlocked_writes == 0);
    if (registers) {
        CHECK(model.log_len == 2);
        check_write(&model, 0, ZZ_REG_VCAP_GEOMETRY_WIDTH, 640);
        check_write(&model, 1, ZZ_REG_VCAP_GEOMETRY_COMMIT, 512);
    } else {
        CHECK(model.log_len == 3);
        check_write(&model, 0, ZZ_REG_USER2, 640);
        check_write(&model, 1, ZZ_REG_USER1, ZZ_CARD_FEATURE_VIDEOCAP_GEOMETRY);
        check_write(&model, 2, ZZ_REG_SET_FEATURE, 512);
    }

    /* Reset auto / Revert restore through the same path. */
    model.log_len = 0;
    model.resolved_width = 720;
    model.resolved_height = 576;
    CHECK(zz_vcap_geometry_apply(&io, 0, 0, 1) == ZZ_VCAP_OK);
    CHECK(model.writes == 2);
    CHECK(model.locks == 2 && model.unlocks == 2 && !model.locked);
    CHECK(model.unlocked_writes == 0);
    if (registers) {
        CHECK(model.log_len == 2);
        check_write(&model, 0, ZZ_REG_VCAP_GEOMETRY_WIDTH, 0);
        check_write(&model, 1, ZZ_REG_VCAP_GEOMETRY_COMMIT, 0);
    } else {
        CHECK(model.log_len == 3);
        check_write(&model, 0, ZZ_REG_USER2, 0);
        check_write(&model, 1, ZZ_REG_USER1, ZZ_CARD_FEATURE_VIDEOCAP_GEOMETRY);
        check_write(&model, 2, ZZ_REG_SET_FEATURE, 0);
    }

    /* Callers without lock hooks still issue the same request. */
    model.log_len = 0;
    io.lock = 0;
    io.unlock = 0;
    CHECK(zz_vcap_geometry_apply(&io, 656, 400, 0) == ZZ_VCAP_OK);
    CHECK(model.writes == 3);
    CHECK(model.log_len == (registers ? 2U : 3U));
    CHECK(model.unlocked_writes == model.log_len);
}

/* A register ABI the client does not know keeps it on the 2.9.0 sequence. */
static void test_unknown_register_abi(void)
{
    struct geometry_model model;
    struct zz_vcap_io io;

    model_init(&model);
    model.regs_abi = (uint16_t)(ZZ_CFG_VCAP_GEOMETRY_REGS_ABI + 1U);
    model.value[ZZ_CFG_KEY_VCAP_GEOMETRY_REGS] = model.regs_abi;
    model.resolved_width = 640;
    model.resolved_height = 512;
    io = model_io(&model);
    CHECK(zz_vcap_geometry_apply(&io, 640, 512, 0) == ZZ_VCAP_OK);
    CHECK(model.log_len == 3);
    check_write(&model, 0, ZZ_REG_USER2, 640);
    check_write(&model, 2, ZZ_REG_SET_FEATURE, 512);
}

/* The firmware decode the client relies on: a staged width serves one commit,
 * a bare commit is rejected without a new request. */
static void test_bare_commit_rejected(void)
{
    struct geometry_model model;

    model_init(&model);
    model.regs_abi = ZZ_CFG_VCAP_GEOMETRY_REGS_ABI;
    model_write16(&model, ZZ_REG_VCAP_GEOMETRY_WIDTH, 640);
    model_write16(&model, ZZ_REG_VCAP_GEOMETRY_COMMIT, 512);
    CHECK(model.writes == 1);
    model_write16(&model, ZZ_REG_VCAP_GEOMETRY_COMMIT, 400);
    CHECK(model.writes == 1);
    CHECK(model.value[ZZ_CFG_KEY_VCAP_GEOMETRY_STATUS] &
        ZZ_VCAP_GEOMETRY_STATUS_REJECTED);
    CHECK(model.value[ZZ_CFG_KEY_VCAP_GEOMETRY_REQUEST_HEIGHT] == 512);
}

int main(void)
{
    static const uint16_t generations[] = {
        ZZ_CFG_VCAP_GEOMETRY_REGS_ABI, /* dedicated registers */
        0                              /* 2.9.0: feature sequence only */
    };
    unsigned i;

    for (i = 0; i < sizeof(generations) / sizeof(generations[0]); ++i) {
        firmware_regs_abi = generations[i];
        test_query_abi_and_actual_readback();
        test_requested_pair_fastpath_and_resolved_apply();
        test_pending_rejection_and_restore();
        test_timeout_cancellation_and_sequences();
        test_request_bounds();
        test_write_sequence();
    }
    test_unknown_register_abi();
    test_bare_commit_rejected();
    if (failures) {
        printf("%u of %u checks failed\n", failures, checks);
        return 1;
    }
    printf("zz_vcap_geometry_test: %u checks passed\n", checks);
    return 0;
}
