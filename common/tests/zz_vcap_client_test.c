/* Exercises the real ordered MMIO client with a register-level fixture.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <string.h>
#include "zz_vcap_client.h"
#include "zz9000_capture_calibration.h"

static unsigned checks, failures;
#define CHECK(expr) do { ++checks; if (!(expr)) { ++failures; \
    printf("FAIL %u: %s\n", (unsigned)__LINE__, #expr); } } while (0)

struct mock {
    uint32_t cap, metadata_cap, clocks, phase, snapshot, geometry;
    uint16_t target;
    unsigned commits, arms, delays, pixel_reads, metadata_reads;
    unsigned ack_after, capture_after;
    int cancel, change_geometry;
};

static uint32_t *mock_register(struct mock *mock, uint32_t offset)
{
    switch (offset) {
    case ZZ_CAPTURE_CAP_REG: return &mock->cap;
    case ZZ_CAPTURE_METADATA_CAP_REG: return &mock->metadata_cap;
    case ZZ_CAPTURE_CLOCK_STATUS_REG: return &mock->clocks;
    case ZZ_CAPTURE_PHASE_STATUS_REG: return &mock->phase;
    case ZZ_CAPTURE_SNAPSHOT_REG: return &mock->snapshot;
    case ZZ_CAPTURE_GEOMETRY_REG: return &mock->geometry;
    default: return 0;
    }
}

static uint16_t mock_read16(void *ctx, uint32_t offset)
{
    struct mock *mock = ctx;
    uint32_t *value = mock_register(mock, offset & ~3U);
    uint32_t raw;

    if (offset == ZZ_CAPTURE_DATA_REG) {
        ++mock->pixel_reads;
        if (mock->change_geometry &&
            mock->pixel_reads % ZZ_CAPTURE_SAMPLES == 1)
            ++mock->geometry;
        return (uint16_t)(mock->pixel_reads & 0xffffU);
    }
    if (offset == ZZ_CAPTURE_METADATA_DATA_REG) {
        ++mock->metadata_reads;
        return (uint16_t)(mock->metadata_reads & 0xffffU);
    }
    raw = value ? *value : 0;
    return offset & 2U ? (uint16_t)raw : (uint16_t)(raw >> 16);
}

static void mock_write16(void *ctx, uint32_t offset, uint16_t value)
{
    struct mock *mock = ctx;

    if (offset == ZZ_CAPTURE_PHASE_TARGET_REG)
        mock->target = value;
    else if (offset == ZZ_CAPTURE_PHASE_COMMIT_REG) {
        CHECK(value == ZZ_CAPTURE_PHASE_TOKEN);
        ++mock->commits;
    } else if (offset == ZZ_CAPTURE_ARM_REG) {
        CHECK(value == ZZ_CAPTURE_ARM_TOKEN);
        ++mock->arms;
        mock->snapshot ^= ZZ_CAPTURE_SNAPSHOT_ARM;
        mock->snapshot &= ~ZZ_CAPTURE_SNAPSHOT_VALID;
        mock->snapshot |= ZZ_CAPTURE_SNAPSHOT_BUSY;
    }
}

static void mock_delay(void *ctx, unsigned ticks)
{
    struct mock *mock = ctx;
    (void)ticks;
    ++mock->delays;
    if (mock->commits && mock->ack_after && --mock->ack_after == 0)
        mock->phase = ZZ_CAPTURE_PHASE_READY | ZZ_CAPTURE_PHASE_DONE |
            (mock->target & 0x0fffU);
    if (mock->arms && mock->capture_after && --mock->capture_after == 0) {
        mock->snapshot &= ~ZZ_CAPTURE_SNAPSHOT_BUSY;
        mock->snapshot |= ZZ_CAPTURE_SNAPSHOT_VALID;
        mock->snapshot += 1UL << 16;
    }
}

static int mock_keep_running(void *ctx)
{
    return !((struct mock *)ctx)->cancel;
}

static struct zz_vcap_io mock_io(struct mock *mock)
{
    struct zz_vcap_io io;
    io.ctx = mock;
    io.read16 = mock_read16;
    io.write16 = mock_write16;
    io.delay = mock_delay;
    io.control_ctx = mock;
    io.keep_running = mock_keep_running;
    return io;
}

static void init_c28(struct mock *mock)
{
    memset(mock, 0, sizeof(*mock));
    mock->cap = ZZ_CAPTURE_CAP_C28;
    mock->metadata_cap = ZZ_CAPTURE_METADATA_CAP;
    mock->clocks = ZZ_CAPTURE_CLOCK_FREQUENCY | ZZ_CAPTURE_CLOCK_LOCKED |
        ZZ_CAPTURE_CLOCK_READY | ZZ_CAPTURE_CLOCK_C28;
    mock->phase = ZZ_CAPTURE_PHASE_READY | ZZ_CAPTURE_PHASE_DONE | 0x0f85U;
    mock->snapshot = ZZ_CAPTURE_SNAPSHOT_VALID | 7UL << 16;
    mock->geometry = 0x00345067UL;
}

static void test_c28_order_and_phase(void)
{
    struct mock mock;
    struct zz_vcap_io io;
    struct zz_vcap_phase_state state;

    init_c28(&mock);
    io = mock_io(&mock);
    CHECK(zz_vcap_read32(&io, ZZ_CAPTURE_CLOCK_STATUS_REG) == mock.clocks);
    CHECK(zz_vcap_phase_read(&io, &state) == ZZ_VCAP_OK);
    CHECK(state.domain == ZZ_VCAP_PHASE_C28 && state.applied == -123);
    CHECK(state.ready && state.done && !state.busy && !state.error);
    CHECK(zz_vcap_phase_check(&io, -123) == ZZ_VCAP_OK);
}

static void test_e7m_masks_and_clock(void)
{
    struct mock mock;
    struct zz_vcap_io io;
    struct zz_vcap_phase_state state;

    init_c28(&mock);
    mock.cap = ZZ_CAPTURE_CAP_E7M;
    mock.clocks = ZZ_CAPTURE_CLOCK_READY;
    mock.phase = (1UL << 11) | ((uint32_t)-12 & 0x3ffU);
    io = mock_io(&mock);
    CHECK(zz_vcap_phase_read(&io, &state) == ZZ_VCAP_OK);
    CHECK(state.domain == ZZ_VCAP_PHASE_E7M && state.applied == -12);
    CHECK(state.done && state.ready && !state.busy && !state.error);
    CHECK(zz_vcap_phase_check(&io, -12) == ZZ_VCAP_OK);
    mock.phase |= 1UL << 12;
    CHECK(zz_vcap_phase_check(&io, -12) == ZZ_VCAP_ENGINE_ERROR);
    mock.phase &= ~(1UL << 12);
    mock.clocks = 0;
    CHECK(zz_vcap_phase_check(&io, -12) == ZZ_VCAP_CLOCK_UNQUALIFIED);
}

static void test_precommit_guards_and_restore(void)
{
    struct mock mock;
    struct zz_vcap_io io;

    init_c28(&mock);
    io = mock_io(&mock);
    mock.phase = ZZ_CAPTURE_PHASE_READY | ZZ_CAPTURE_PHASE_BUSY;
    CHECK(zz_vcap_phase_apply(&io, 17, 0) == ZZ_VCAP_TIMEOUT);
    CHECK(mock.commits == 0);
    init_c28(&mock);
    io = mock_io(&mock);
    mock.clocks = ZZ_CAPTURE_CLOCK_C28;
    CHECK(zz_vcap_phase_apply(&io, 17, 0) == ZZ_VCAP_CLOCK_UNQUALIFIED);
    CHECK(mock.commits == 0);
    init_c28(&mock);
    io = mock_io(&mock);
    mock.cancel = 1;
    mock.ack_after = 1;
    CHECK(zz_vcap_phase_apply(&io, 17, 1) == ZZ_VCAP_OK);
    CHECK(mock.commits == 1 && mock.target == 17);
    init_c28(&mock);
    io = mock_io(&mock);
    mock.phase |= ZZ_CAPTURE_PHASE_ERROR;
    CHECK(zz_vcap_phase_apply(&io, -31, 0) == ZZ_VCAP_ENGINE_ERROR);
    CHECK(mock.commits == 0);
    init_c28(&mock);
    io = mock_io(&mock);
    mock.phase |= ZZ_CAPTURE_PHASE_ERROR;
    mock.ack_after = 1;
    CHECK(zz_vcap_phase_apply(&io, -31, 1) == ZZ_VCAP_OK);
    CHECK(mock.commits == 1 && mock.target == (uint16_t)-31);
}

static void test_capture_detects_source_change(void)
{
    struct mock mock;
    struct zz_vcap_io io;
    struct zz_vcap_capture_context context;
    struct zz_vcap_capture_data data;
    uint32_t pixels[ZZ_CAPTURE_SAMPLES];
    uint32_t metadata[ZZ_CAPTURE_METADATA_WORDS];

    init_c28(&mock);
    io = mock_io(&mock);
    mock.capture_after = 1;
    memset(&context, 0, sizeof(context));
    context.phase = -123;
    context.require_phase = 1;
    context.geometry = mock.geometry;
    context.have_geometry = 1;
    memset(&data, 0, sizeof(data));
    data.pixels = pixels;
    data.metadata = metadata;
    CHECK(zz_vcap_capture(&io, &context, &data) == ZZ_VCAP_OK);
    CHECK(mock.arms == 1 && mock.pixel_reads == ZZ_CAPTURE_SAMPLES);
    CHECK(mock.metadata_reads == ZZ_CAPTURE_METADATA_WORDS);
    mock.change_geometry = 1;
    mock.capture_after = 1;
    CHECK(zz_vcap_capture(&io, &context, &data) == ZZ_VCAP_STATE_CHANGED);
}

int main(void)
{
    test_c28_order_and_phase();
    test_e7m_masks_and_clock();
    test_precommit_guards_and_restore();
    test_capture_detects_source_change();
    if (failures) {
        printf("%u of %u checks failed\n", failures, checks);
        return 1;
    }
    printf("zz_vcap client: %u checks passed\n", checks);
    return 0;
}
