/* Executes production polling, screen setup, sweep, refinement and cleanup.
 * The mocks model hardware/OS responses, never the utility's algorithms.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdarg.h>
#include <assert.h>
#define main zzcapture_main
#include "../../ZZCapture/ZZCapture.c"
#undef main
#undef printf
#undef puts
#undef fflush

static unsigned checks, failures;
#define CHECK(expr) do { ++checks; if (!(expr)) { ++failures; \
    printf("FAIL %u: %s\n", (unsigned)__LINE__, #expr); } } while (0)

static struct {
    int phase, target, busy, phase_ticks, ack_ticks, error, fail_commit;
    unsigned writes, commits, arms, data_reads, ticks, signals, replies;
    unsigned arms_at_commit, reads_at_commit;
    UWORD wire, address;
    int clock_ok, snapshot_ticks, snapshot_wait, snapshot_never;
    int timed_fields, stuck_parity, delay_minimum;
    unsigned native_time, snapshot_due, field_time, delay_time, read_time;
    unsigned alternate_read_time, read_pattern_length;
    const unsigned *delivery_steps;
    unsigned delivery_count, delivered, delivery_sequence, delivery_stop_after;
    unsigned corrupt_at_read, cancel_after_reads, escape_after_reads;
    uint32_t snapshot_status, snapshot_geometry;
    int cancel_phase, cancel_snapshot, cancel_always, escape_snapshot;
    int cancel_sent, screen_failure, all_clean;
    unsigned screen_opens, live_libraries, live_screens, live_windows, live_memory;
    int closed_before_restore, require_restore, entry;
    char output[32768];
    size_t output_length;
} mock;
static struct Task task;
static struct GfxBase graphics;
static struct IntuitionBase intuition;
static struct Screen native_screen;
static struct Window native_window;
static struct BitMap bitmap;
static UBYTE planes[8][160 * 512];
static UWORD pointer_memory[8];
static ULONG loaded_colors[770];
static struct IntuiMessage escape_message;

static uint32_t applied_status(void)
{
    return ((unsigned)mock.phase & 0xfffU) |
        (mock.busy ? ZZ_CAPTURE_PHASE_BUSY :
            ZZ_CAPTURE_PHASE_READY | ZZ_CAPTURE_PHASE_DONE) |
        (mock.error ? ZZ_CAPTURE_PHASE_ERROR : 0);
}

static uint32_t screen_pixel(unsigned sample)
{
    unsigned x = 211 + sample % ZZ_CAPTURE_COLUMNS;
    unsigned y = 64 + sample / ZZ_CAPTURE_COLUMNS;
    unsigned p, pen = 0;
    uint32_t rgb;
    for (p = 0; p < 8; ++p)
        if (planes[p][y * 160 + x / 8] & (0x80U >> (x & 7))) pen |= 1U << p;
    rgb = (uint32_t)(loaded_colors[1 + pen * 3] >> 24) << 16 |
          (uint32_t)(loaded_colors[2 + pen * 3] >> 24) << 8 |
          (uint32_t)(loaded_colors[3 + pen * 3] >> 24);
    if (!mock.all_clean && (mock.phase < -140 || mock.phase > 140)) rgb ^= 1;
    return rgb;
}

/* Fields run independently of requests. Delay's minimum duration stays at
 * 50 Hz while the source field period and snapshot read cost can vary.
 * Timer replies are serviced on field boundaries in this model; the ROI
 * completes partway into a field. These timings are synthetic, not measured
 * bus latencies. The default preserves the original PAL cadence fixture. */
#define FIELD_TIME 4096U
static void advance_native(unsigned now)
{
    mock.native_time = now;
    if (mock.timed_fields && !mock.snapshot_never && mock.snapshot_due &&
            now >= mock.snapshot_due) {
        unsigned sequence = mock.snapshot_due / mock.field_time;
        if (mock.delivery_count) {
            mock.delivery_sequence += mock.delivery_stop_after &&
                mock.delivered >= mock.delivery_stop_after ? 2 :
                mock.delivery_steps[mock.delivered % mock.delivery_count];
            ++mock.delivered;
            sequence = mock.delivery_sequence;
        }
        mock.snapshot_status = (sequence & 0xffffU) << 16 |
            (mock.snapshot_status & ZZ_CAPTURE_SNAPSHOT_ARM) |
            ZZ_CAPTURE_SNAPSHOT_VALID |
            (wanted_ntsc ? ZZ_CAPTURE_SNAPSHOT_NTSC : 0) |
            (wanted_lace ? ZZ_CAPTURE_SNAPSHOT_LACE : 0) |
            (!mock.stuck_parity && (sequence & 1) ? ZZ_CAPTURE_SNAPSHOT_PARITY : 0);
        mock.snapshot_due = 0;
    }
}

ULONG zz9000_read_reg32(ULONG base, ULONG reg)
{
    (void)base;
    switch (reg) {
    case ZZ_CAPTURE_CAP_REG: return ZZ_CAPTURE_CAP_C28;
    case ZZ_CAPTURE_PHASE_STATUS_REG: return applied_status();
    case ZZ_CAPTURE_CLOCK_STATUS_REG:
        return mock.clock_ok ? ZZ_CAPTURE_CLOCK_FREQUENCY | ZZ_CAPTURE_CLOCK_LOCKED |
            ZZ_CAPTURE_CLOCK_READY | ZZ_CAPTURE_CLOCK_C28 : ZZ_CAPTURE_CLOCK_C28;
    case ZZ_CAPTURE_SNAPSHOT_REG: return mock.snapshot_status;
    case ZZ_CAPTURE_GEOMETRY_REG: return mock.snapshot_geometry;
    case ZZ_CAPTURE_CLOCK_COUNTS_REG: return 7100UL << 16 | 28400;
    case ZZ_CAPTURE_DATA_REG: {
        uint32_t rgb;
        ++mock.data_reads;
        if (mock.timed_fields) advance_native(mock.native_time +
            (mock.read_pattern_length && mock.arms % mock.read_pattern_length ?
                mock.alternate_read_time : mock.read_time));
        rgb = screen_pixel(mock.address);
        return mock.data_reads == mock.corrupt_at_read ? rgb ^ 1U : rgb;
    }
    default: assert(0); return 0;
    }
}
UWORD zz9000_read_reg16(ULONG base, ULONG reg)
{ (void)base; (void)reg; return 0x160; }
void zz9000_write_reg16(ULONG base, ULONG reg, UWORD value)
{
    (void)base;
    ++mock.writes;
    switch (reg) {
    case ZZ_CAPTURE_PHASE_TARGET_REG:
        mock.wire = value;
        /* Match the controller's signed 16-bit input, not status decoding. */
        mock.target = (int16_t)value;
        break;
    case ZZ_CAPTURE_PHASE_COMMIT_REG:
        assert(value == ZZ_CAPTURE_PHASE_TOKEN);
        ++mock.commits;
        mock.arms_at_commit = mock.arms;
        mock.reads_at_commit = mock.data_reads;
        mock.error = !zz_capture_phase_valid(mock.target);
        mock.busy = 1;
        mock.phase_ticks = mock.ack_ticks;
        if ((int)mock.commits == mock.fail_commit) mock.phase_ticks = -1;
        break;
    case ZZ_CAPTURE_ARM_REG:
        assert(value == ZZ_CAPTURE_ARM_TOKEN);
        ++mock.arms;
        mock.snapshot_status ^= ZZ_CAPTURE_SNAPSHOT_ARM;
        mock.snapshot_status &= ~ZZ_CAPTURE_SNAPSHOT_VALID;
        mock.snapshot_status |= ZZ_CAPTURE_SNAPSHOT_BUSY;
        mock.snapshot_ticks = mock.snapshot_wait;
        if (mock.timed_fields)
            mock.snapshot_due = (mock.native_time / mock.field_time + 1) * mock.field_time + mock.field_time / 3;
        break;
    case ZZ_CAPTURE_ADDRESS_REG:
        assert(value < ZZ_CAPTURE_SAMPLES);
        mock.address = value;
        break;
    default: assert(0);
    }
}
int zz9000_find_board(struct ZZ9000Board *found)
{ found->address = 0x100000; found->zorro_version = 3; return 1; }
struct Task *FindTask(CONST_STRPTR name) { (void)name; return &task; }
ULONG SetSignal(ULONG signals, ULONG mask)
{
    (void)signals; (void)mask;
    ++mock.signals;
    if (mock.cancel_always || (!mock.cancel_sent &&
        ((mock.cancel_after_reads && mock.data_reads >= mock.cancel_after_reads) ||
         (mock.cancel_phase && mock.commits == 1 && mock.busy) ||
         (mock.cancel_snapshot && (mock.snapshot_status & ZZ_CAPTURE_SNAPSHOT_BUSY))))) {
        mock.cancel_sent = 1;
        return SIGBREAKF_CTRL_C;
    }
    return 0;
}
void *GetMsg(void *port)
{
    (void)port;
    if (!mock.cancel_sent &&
        ((mock.escape_snapshot && (mock.snapshot_status & ZZ_CAPTURE_SNAPSHOT_BUSY)) ||
         (mock.escape_after_reads && mock.data_reads >= mock.escape_after_reads))) {
        mock.cancel_sent = 1;
        escape_message.Class = IDCMP_RAWKEY;
        escape_message.Code = ZZ_CAPTURE_RAWKEY_ESCAPE;
        return &escape_message;
    }
    return NULL;
}
void ReplyMsg(struct Message *message) { (void)message; ++mock.replies; }
void WaitTOF(void)
{
    assert(GfxBase && screen && mock.timed_fields);
    ++mock.ticks;
    advance_native((mock.native_time / mock.field_time + 1) * mock.field_time);
}
void Delay(ULONG ticks)
{
    ULONG t;
    for (t = 0; t < ticks; ++t) {
        ++mock.ticks;
        if (mock.timed_fields)
            advance_native(((mock.native_time + (mock.delay_minimum ? mock.delay_time - 1 : 0)) /
                mock.field_time + 1) * mock.field_time);
        if (mock.busy && mock.phase_ticks > 0 && --mock.phase_ticks == 0) {
            mock.busy = 0;
            if (!mock.error) mock.phase = mock.target;
        }
        if (!mock.timed_fields && !mock.snapshot_never && mock.snapshot_ticks > 0 &&
            --mock.snapshot_ticks == 0) {
            uint32_t sequence = ((mock.snapshot_status >> 16) + 1U) & 0xffffU;
            mock.snapshot_status = sequence << 16 |
                (mock.snapshot_status & ZZ_CAPTURE_SNAPSHOT_ARM) |
                ZZ_CAPTURE_SNAPSHOT_VALID |
                (wanted_ntsc ? ZZ_CAPTURE_SNAPSHOT_NTSC : 0) |
                (wanted_lace ? ZZ_CAPTURE_SNAPSHOT_LACE : 0) |
                (sequence & 1 ? ZZ_CAPTURE_SNAPSHOT_PARITY : 0);
        }
    }
    assert(mock.ticks < 1000000);
}
void *OpenLibrary(CONST_STRPTR name, ULONG version)
{
    int gfx = !strcmp(name, "graphics.library");
    assert(version == 39);
    if (mock.screen_failure == (gfx ? 1 : 2)) return NULL;
    ++mock.live_libraries;
    return gfx ? (void *)&graphics : (void *)&intuition;
}
void CloseLibrary(struct Library *library)
{ assert(library && mock.live_libraries); --mock.live_libraries; }
ULONG ModeNotAvailable(ULONG id) { (void)id; return 0; }
ULONG GetDisplayInfoData(void *handle, UBYTE *data, ULONG size, ULONG tag, ULONG id)
{
    (void)handle; (void)size;
    if (tag == DTAG_DISP) {
        struct DisplayInfo *info = (struct DisplayInfo *)data;
        info->PropertyFlags = (id & PAL_MONITOR_ID ? DIPF_IS_PAL : 0) |
            (id & 1 ? DIPF_IS_LACE : 0);
        info->RedBits = info->GreenBits = info->BlueBits = 8;
    } else ((struct DimensionInfo *)data)->MaxDepth = 8;
    return 1;
}
struct Screen *OpenScreenTags(void *unused, ...)
{
    (void)unused;
    ++mock.screen_opens;
    if (mock.screen_failure == 3) return NULL;
    native_screen.Width = mock.screen_failure == 4 ? 640 : 1280;
    native_screen.Height = (wanted_ntsc ? 200 : 256) * (wanted_lace ? 2 : 1);
    native_screen.ViewPort.mode = (wanted_ntsc ? NTSC_MONITOR_ID : PAL_MONITOR_ID) |
        (wanted_lace ? SUPERLACE_KEY : SUPER_KEY);
    ++mock.live_screens;
    return &native_screen;
}
struct Window *OpenWindowTags(void *unused, ...)
{
    (void)unused;
    if (mock.screen_failure == 5) return NULL;
    ++mock.live_windows;
    return &native_window;
}
void CloseWindow(struct Window *closed)
{ assert(closed == &native_window && mock.live_windows); --mock.live_windows; }
int CloseScreen(struct Screen *closed)
{
    assert(closed == &native_screen && mock.live_screens);
    if (mock.require_restore && (mock.phase != mock.entry || mock.busy))
        mock.closed_before_restore = 1;
    --mock.live_screens;
    intuition.FirstScreen = NULL;
    return 1;
}
void *AllocMem(ULONG size, ULONG flags)
{
    assert(size == 16 && flags == (MEMF_CHIP | MEMF_CLEAR));
    if (mock.screen_failure == 6) return NULL;
    ++mock.live_memory;
    return pointer_memory;
}
void FreeMem(void *memory, ULONG size)
{ assert(memory == pointer_memory && size == 16 && mock.live_memory); --mock.live_memory; }
void SetPointer(struct Window *win, UWORD *data, unsigned height, unsigned width, int x, int y)
{ (void)win; (void)data; (void)height; (void)width; (void)x; (void)y; }
void LoadRGB32(struct ViewPort *vp, const ULONG *palette)
{ (void)vp; memcpy(loaded_colors, palette, sizeof(loaded_colors)); }
ULONG GetVPModeID(struct ViewPort *vp) { return vp->mode; }
void SetAPen(struct RastPort *rp, unsigned pen) { (void)rp; (void)pen; }
void RectFill(struct RastPort *rp, int x0, int y0, int x1, int y1)
{ (void)rp; (void)x0; (void)y0; (void)x1; (void)y1; }
void SetDrMd(struct RastPort *rp, unsigned mode) { (void)rp; (void)mode; }
void Move(struct RastPort *rp, int x, int y) { (void)rp; (void)x; (void)y; }
void Text(struct RastPort *rp, CONST_STRPTR text, ULONG length)
{ (void)rp; (void)text; (void)length; }
void WaitBlit(void) { }
void ScreenToFront(struct Screen *front) { intuition.FirstScreen = front; }
void ActivateWindow(struct Window *active) { (void)active; }
int zzcapture_test_printf(const char *format, ...)
{
    va_list ap;
    int result;
    va_start(ap, format);
    result = vsnprintf(mock.output + mock.output_length,
        sizeof(mock.output) - mock.output_length, format, ap);
    va_end(ap);
    assert(result >= 0 && (size_t)result < sizeof(mock.output) - mock.output_length);
    mock.output_length += (size_t)result;
    return result;
}
int zzcapture_test_puts(const char *text) { return zzcapture_test_printf("%s\n", text); }
int zzcapture_test_fflush(FILE *file) { (void)file; return 0; }

static void reset_fixture(void)
{
    unsigned p;
    memset(&mock, 0, sizeof(mock));
    memset(&current, 0, sizeof(current));
    memset(previous, 0, sizeof(previous));
    memset(&coverage, 0, sizeof(coverage));
    memset(planes, 0, sizeof(planes));
    memset(loaded_colors, 0, sizeof(loaded_colors));
    mock.phase = mock.entry = -77;
    mock.clock_ok = 1;
    mock.delay_minimum = 1;
    mock.field_time = mock.delay_time = FIELD_TIME;
    mock.read_time = 2;
    mock.ack_ticks = mock.snapshot_wait = 2;
    mock.snapshot_geometry = 26UL << 12 | 188;
    task.tc_SPLower = (void *)(uintptr_t)0x1000;
    task.tc_SPUpper = (void *)(uintptr_t)(0x1000 + ZZ_CAPTURE_MIN_STACK);
    graphics.ChipRevBits0 = GFXF_AA_ALICE | GFXF_AA_LISA;
    intuition.FirstScreen = NULL;
    bitmap.Depth = 8;
    bitmap.BytesPerRow = 160;
    for (p = 0; p < 8; ++p) bitmap.Planes[p] = planes[p];
    native_screen.RastPort.BitMap = &bitmap;
    GfxBase = NULL; IntuitionBase = NULL; screen = NULL; window = NULL;
    empty_pointer = NULL;
    failure[0] = 0;
    aborted = wanted_ntsc = wanted_lace = have_geometry = 0;
    zz9000_find_board(&board);
}
static void check_closed(void)
{
    CHECK(!screen && !window && !empty_pointer && !GfxBase && !IntuitionBase);
    CHECK(!mock.live_libraries && !mock.live_screens && !mock.live_windows && !mock.live_memory);
}

static void test_wire_and_phase_waits(void)
{
    int phase;
    for (phase = ZZ_CAPTURE_PHASE_MIN; phase <= ZZ_CAPTURE_PHASE_MAX; ++phase) {
        reset_fixture();
        CHECK(apply_phase(phase, 0));
        CHECK(mock.wire == (uint16_t)(int16_t)phase);
        CHECK(mock.phase == phase && mock.commits == 1 && mock.writes == 2);
        CHECK(mock.ticks == 2);
    }
    reset_fixture(); CHECK(apply_phase(-896, 0)); CHECK(mock.wire == 0xfc80);
    reset_fixture(); CHECK(apply_phase(-1, 0)); CHECK(mock.wire == 0xffff);
    reset_fixture(); mock.busy = 1; mock.phase_ticks = -1;
    CHECK(!apply_phase(0, 0)); CHECK(mock.ticks == PHASE_WAIT_TICKS && mock.writes == 0);
    CHECK(strstr(failure, "become idle") != NULL);
    reset_fixture(); mock.fail_commit = 1;
    CHECK(!apply_phase(0, 0)); CHECK(mock.ticks == PHASE_WAIT_TICKS + 1);
    CHECK(strstr(failure, "not acknowledged") != NULL);
    reset_fixture(); mock.clock_ok = 0;
    CHECK(!apply_phase(0, 0)); CHECK(mock.writes == 0 && mock.ticks == 0);
    reset_fixture(); mock.cancel_phase = 1;
    CHECK(!apply_phase(100, 0)); CHECK(aborted && mock.busy && mock.cancel_sent);
    mock.cancel_always = 1;
    CHECK(apply_phase(-77, 1));
    CHECK(mock.phase == -77 && mock.wire == 0xffb3 && mock.commits == 2);
    CHECK(mock.ticks == 4 && mock.signals == 1);
}

static void test_snapshot_and_cleanup(void)
{
    int failure_stage;
    reset_fixture(); CHECK(open_screen());
    CHECK(take_snapshot(&current, -77, 1));
    CHECK(zz_capture_pattern_errors(current.pixels) == 0 && mock.data_reads == ZZ_CAPTURE_SAMPLES);
    CHECK(take_snapshot(&current, -77, 0));
    CHECK(mock.data_reads == ZZ_CAPTURE_SAMPLES); /* Discard reads metadata only. */
    close_screen(); check_closed();
    reset_fixture(); CHECK(open_screen()); mock.snapshot_never = 1;
    CHECK(!take_snapshot(&current, -77, 1));
    CHECK(mock.ticks == 10 + FRAME_WAIT_TICKS && mock.arms == 1 && !mock.data_reads);
    CHECK(strstr(failure, "No fresh") != NULL); close_screen(); check_closed();
    for (failure_stage = 1; failure_stage <= 6; ++failure_stage) {
        reset_fixture(); mock.screen_failure = failure_stage; mock.require_restore = 1;
        CHECK(calibrate(-77) == 20);
        CHECK(mock.phase == -77 && mock.commits == 1 && mock.wire == 0xffb3);
        CHECK(strstr(mock.output, "restored and acknowledged") != NULL);
        CHECK(!mock.closed_before_restore); check_closed();
    }
}

static void test_calibration_failures(void)
{
    int kind;
    for (kind = 0; kind < 3; ++kind) {
        reset_fixture(); mock.require_restore = 1;
        mock.cancel_phase = kind == 0;
        mock.cancel_snapshot = kind == 1;
        mock.escape_snapshot = kind == 2;
        CHECK(calibrate(-77) == 20);
        CHECK(mock.cancel_sent && aborted && mock.phase == -77 && mock.commits == 2);
        CHECK(strstr(mock.output, "Calibration cancelled") != NULL);
        CHECK(strstr(mock.output, "restored and acknowledged") != NULL);
        CHECK(!mock.closed_before_restore && mock.ticks < 20);
        if (kind == 2) CHECK(mock.replies == 1);
        check_closed();
    }
    reset_fixture(); mock.snapshot_never = 1; mock.require_restore = 1;
    CHECK(calibrate(-77) == 20);
    CHECK(mock.phase == -77 && mock.commits == 2 && mock.ticks == 10 + 2 + FRAME_WAIT_TICKS + 2);
    CHECK(strstr(mock.output, "No fresh") != NULL && !mock.closed_before_restore);
    check_closed();
    reset_fixture(); mock.cancel_snapshot = 1; mock.fail_commit = 2;
    CHECK(calibrate(-77) == 20);
    CHECK(mock.commits == 2 && mock.wire == 0xffb3);
    CHECK(strstr(mock.output, "RESTORE FAILED") != NULL);
    CHECK(strstr(mock.output, "phase is unknown") != NULL);
    CHECK(strstr(mock.output, "restored and acknowledged") == NULL);
    CHECK(mock.ticks == 10 + 2 + PHASE_WAIT_TICKS + 1);
    check_closed();
    reset_fixture(); mock.all_clean = 1; mock.require_restore = 1;
    CHECK(calibrate(-77) == 20);
    CHECK(mock.phase == -77 && mock.commits == ZZ_CAPTURE_BINS + 1);
    CHECK(strstr(mock.output, "Every position passed") != NULL);
    CHECK(strstr(mock.output, "Candidate setting") == NULL);
    CHECK(!mock.closed_before_restore); check_closed();
}

static void test_success_and_entry_guard(void)
{
    char *pal[] = {"ZZCapture", "calibrate", "pal"};
    char *ntsc_lace[] = {"ZZCapture", "calibrate", "ntsc", "lace"};
    char *phase[] = {"ZZCapture", "phase", "-1"};
    reset_fixture();
    task.tc_SPUpper = (void *)(uintptr_t)(0x1000 + 4096);
    CHECK(zzcapture_main(3, pal) == 20);
    CHECK(mock.writes == 0 && mock.screen_opens == 0 && mock.live_libraries == 0);
    CHECK(strstr(mock.output, "Stack 32768") != NULL);
    CHECK(zzcapture_main(3, phase) == 20 && mock.writes == 0);
    task.tc_SPUpper = (void *)(uintptr_t)(0x1000 + ZZ_CAPTURE_MIN_STACK - 1);
    CHECK(zzcapture_main(3, phase) == 20 && mock.writes == 0);
    reset_fixture(); CHECK(zzcapture_main(3, phase) == 0);
    CHECK(mock.phase == -1 && mock.wire == 0xffff);
    CHECK(strstr(mock.output, "applied and acknowledged") != NULL);
    reset_fixture(); mock.cancel_phase = 1;
    CHECK(zzcapture_main(3, phase) == 20);
    CHECK(mock.phase == -77 && mock.wire == 0xffb3 && mock.commits == 2);
    CHECK(strstr(mock.output, "Original phase -77 restored.") != NULL);
    reset_fixture(); mock.cancel_phase = 1; mock.fail_commit = 2;
    CHECK(zzcapture_main(3, phase) == 20 && mock.commits == 2);
    CHECK(strstr(mock.output, "RESTORE FAILED") != NULL);
    CHECK(strstr(mock.output, "Original phase -77 restored.") == NULL);
    reset_fixture(); CHECK(zzcapture_main(3, pal) == 0);
    CHECK(mock.phase == 0 && mock.commits > ZZ_CAPTURE_BINS);
    CHECK(mock.arms - mock.arms_at_commit == 53);
    CHECK(mock.data_reads - mock.reads_at_commit == 51 * ZZ_CAPTURE_SAMPLES);
    CHECK(strstr(mock.output, "Candidate setting: videocap_c28_phase = 0") != NULL);
    CHECK(strstr(mock.output, "through 140") != NULL);
    CHECK(strstr(mock.output, "Original phase") == NULL);
    check_closed();
    reset_fixture(); CHECK(zzcapture_main(4, ntsc_lace) == 0);
    CHECK(wanted_ntsc && wanted_lace && mock.phase == 0);
    CHECK(mock.arms - mock.arms_at_commit == 104);
    CHECK(mock.data_reads - mock.reads_at_commit == 102 * ZZ_CAPTURE_SAMPLES);
    CHECK(mock.snapshot_status & ZZ_CAPTURE_SNAPSHOT_LACE);
    CHECK(mock.snapshot_status & ZZ_CAPTURE_SNAPSHOT_NTSC);
    CHECK(strstr(mock.output, "Candidate setting: videocap_c28_phase = 0") != NULL);
    check_closed();
}

static void test_interlace_polling_cadence(void)
{
    struct score score;
    char *check_lace[] = {"ZZCapture", "check", "pal", "lace"};
    char *check_pal[] = {"ZZCapture", "check", "pal"};
    char *calibrate_lace[] = {"ZZCapture", "calibrate", "pal", "lace"};
    reset_fixture(); wanted_lace = 1; mock.timed_fields = 1;
    mock.delay_minimum = 0;
    CHECK(open_screen());
    CHECK(measure_phase(-77, SWEEP_COMPARISONS, &score));
    CHECK(!score.wrong && !score.changed);
    CHECK(coverage.comparisons[0] >= SWEEP_COMPARISONS && coverage.comparisons[1] >= SWEEP_COMPARISONS);
    CHECK(coverage.cadence_waits > 0 && coverage.min_step == 2 && coverage.max_step == 3);
    close_screen(); check_closed();

    /* DOS Delay must wait the requested duration. Rounding a half-field
     * start up to the next interrupt adds two fields for Delay(1), unlike
     * waiting only until the next vertical blank. This reproduces the
     * maintainer's 0.2 report: one step of 2 followed by steps of 4. */
    reset_fixture(); wanted_lace = 1; mock.timed_fields = mock.delay_minimum = 1;
    CHECK(open_screen());
    if (!measure_phase(-77, SWEEP_COMPARISONS, &score))
        printf("minimum-delay coverage: %u/%u samples, sequence delta %u, steps %u..%u, waits %u\n",
            coverage.samples[0], coverage.samples[1],
            (coverage.last_sequence - coverage.first_sequence) & 0xffffU,
            coverage.min_step, coverage.max_step, coverage.cadence_waits);
    CHECK(coverage.comparisons[0] >= SWEEP_COMPARISONS && coverage.comparisons[1] >= SWEEP_COMPARISONS);
    CHECK(!score.wrong && !score.changed);
    close_screen(); check_closed();

    reset_fixture(); wanted_lace = 1; mock.timed_fields = 1;
    mock.native_time = FIELD_TIME * 65500U;
    CHECK(open_screen());
    CHECK(measure_phase(-77, SWEEP_COMPARISONS, &score));
    CHECK(coverage.last_sequence < coverage.first_sequence);
    CHECK(coverage.min_step == 2 && coverage.max_step == 3);
    close_screen(); check_closed();

    reset_fixture(); mock.timed_fields = 1;
    CHECK(zzcapture_main(4, calibrate_lace) == 0);
    CHECK(mock.phase == 0 && coverage.cadence_waits > 0);
    CHECK(strstr(mock.output, "Candidate setting: videocap_c28_phase = 0") != NULL);
    check_closed();

    /* A delay must not turn a stuck FPGA parity bit into a successful run. */
    reset_fixture(); wanted_lace = 1; mock.timed_fields = mock.stuck_parity = 1;
    CHECK(open_screen());
    CHECK(!measure_phase(-77, SWEEP_COMPARISONS, &score));
    CHECK(coverage.samples[0] == 6 * (SWEEP_COMPARISONS + 1) && coverage.samples[1] == 0);
    CHECK(strstr(failure, "Both interlaced fields") != NULL);
    close_screen(); check_closed();

    reset_fixture(); mock.timed_fields = 1; mock.require_restore = 1;
    CHECK(zzcapture_main(4, check_lace) == 0);
    CHECK(coverage.comparisons[0] >= RETEST_COMPARISONS && coverage.comparisons[1] >= RETEST_COMPARISONS);
    CHECK(strstr(mock.output, "Current phase passed") != NULL);
    CHECK(strstr(mock.output, "Field coverage at phase -77") != NULL);
    CHECK(mock.phase == -77 && mock.commits == 2 && !mock.closed_before_restore);
    check_closed();

    reset_fixture(); mock.timed_fields = mock.stuck_parity = 1; mock.require_restore = 1;
    CHECK(zzcapture_main(4, check_lace) == 20);
    CHECK(strstr(mock.output, "comparisons 305/0") != NULL);
    CHECK(strstr(mock.output, "Current phase passed") == NULL);
    CHECK(strstr(mock.output, "restored and acknowledged") != NULL);
    CHECK(mock.phase == -77 && !mock.closed_before_restore);
    check_closed();

    reset_fixture(); mock.phase = mock.entry = 300; mock.require_restore = 1;
    CHECK(zzcapture_main(3, check_pal) == 20);
    CHECK(strstr(mock.output, "wrong or changing pixels") != NULL);
    CHECK(mock.phase == 300 && !mock.closed_before_restore);
    check_closed();

    reset_fixture(); mock.cancel_snapshot = 1; mock.require_restore = 1;
    CHECK(zzcapture_main(4, check_lace) == 20);
    CHECK(mock.phase == -77 && !mock.closed_before_restore);
    CHECK(strstr(mock.output, "Calibration cancelled") != NULL);
    check_closed();

    reset_fixture(); mock.fail_commit = 2;
    CHECK(zzcapture_main(4, check_lace) == 20);
    CHECK(strstr(mock.output, "RESTORE FAILED") != NULL);
    CHECK(strstr(mock.output, "restored and acknowledged") == NULL);
    check_closed();
}
static void configure_skewed_delivery(void)
{
    static const unsigned steps[] = {2, 2, 3, 3, 2, 4, 2};
    mock.timed_fields = 1;
    mock.field_time = 3417;
    mock.delivery_steps = steps;
    mock.delivery_count = sizeof(steps) / sizeof(steps[0]);
}

static void test_skewed_interlace_delivery(void)
{
    /* Synthetic delivery reproduces the reported 56/10 snapshots and 55/9
     * comparisons at the old 66-snapshot cutoff. Steps stay within the
     * reported 2..4 range. The report does not contain an exact field trace;
     * this checks collection accounting, not the physical cause of the skew. */
    struct score score;
    int ok;
    reset_fixture(); configure_skewed_delivery(); wanted_ntsc = wanted_lace = 1;
    CHECK(open_screen());
    ok = measure_phase(-77, SWEEP_COMPARISONS, &score);
    if (!ok)
        printf("skewed delivery: samples %u/%u, comparisons %u/%u, steps %u..%u\n",
            coverage.samples[0], coverage.samples[1], coverage.comparisons[0],
            coverage.comparisons[1], coverage.min_step, coverage.max_step);
    CHECK(ok);
    CHECK(coverage.samples[0] + coverage.samples[1] > 66);
    CHECK(coverage.comparisons[0] >= SWEEP_COMPARISONS &&
        coverage.comparisons[1] >= SWEEP_COMPARISONS);
    CHECK(!score.wrong && !score.changed);
    close_screen(); check_closed();
}

static void test_extended_collection_lifecycle(void)
{
    char *check_lace[] = {"ZZCapture", "check", "ntsc", "lace"};
    char *calibrate_lace[] = {"ZZCapture", "calibrate", "ntsc", "lace"};
    const unsigned extended_read = 307U * ZZ_CAPTURE_SAMPLES;
    unsigned kind;

    reset_fixture(); configure_skewed_delivery(); mock.require_restore = 1;
    CHECK(zzcapture_main(4, check_lace) == 0);
    CHECK(coverage.comparisons[0] >= 50 && coverage.comparisons[1] >= 50);
    CHECK(coverage.samples[0] + coverage.samples[1] > 306);
    CHECK(strstr(mock.output, "Current phase passed") != NULL);
    CHECK(mock.phase == -77 && !mock.closed_before_restore);
    check_closed();

    /* One corrupt pixel AFTER the old cutoff must invalidate the check. */
    reset_fixture(); configure_skewed_delivery(); mock.require_restore = 1;
    mock.corrupt_at_read = 306U * ZZ_CAPTURE_SAMPLES + 2;
    CHECK(zzcapture_main(4, check_lace) == 20);
    CHECK(coverage.comparisons[0] >= 50 && coverage.comparisons[1] >= 50);
    CHECK(strstr(mock.output, "Expected-pixel errors: 1;") != NULL);
    CHECK(strstr(mock.output, "wrong or changing pixels") != NULL);
    CHECK(strstr(mock.output, "Current phase passed") == NULL);
    CHECK(mock.phase == -77 && !mock.closed_before_restore);
    check_closed();

    for (kind = 0; kind < 3; ++kind) {
        reset_fixture(); configure_skewed_delivery(); mock.require_restore = 1;
        if (kind == 1) mock.escape_after_reads = extended_read;
        else mock.cancel_after_reads = extended_read;
        if (kind == 2) mock.fail_commit = 2;
        CHECK(zzcapture_main(4, check_lace) == 20);
        CHECK(mock.cancel_sent && aborted && mock.data_reads == extended_read);
        CHECK(strstr(mock.output, "Calibration cancelled") != NULL);
        CHECK(strstr(mock.output, "Current phase passed") == NULL);
        if (kind == 2) {
            CHECK(strstr(mock.output, "RESTORE FAILED") != NULL);
            CHECK(strstr(mock.output, "restored and acknowledged") == NULL);
        } else {
            CHECK(strstr(mock.output, "restored and acknowledged") != NULL);
            CHECK(mock.phase == -77 && !mock.closed_before_restore);
        }
        if (kind == 1) CHECK(mock.replies == 1);
        check_closed();
    }

    reset_fixture(); configure_skewed_delivery();
    CHECK(zzcapture_main(4, calibrate_lace) == 0);
    CHECK(mock.phase == 0 && coverage.comparisons[0] >= 50 && coverage.comparisons[1] >= 50);
    CHECK(strstr(mock.output, "Candidate setting: videocap_c28_phase = 0") != NULL);
    check_closed();
}

static void test_collection_progress_bounds(void)
{
    unsigned sparse_steps[64], i;
    struct score score;

    /* Both parities appear, then one stops before completing its quota. */
    reset_fixture(); configure_skewed_delivery(); wanted_ntsc = wanted_lace = 1;
    mock.delivery_stop_after = 10;
    CHECK(open_screen());
    CHECK(!measure_phase(-77, SWEEP_COMPARISONS, &score));
    CHECK(coverage.samples[0] && coverage.samples[1]);
    CHECK(coverage.samples[0] + coverage.samples[1] == 73);
    CHECK(strstr(failure, "Both interlaced fields") != NULL);
    close_screen(); check_closed();

    /* Rare progress must not keep extending the total budget forever. */
    for (i = 0; i < 64; ++i) sparse_steps[i] = 2;
    sparse_steps[2] = sparse_steps[3] = 3;
    reset_fixture(); configure_skewed_delivery(); wanted_ntsc = wanted_lace = 1;
    mock.delivery_steps = sparse_steps; mock.delivery_count = 64;
    CHECK(open_screen());
    CHECK(!measure_phase(-77, SWEEP_COMPARISONS, &score));
    CHECK(coverage.samples[0] + coverage.samples[1] == 132);
    CHECK(coverage.comparisons[1] > 0 && coverage.comparisons[1] < SWEEP_COMPARISONS);
    CHECK(strstr(failure, "Both interlaced fields") != NULL);
    close_screen(); check_closed();
}

static void test_pal_ntsc_read_cadence(void)
{
    unsigned ntsc, cost;
    for (ntsc = 0; ntsc < 2; ++ntsc) for (cost = 1; cost <= 7; ++cost) {
        struct score score;
        reset_fixture(); wanted_ntsc = ntsc; wanted_lace = mock.timed_fields = 1;
        mock.field_time = ntsc ? 3417 : FIELD_TIME;
        mock.read_time = cost;
        mock.alternate_read_time = 8 - cost;
        mock.read_pattern_length = 7;
        CHECK(open_screen());
        CHECK(measure_phase(-77, RETEST_COMPARISONS, &score));
        CHECK(coverage.comparisons[0] >= 50 && coverage.comparisons[1] >= 50);
        CHECK(!score.wrong && !score.changed);
        close_screen(); check_closed();
    }
}
int main(void)
{
    test_wire_and_phase_waits();
    test_snapshot_and_cleanup();
    test_calibration_failures();
    test_success_and_entry_guard();
    test_interlace_polling_cadence();
    test_skewed_interlace_delivery();
    test_extended_collection_lifecycle();
    test_collection_progress_bounds();
    test_pal_ntsc_read_cadence();
    printf("ZZCapture lifecycle: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
