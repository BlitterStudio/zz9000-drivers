/*
 * Native-video row observation and A4000 C28 sampling calibration.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifdef ZZCAPTURE_TEST_IO
#include "zzcapture_test_platform.h"
#else
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/tasks.h>
#include <devices/timer.h>
#include <graphics/gfxbase.h>
#include <graphics/modeid.h>
#include <intuition/intuitionbase.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/timer.h>
#include "zz9000_hw.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>

#include "zz9000_capture_calibration.h"

struct GfxBase *GfxBase;
struct IntuitionBase *IntuitionBase;
struct Device *TimerBase;

#define ZZ_CAPTURE_VERSION "0.9"
static const char version[] __attribute__((used)) =
    "$VER: ZZCapture " ZZ_CAPTURE_VERSION " (23.09.2026)\r\n";

#define PHASE_WAIT_TICKS 250U
#define FRAME_WAIT_TICKS 100U
#define SWEEP_COMPARISONS 10U
#define RETEST_COMPARISONS 50U
#define STARTUP_DURATION_MS 180000UL
#define STARTUP_MAX_PAIRS 200U
#define ZZ_CAPTURE_RAWKEY_ESCAPE 0x45U
#define ZZ_CAPTURE_MIN_STACK 32768UL

struct snapshot {
    uint32_t pixels[ZZ_CAPTURE_SAMPLES];
    uint32_t metadata[ZZ_CAPTURE_METADATA_WORDS];
    ULONG status;
    ULONG geometry;
};

struct score {
    unsigned long wrong;
    unsigned long changed;
};

static struct ZZ9000Board board;
static struct Screen *screen;
static struct Window *window;
static UWORD *empty_pointer;
static struct snapshot current, previous[2];
/* One bounded packet per check/startup command. No extra hardware captures,
 * allocations, analysis or bulk output inside the measurement loop. */
static struct {
    int enabled, valid, have_reference, phase;
    unsigned measurement, captured_measurement, sample, parity, wrong, changed;
    struct snapshot failed, reference;
    ULONG metadata_capability;
    ULONG field_diag_capability, field_diag_build, field_diag_variant;
    ULONG clock_status, clock_counts, phase_status;
} first_failure;
static char failure[192];
static int aborted;
static int wanted_ntsc, wanted_lace;
static int have_geometry;
static ULONG geometry;
static unsigned light_pen, dark_pen;
static ULONG colors[770];
static struct {
    int phase;
    unsigned samples[2], comparisons[2], cadence_waits;
    unsigned first_sequence, last_sequence, min_step, max_step;
} coverage;

static ULONG read32(ULONG reg)
{
    return zz9000_read_reg32(board.address, reg);
}

static int fail(const char *message)
{
    snprintf(failure, sizeof(failure), "%s", message);
    return 0;
}

static int stack_ready(void)
{
    struct Task *task = FindTask(NULL);
    if (!task || (uintptr_t)task->tc_SPUpper <= (uintptr_t)task->tc_SPLower ||
        (uintptr_t)task->tc_SPUpper - (uintptr_t)task->tc_SPLower < ZZ_CAPTURE_MIN_STACK)
        return fail("Run Stack 32768 in this Shell before changing or calibrating the phase.");
    return 1;
}

static int capability(void)
{
    if (read32(ZZ_CAPTURE_CAP_REG) != ZZ_CAPTURE_CAP_C28 ||
        read32(ZZ_CAPTURE_METADATA_CAP_REG) != ZZ_CAPTURE_METADATA_CAP)
        return fail("This firmware does not support matched C28 row-timing capture.");
    return 1;
}

static int observation_capability(void)
{
    ULONG capture = read32(ZZ_CAPTURE_CAP_REG);
    if ((capture != ZZ_CAPTURE_CAP_E7M && capture != ZZ_CAPTURE_CAP_C28) ||
        read32(ZZ_CAPTURE_METADATA_CAP_REG) != ZZ_CAPTURE_METADATA_CAP ||
        read32(ZZ_CAPTURE_FIELD_DIAG_CAP_REG) != ZZ_CAPTURE_FIELD_DIAG_CAP)
        return fail("This firmware does not support matched row-metadata observation.");
    return 1;
}

/* Retain provenance and live controller state at the first scored failure.
 * This performs no capture or arm operation. The field diagnostic identity
 * describes the loaded bitstream; row words remain tied to each snapshot. */
static void capture_failure_context(void)
{
    first_failure.metadata_capability = read32(ZZ_CAPTURE_METADATA_CAP_REG);
    first_failure.field_diag_capability = read32(ZZ_CAPTURE_FIELD_DIAG_CAP_REG);
    first_failure.field_diag_build = read32(ZZ_CAPTURE_FIELD_DIAG_BUILD_REG);
    first_failure.field_diag_variant = read32(ZZ_CAPTURE_FIELD_DIAG_VARIANT_REG);
    first_failure.clock_status = read32(ZZ_CAPTURE_CLOCK_STATUS_REG);
    first_failure.clock_counts = read32(ZZ_CAPTURE_CLOCK_COUNTS_REG);
    first_failure.phase_status = read32(ZZ_CAPTURE_PHASE_STATUS_REG);
}

static int continue_test(void)
{
    struct IntuiMessage *message;
    if (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) aborted = 1;
    if (window) {
        while ((message = (struct IntuiMessage *)GetMsg(window->UserPort))) {
            if (message->Class == IDCMP_RAWKEY && message->Code == ZZ_CAPTURE_RAWKEY_ESCAPE)
                aborted = 1;
            ReplyMsg((struct Message *)message);
        }
    }
    if (aborted) return fail("Calibration cancelled.");
    if (screen && IntuitionBase->FirstScreen != screen)
        return fail("The test screen was moved to the back; calibration stopped.");
    return 1;
}

static int clock_ready(void)
{
    ULONG status = read32(ZZ_CAPTURE_CLOCK_STATUS_REG);
    const ULONG required = ZZ_CAPTURE_CLOCK_FREQUENCY | ZZ_CAPTURE_CLOCK_LOCKED |
        ZZ_CAPTURE_CLOCK_READY | ZZ_CAPTURE_CLOCK_C28;
    if ((status & required) != required || (status & ZZ_CAPTURE_CLOCK_FAULT))
        return fail("The 28 MHz input clock is missing, unlocked, or unstable.");
    return 1;
}

static int phase_is(int target)
{
    ULONG status = read32(ZZ_CAPTURE_PHASE_STATUS_REG);
    if (!clock_ready()) return 0;
    if (!(status & ZZ_CAPTURE_PHASE_READY) ||
        (status & (ZZ_CAPTURE_PHASE_BUSY | ZZ_CAPTURE_PHASE_ERROR)) ||
        zz_capture_phase_decode(status) != target)
        return fail("The sampling phase changed or stopped responding during capture.");
    return 1;
}

/* Restore also uses this bounded path, but ignores cancellation and permits
 * an error flag to be cleared by a fresh request. No blind writes on a mixed
 * firmware install, and no claim of restoration without applied readback. */
static int apply_phase(int target, int restoring)
{
    unsigned tick;
    ULONG status, clocks;
    if (!capability() || !zz_capture_phase_valid(target)) return 0;
    for (tick = 0; tick < PHASE_WAIT_TICKS; ++tick) {
        status = read32(ZZ_CAPTURE_PHASE_STATUS_REG);
        if (!(status & ZZ_CAPTURE_PHASE_BUSY)) break;
        if (!restoring && !continue_test()) return 0;
        Delay(1);
    }
    if (tick == PHASE_WAIT_TICKS)
        return fail("The phase controller did not become idle.");
    clocks = read32(ZZ_CAPTURE_CLOCK_STATUS_REG);
    if ((clocks & (ZZ_CAPTURE_CLOCK_FREQUENCY | ZZ_CAPTURE_CLOCK_LOCKED |
                   ZZ_CAPTURE_CLOCK_C28)) !=
        (ZZ_CAPTURE_CLOCK_FREQUENCY | ZZ_CAPTURE_CLOCK_LOCKED | ZZ_CAPTURE_CLOCK_C28))
        return fail("Cannot set the sampling phase without a locked 28 MHz input.");
    zz9000_write_reg16(board.address, ZZ_CAPTURE_PHASE_TARGET_REG,
        (UWORD)zz_capture_phase_encode(target));
    zz9000_write_reg16(board.address, ZZ_CAPTURE_PHASE_COMMIT_REG,
        ZZ_CAPTURE_PHASE_TOKEN);
    /* Give the request a chance to replace old done/error flags. */
    Delay(1);
    for (tick = 0; tick < PHASE_WAIT_TICKS; ++tick) {
        status = read32(ZZ_CAPTURE_PHASE_STATUS_REG);
        if ((status & (ZZ_CAPTURE_PHASE_READY | ZZ_CAPTURE_PHASE_DONE)) ==
            (ZZ_CAPTURE_PHASE_READY | ZZ_CAPTURE_PHASE_DONE) &&
            !(status & (ZZ_CAPTURE_PHASE_BUSY | ZZ_CAPTURE_PHASE_ERROR)) &&
            zz_capture_phase_decode(status) == target)
            return clock_ready();
        if (!restoring && !continue_test()) return 0;
        Delay(1);
    }
    return fail("The requested sampling phase was not acknowledged in time.");
}

/* Status crosses a 16-bit host bus. Require two identical complete reads to
 * avoid joining a new field sequence to the previous snapshot's flags. */
static int snapshot_status(ULONG *result)
{
    unsigned attempt;
    for (attempt = 0; attempt < 8; ++attempt) {
        ULONG first = read32(ZZ_CAPTURE_SNAPSHOT_REG);
        ULONG second = read32(ZZ_CAPTURE_SNAPSHOT_REG);
        if (first == second) {
            *result = first;
            return 1;
        }
    }
    return fail("Capture status would not remain stable for a read.");
}

static int take_snapshot_options(struct snapshot *sample, int target,
    int read_pixels, int read_metadata, int require_phase)
{
    ULONG before, status, after;
    unsigned tick, i;
    if (!continue_test() ||
        !(require_phase ? capability() : observation_capability()) ||
        (require_phase && !phase_is(target)) ||
        !snapshot_status(&before)) return 0;
    if (before & ZZ_CAPTURE_SNAPSHOT_BUSY)
        return fail("Another capture is already running. Close other diagnostic tools.");
    zz9000_write_reg16(board.address, ZZ_CAPTURE_ARM_REG, ZZ_CAPTURE_ARM_TOKEN);
    for (tick = 0; tick < FRAME_WAIT_TICKS; ++tick) {
        if (!continue_test() || (require_phase && !phase_is(target)) ||
            !snapshot_status(&status))
            return 0;
        if ((status & (ZZ_CAPTURE_SNAPSHOT_VALID | ZZ_CAPTURE_SNAPSHOT_BUSY)) ==
                ZZ_CAPTURE_SNAPSHOT_VALID &&
            ((status ^ before) & ZZ_CAPTURE_SNAPSHOT_ARM) &&
            (!(before & ZZ_CAPTURE_SNAPSHOT_VALID) || (status >> 16) != (before >> 16)))
            break;
        Delay(1);
    }
    if (tick == FRAME_WAIT_TICKS)
        return fail("No fresh native-video capture arrived in time.");
    sample->status = status;
    sample->geometry = read32(ZZ_CAPTURE_GEOMETRY_REG);
    if (require_phase &&
        (!!(status & ZZ_CAPTURE_SNAPSHOT_NTSC) != wanted_ntsc ||
         !!(status & ZZ_CAPTURE_SNAPSHOT_LACE) != wanted_lace))
        return fail("The captured PAL/NTSC or interlace mode does not match the test screen.");
    if (have_geometry && sample->geometry != geometry)
        return fail("Capture framing changed during calibration. Close other video controls.");
    if (read_pixels) {
        for (i = 0; i < ZZ_CAPTURE_SAMPLES; ++i) {
            zz9000_write_reg16(board.address, ZZ_CAPTURE_ADDRESS_REG, (UWORD)i);
            sample->pixels[i] = read32(ZZ_CAPTURE_DATA_REG);
        }
    }
    if (read_metadata) {
        for (i = 0; i < ZZ_CAPTURE_METADATA_WORDS; ++i) {
            zz9000_write_reg16(board.address, ZZ_CAPTURE_METADATA_ADDR_REG,
                (UWORD)i);
            sample->metadata[i] = read32(ZZ_CAPTURE_METADATA_DATA_REG);
        }
    }
    if (!snapshot_status(&after) || (require_phase && !phase_is(target))) return 0;
    if (after != status || sample->geometry != read32(ZZ_CAPTURE_GEOMETRY_REG))
        return fail("The capture changed while being read. Close other diagnostic tools.");
    if (!have_geometry) {
        geometry = sample->geometry;
        have_geometry = 1;
    }
    return 1;
}

static int take_snapshot(struct snapshot *sample, int target, int read_pixels)
{
    return take_snapshot_options(sample, target, read_pixels, read_pixels, 1);
}

static int measure_phase(int target, unsigned count, struct score *score)
{
    unsigned have_previous[2] = {0, 0};
    unsigned last_sample[2] = {0, 0};
    const unsigned field_limit = 6 * (count + 1);
    /* Each needed parity must arrive within field_limit captures. It can
     * refresh that deadline only count times before its next sample completes
     * the quota, so (count + 1) * field_limit also bounds sparse progress. */
    const unsigned total_limit = field_limit * (wanted_lace ? count + 1 : 1);
    unsigned captured, parity, i, last_parity = 2;
    memset(&coverage, 0, sizeof(coverage));
    coverage.phase = target;
    if (first_failure.enabled) ++first_failure.measurement;
    score->wrong = score->changed = 0;
    if (!apply_phase(target, 0)) return 0;
    /* Two complete post-acknowledgement captures are deliberately discarded. */
    for (i = 0; i < 2; ++i)
        if (!take_snapshot(&current, target, 0)) return 0;
    /* Per-parity counters require ten comparisons of EACH interlaced field,
     * not ten accidental comparisons between opposite fields. */
    for (captured = 0; captured < total_limit; ++captured) {
        unsigned wrong, changed = 0;
        if (!take_snapshot(&current, target, 1)) return 0;
        parity = wanted_lace ? !!(current.status & ZZ_CAPTURE_SNAPSHOT_PARITY) : 0;
        if (captured) {
            unsigned step = ((current.status >> 16) - coverage.last_sequence) & 0xffffU;
            if (captured == 1 || step < coverage.min_step) coverage.min_step = step;
            if (step > coverage.max_step) coverage.max_step = step;
        } else coverage.first_sequence = current.status >> 16;
        coverage.last_sequence = current.status >> 16;
        ++coverage.samples[parity];
        last_sample[parity] = captured + 1;
        wrong = zz_capture_pattern_errors(current.pixels);
        score->wrong += wrong;
        if (have_previous[parity]) {
            changed = zz_capture_changed_pixels(
                current.pixels, previous[parity].pixels);
            score->changed += changed;
            ++coverage.comparisons[parity];
        }
        if (first_failure.enabled && !first_failure.valid && (wrong || changed)) {
            first_failure.failed = current;
            first_failure.have_reference = have_previous[parity];
            if (have_previous[parity]) first_failure.reference = previous[parity];
            first_failure.phase = target;
            first_failure.captured_measurement = first_failure.measurement;
            first_failure.sample = captured + 1;
            first_failure.parity = parity;
            first_failure.wrong = wrong;
            first_failure.changed = changed;
            capture_failure_context();
            first_failure.valid = 1;
        }
        previous[parity] = current;
        have_previous[parity] = 1;
        if (coverage.comparisons[0] >= count &&
                (!wanted_lace || coverage.comparisons[1] >= count))
            return 1;
        /* Each needed field keeps its own no-progress deadline. Completed
         * fields cannot prolong a stalled field; every sample still scores. */
        for (i = 0; i < (wanted_lace ? 2U : 1U); ++i)
            if (coverage.comparisons[i] < count &&
                    captured + 1 - last_sample[i] >= field_limit)
                return fail("Both interlaced fields were not captured often enough.");
        /* Snapshot polling and Zorro reads can repeatedly skip an even
         * number of fields. Wait for the next vertical blank when the same
         * parity repeats. DOS Delay(1) waits a duration, which can round to
         * two fields and preserve starvation. Keep every scored sample and
         * the existing bounded coverage requirement. The native test screen
         * and graphics.library remain open throughout measurement. */
        if (wanted_lace && parity == last_parity && coverage.comparisons[parity ^ 1] < count) {
            WaitTOF();
            ++coverage.cadence_waits;
        }
        last_parity = parity;
    }
    return fail("Both interlaced fields were not captured often enough.");
}

static void print_coverage(void)
{
    if (!coverage.samples[0] && !coverage.samples[1]) return;
    printf("Field coverage at phase %d: samples %u/%u, comparisons %u/%u; sequences %u..%u, steps %u..%u; cadence waits %u.\n",
        coverage.phase, coverage.samples[0], coverage.samples[1],
        coverage.comparisons[0], coverage.comparisons[1],
        coverage.first_sequence, coverage.last_sequence,
        coverage.min_step, coverage.max_step, coverage.cadence_waits);
}

static void print_row_timing(const char *name, const struct snapshot *sample,
    unsigned y)
{
    uint32_t identity = sample->metadata[y * 3];
    uint32_t timing = sample->metadata[y * 3 + 1];
    uint32_t context = sample->metadata[y * 3 + 2];
    printf("Timing %s y=%u raw_y=%lu history=%u grid_seen=%u pair=%u grid=%lu "
           "hsync_low=%lu shortlines=%lu timestamp=%lu interval=%lu "
           "sample_x=%lu phase_x=%lu grid_pair_first=%u full_width=%u "
           "sample_mode=%lu fullrate=%u csync_vsync=%u rgb_mode=%lu "
           "raw=%08lx/%08lx/%08lx\n",
        name, y,
        (unsigned long)((identity >> ZZ_CAPTURE_ROW_RAW_Y_SHIFT) &
            ZZ_CAPTURE_ROW_RAW_Y_MASK),
        !!(identity & ZZ_CAPTURE_ROW_HISTORY_VALID),
        !!(identity & ZZ_CAPTURE_ROW_GRID_SEEN),
        !!(identity & ZZ_CAPTURE_ROW_PAIR_PARITY),
        (unsigned long)((identity >> ZZ_CAPTURE_ROW_GRID_SHIFT) &
            ZZ_CAPTURE_ROW_GRID_MASK),
        (unsigned long)((identity >> ZZ_CAPTURE_ROW_HSYNC_SHIFT) &
            ZZ_CAPTURE_ROW_HSYNC_MASK),
        (unsigned long)((identity >> ZZ_CAPTURE_ROW_SHORT_SHIFT) &
            ZZ_CAPTURE_ROW_SHORT_MASK),
        (unsigned long)((timing >> ZZ_CAPTURE_ROW_TIME_SHIFT) &
            ZZ_CAPTURE_ROW_TIME_MASK),
        (unsigned long)(timing & ZZ_CAPTURE_ROW_INTERVAL_MASK),
        (unsigned long)((context >> ZZ_CAPTURE_ROW_SAMPLE_X_SHIFT) &
            ZZ_CAPTURE_ROW_SAMPLE_X_MASK),
        (unsigned long)((context >> ZZ_CAPTURE_ROW_PHASE_X_SHIFT) &
            ZZ_CAPTURE_ROW_PHASE_X_MASK),
        !!(context & ZZ_CAPTURE_ROW_PAIR_FIRST),
        !!(context & ZZ_CAPTURE_ROW_FULL_WIDTH),
        (unsigned long)((context >> ZZ_CAPTURE_ROW_MODE_SHIFT) &
            ZZ_CAPTURE_ROW_MODE_MASK),
        !!(context & ZZ_CAPTURE_ROW_FULLRATE),
        !!(context & ZZ_CAPTURE_ROW_CSYNC_VSYNC),
        (unsigned long)(context & ZZ_CAPTURE_ROW_RGB_MODE_MASK),
        (unsigned long)identity, (unsigned long)timing,
        (unsigned long)context);
}

static void print_snapshot_evidence(const char *name, const struct snapshot *sample)
{
    struct zz_capture_row_analysis row;
    unsigned y, i, x;
    printf("Snapshot %s status=%08lx geometry=%08lx pattern_wrong=%u\n", name,
        (unsigned long)sample->status, (unsigned long)sample->geometry,
        zz_capture_pattern_errors(sample->pixels));
    for (y = 0; y < ZZ_CAPTURE_ROWS; ++y) {
        zz_capture_analyze_row(sample->pixels + y * ZZ_CAPTURE_COLUMNS, &row);
        printf("Row %s y=%u origin=%d ties=%u residual_pixels=%u ",
            name, y, row.origin, row.ties, row.residual_pixels);
        if (row.origin >= 0)
            printf("residual_bits=%u bit_mask=%06lx\n", row.residual_bits,
                (unsigned long)row.bit_mask);
        else puts("residual_bits=unknown bit_mask=unknown");
        print_row_timing(name, sample, y);
    }
    for (i = 0; i < ZZ_CAPTURE_SAMPLES; i += 8) {
        printf("RAW %s %04u:", name, i);
        for (x = 0; x < 8; ++x) printf(" %08lx", (unsigned long)sample->pixels[i + x]);
        printf("\n");
    }
}

/* Called only after restoration and resource cleanup, including on abort.
 * The reference may itself be bad; its strict pattern score is printed too.
 * A first-sample failure has no temporal reference and must say so. */
static void print_failure_evidence(void)
{
    if (!first_failure.valid) {
        puts("Failure evidence: none (no scored pixel failure retained).");
        return;
    }
    printf("Failure evidence v2: measurement=%u sample=%u phase=%d parity=%u reference=%u wrong=%u changed=%u\n",
        first_failure.captured_measurement, first_failure.sample, first_failure.phase,
        first_failure.parity, first_failure.have_reference,
        first_failure.wrong, first_failure.changed);
    printf("Capture context metadata_cap=0x%08lx field_diag_cap=0x%08lx "
           "build=0x%08lx variant=0x%08lx clock=0x%08lx counts=0x%08lx "
           "phase_status=0x%08lx\n",
        (unsigned long)first_failure.metadata_capability,
        (unsigned long)first_failure.field_diag_capability,
        (unsigned long)first_failure.field_diag_build,
        (unsigned long)first_failure.field_diag_variant,
        (unsigned long)first_failure.clock_status,
        (unsigned long)first_failure.clock_counts,
        (unsigned long)first_failure.phase_status);
    puts("Diagnostic row alignment only; strict scores above remain authoritative.\n"
         "Origins are modulo 256; -1 means ambiguous. Bit metrics use a unique best origin.\n"
         "Timing words are frozen with each row; raw words follow in capture order.\n"
         "Reference is the previous scored snapshot of this parity at the same phase, if present.");
    print_snapshot_evidence("failed", &first_failure.failed);
    if (first_failure.have_reference) print_snapshot_evidence("reference", &first_failure.reference);
    puts("End failure evidence v2.");
}

static int valid_mode(ULONG id, int ntsc, int lace)
{
    struct DisplayInfo info;
    struct DimensionInfo dimensions;
    memset(&info, 0, sizeof(info));
    memset(&dimensions, 0, sizeof(dimensions));
    return ModeNotAvailable(id) == 0 &&
        GetDisplayInfoData(NULL, (UBYTE *)&info, sizeof(info), DTAG_DISP, id) != 0 &&
        GetDisplayInfoData(NULL, (UBYTE *)&dimensions, sizeof(dimensions), DTAG_DIMS, id) != 0 &&
        !(info.PropertyFlags & (DIPF_IS_FOREIGN | DIPF_IS_HAM | DIPF_IS_DUALPF |
                               DIPF_IS_EXTRAHALFBRITE | DIPF_IS_SCANDBL)) &&
        !!(info.PropertyFlags & DIPF_IS_PAL) == !ntsc &&
        !!(info.PropertyFlags & DIPF_IS_LACE) == lace &&
        info.RedBits == 8 && info.GreenBits == 8 && info.BlueBits == 8 &&
        dimensions.MaxDepth >= 8;
}

static void draw_message(const char *text)
{
    static const char title[] = "ZZ9000 capture calibration - Esc cancels";
    struct RastPort *rp = &screen->RastPort;
    SetAPen(rp, dark_pen);
    RectFill(rp, 0, 0, screen->Width - 1, 23);
    SetAPen(rp, light_pen);
    SetDrMd(rp, JAM1);
    Move(rp, 12, 10);
    Text(rp, (CONST_STRPTR)title, sizeof(title) - 1);
    Move(rp, 12, 21);
    Text(rp, (CONST_STRPTR)text, (ULONG)strlen(text));
    WaitBlit();
}

static void close_screen(void)
{
    if (window) { CloseWindow(window); window = NULL; }
    if (empty_pointer) { FreeMem(empty_pointer, 16); empty_pointer = NULL; }
    if (screen) { CloseScreen(screen); screen = NULL; }
    if (IntuitionBase) { CloseLibrary((struct Library *)IntuitionBase); IntuitionBase = NULL; }
    if (GfxBase) { CloseLibrary((struct Library *)GfxBase); GfxBase = NULL; }
}

static int open_screen(void)
{
    ULONG id, key, error = 0;
    struct BitMap *bitmap;
    unsigned pen, plane, x, y, byte, bit;
    unsigned darkest = UINT_MAX, brightest = 0;
    GfxBase = (struct GfxBase *)OpenLibrary((CONST_STRPTR)"graphics.library", 39);
    IntuitionBase = (struct IntuitionBase *)OpenLibrary((CONST_STRPTR)"intuition.library", 39);
    if (!GfxBase || !IntuitionBase)
        return fail("AmigaOS 3 graphics and Intuition are required.");
    if ((GfxBase->ChipRevBits0 & (GFXF_AA_ALICE | GFXF_AA_LISA)) !=
        (GFXF_AA_ALICE | GFXF_AA_LISA))
        return fail("This test requires AGA native video on an A4000 with the C28 build.");
    key = wanted_lace ? SUPERLACE_KEY : SUPER_KEY;
    id = (wanted_ntsc ? NTSC_MONITOR_ID : PAL_MONITOR_ID) | key;
    if (!valid_mode(id, wanted_ntsc, wanted_lace)) {
        /* Default monitor is allowed only when its actual native standard
         * and full 24-bit palette match the explicitly requested mode. */
        id = key;
        if (!valid_mode(id, wanted_ntsc, wanted_lace))
            return fail("Requested native SuperHires 256-color mode is unavailable. Load its PAL/NTSC monitor.");
    }
    screen = OpenScreenTags(NULL, SA_Type, CUSTOMSCREEN,
        SA_DisplayID, id, SA_Width, 1280,
        SA_Height, (wanted_ntsc ? 200 : 256) * (wanted_lace ? 2 : 1),
        SA_Depth, 8, SA_Interleaved, FALSE, SA_Quiet, TRUE,
        SA_ShowTitle, FALSE, SA_Behind, TRUE, SA_AutoScroll, FALSE,
        SA_ErrorCode, (ULONG)&error, TAG_END);
    if (!screen) {
        snprintf(failure, sizeof(failure),
            "Could not open the native test screen (error %lu). Free some Chip RAM.",
            (unsigned long)error);
        return 0;
    }
    if (!valid_mode(GetVPModeID(&screen->ViewPort), wanted_ntsc, wanted_lace) ||
        screen->Width != 1280 || screen->RastPort.BitMap->Depth != 8)
        return fail("The opened screen does not have the required native pixel format.");
    window = OpenWindowTags(NULL, WA_CustomScreen, (ULONG)screen,
        WA_Left, 0, WA_Top, 0, WA_Width, screen->Width, WA_Height, screen->Height,
        WA_Borderless, TRUE, WA_Backdrop, TRUE, WA_RMBTrap, TRUE,
        WA_NoCareRefresh, TRUE, WA_Activate, FALSE, WA_IDCMP, IDCMP_RAWKEY, TAG_END);
    if (!window) return fail("Could not open the test screen's keyboard controls.");
    empty_pointer = (UWORD *)AllocMem(16, MEMF_CHIP | MEMF_CLEAR);
    if (!empty_pointer) return fail("Could not allocate the invisible pointer.");
    SetPointer(window, empty_pointer, 1, 16, 0, 0);
    colors[0] = 256UL << 16;
    for (pen = 0; pen < 256; ++pen) {
        ULONG rgb = zz_capture_pattern_rgb(pen);
        unsigned r = (rgb >> 16) & 255, g = (rgb >> 8) & 255, b = rgb & 255;
        unsigned brightness = 3 * r + 6 * g + b;
        colors[1 + pen * 3] = r * 0x01010101UL;
        colors[2 + pen * 3] = g * 0x01010101UL;
        colors[3 + pen * 3] = b * 0x01010101UL;
        if (brightness < darkest) { darkest = brightness; dark_pen = pen; }
        if (brightness > brightest) { brightest = brightness; light_pen = pen; }
    }
    colors[769] = 0;
    LoadRGB32(&screen->ViewPort, colors);
    bitmap = screen->RastPort.BitMap;
    WaitBlit();
    for (plane = 0; plane < 8; ++plane) {
        UBYTE *first_row = bitmap->Planes[plane];
        for (x = 0; x < 1280; x += 8) {
            byte = 0;
            for (bit = 0; bit < 8; ++bit)
                if ((x + bit) & (1U << plane)) byte |= 0x80U >> bit;
            first_row[x / 8] = (UBYTE)byte;
        }
        for (y = 1; y < (unsigned)screen->Height; ++y)
            memcpy(first_row + y * bitmap->BytesPerRow, first_row, 1280 / 8);
    }
    draw_message("Preparing the static full-detail test image...");
    ScreenToFront(screen);
    ActivateWindow(window);
    Delay(10);
    return 1;
}

static void print_info(void)
{
    ULONG status = read32(ZZ_CAPTURE_PHASE_STATUS_REG);
    ULONG clocks = read32(ZZ_CAPTURE_CLOCK_STATUS_REG);
    ULONG counts = read32(ZZ_CAPTURE_CLOCK_COUNTS_REG);
    printf("Board: Zorro %u at 0x%08lx; firmware 0x%04x\n",
        (unsigned)board.zorro_version, (unsigned long)board.address,
        (unsigned)zz9000_read_reg16(board.address, ZZ_REG_FW_VERSION));
    printf("Capture capability: 0x%08lx; required 0x%08lx\n",
        (unsigned long)read32(ZZ_CAPTURE_CAP_REG), (unsigned long)ZZ_CAPTURE_CAP_C28);
    printf("Row metadata capability: 0x%08lx; required 0x%08lx\n",
        (unsigned long)read32(ZZ_CAPTURE_METADATA_CAP_REG),
        (unsigned long)ZZ_CAPTURE_METADATA_CAP);
    printf("Field diagnostic: capability=0x%08lx build=0x%08lx variant=0x%08lx\n",
        (unsigned long)read32(ZZ_CAPTURE_FIELD_DIAG_CAP_REG),
        (unsigned long)read32(ZZ_CAPTURE_FIELD_DIAG_BUILD_REG),
        (unsigned long)read32(ZZ_CAPTURE_FIELD_DIAG_VARIANT_REG));
    printf("Clock: C28=%u, frequency valid=%u, locked=%u, ready=%u, fault=%u\n",
        !!(clocks & ZZ_CAPTURE_CLOCK_C28), !!(clocks & ZZ_CAPTURE_CLOCK_FREQUENCY),
        !!(clocks & ZZ_CAPTURE_CLOCK_LOCKED), !!(clocks & ZZ_CAPTURE_CLOCK_READY),
        !!(clocks & ZZ_CAPTURE_CLOCK_FAULT));
    printf("Edges per 1 ms: C28=%lu, E7M=%lu\n",
        (unsigned long)(counts & 0xffff), (unsigned long)(counts >> 16));
    printf("Phase: %d; busy=%u, done=%u, error=%u, ready=%u\n",
        zz_capture_phase_decode(status), !!(status & ZZ_CAPTURE_PHASE_BUSY),
        !!(status & ZZ_CAPTURE_PHASE_DONE), !!(status & ZZ_CAPTURE_PHASE_ERROR),
        !!(status & ZZ_CAPTURE_PHASE_READY));
    if (snapshot_status(&status))
        printf("Last snapshot: valid=%u, busy=%u, sequence=%lu, parity=%u, lace=%u, ntsc=%u (frozen metadata).\n",
            !!(status & ZZ_CAPTURE_SNAPSHOT_VALID), !!(status & ZZ_CAPTURE_SNAPSHOT_BUSY),
            (unsigned long)(status >> 16), !!(status & ZZ_CAPTURE_SNAPSHOT_PARITY),
            !!(status & ZZ_CAPTURE_SNAPSHOT_LACE), !!(status & ZZ_CAPTURE_SNAPSHOT_NTSC));
}

static int observe_rows(void)
{
    ULONG capture, metadata, diagnostic, build, variant, clocks, counts;
    unsigned y;
    memset(&current, 0, sizeof(current));
    if (!take_snapshot_options(&current, 0, 0, 1, 0)) {
        puts(failure);
        return 20;
    }
    capture = read32(ZZ_CAPTURE_CAP_REG);
    metadata = read32(ZZ_CAPTURE_METADATA_CAP_REG);
    diagnostic = read32(ZZ_CAPTURE_FIELD_DIAG_CAP_REG);
    build = read32(ZZ_CAPTURE_FIELD_DIAG_BUILD_REG);
    variant = read32(ZZ_CAPTURE_FIELD_DIAG_VARIANT_REG);
    clocks = read32(ZZ_CAPTURE_CLOCK_STATUS_REG);
    counts = read32(ZZ_CAPTURE_CLOCK_COUNTS_REG);
    printf("Board: Zorro %u at 0x%08lx; firmware 0x%04x\n",
        (unsigned)board.zorro_version, (unsigned long)board.address,
        (unsigned)zz9000_read_reg16(board.address, ZZ_REG_FW_VERSION));
    printf("Observation context capture_cap=0x%08lx metadata_cap=0x%08lx "
           "field_diag_cap=0x%08lx build=0x%08lx variant=0x%08lx "
           "clock=0x%08lx counts=0x%08lx\n",
        (unsigned long)capture, (unsigned long)metadata,
        (unsigned long)diagnostic, (unsigned long)build,
        (unsigned long)variant, (unsigned long)clocks,
        (unsigned long)counts);
    printf("Snapshot observe status=%08lx geometry=%08lx\n",
        (unsigned long)current.status, (unsigned long)current.geometry);
    for (y = 0; y < ZZ_CAPTURE_ROWS; ++y)
        print_row_timing("observe", &current, y);
    puts("End row metadata observation.");
    return 0;
}

/* ReadEClock measures elapsed time independently of wall-clock adjustments.
 * No asynchronous timer requests are submitted. Keep the device open until
 * after phase restoration and close every resource on partial setup failure. */
struct startup_timer {
    struct MsgPort *port;
    struct timerequest *request;
    ULONG frequency;
    uint64_t origin, last;
};

static uint64_t eclock_value(const struct EClockVal *value)
{
    return (uint64_t)value->ev_hi << 32 | value->ev_lo;
}

static int open_startup_timer(struct startup_timer *timer)
{
    struct EClockVal value;
    timer->port = CreateMsgPort();
    if (!timer->port) return fail("Could not create the startup timer port.");
    timer->request = (struct timerequest *)CreateIORequest(timer->port, sizeof(*timer->request));
    if (!timer->request) return fail("Could not allocate the startup timer request.");
    if (OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_ECLOCK, (struct IORequest *)timer->request, 0))
        return fail("Could not open timer.device for startup timestamps.");
    TimerBase = timer->request->tr_node.io_Device;
    timer->frequency = ReadEClock(&value);
    if (!timer->frequency) return fail("The startup timer has no E-clock frequency.");
    timer->origin = timer->last = eclock_value(&value);
    return 1;
}

static void close_startup_timer(struct startup_timer *timer)
{
    if (TimerBase) {
        CloseDevice((struct IORequest *)timer->request);
        TimerBase = NULL;
    }
    if (timer->request) DeleteIORequest((struct IORequest *)timer->request);
    if (timer->port) DeleteMsgPort(timer->port);
}

static int startup_elapsed(struct startup_timer *timer, unsigned long *milliseconds)
{
    struct EClockVal value;
    uint64_t now, delta;
    if (ReadEClock(&value) != timer->frequency)
        return fail("The startup timer frequency changed.");
    now = eclock_value(&value);
    if (now < timer->last) return fail("The startup timer moved backwards.");
    timer->last = now;
    delta = now - timer->origin;
    /* Runs longer than an hour indicate a stalled test or unusable timing. */
    if (delta / timer->frequency >= 3600)
        return fail("The startup timer exceeded the one-hour safety bound.");
    *milliseconds = (unsigned long)(delta * 1000 / timer->frequency);
    return 1;
}

struct startup_telemetry {
    ULONG clock, counts, phase;
};

static void startup_telemetry(struct startup_telemetry *sample)
{
    sample->clock = read32(ZZ_CAPTURE_CLOCK_STATUS_REG);
    sample->counts = read32(ZZ_CAPTURE_CLOCK_COUNTS_REG);
    sample->phase = read32(ZZ_CAPTURE_PHASE_STATUS_REG);
}

static int startup_measure(struct startup_timer *timer, unsigned row, const char *role,
    int phase, unsigned long *elapsed, unsigned *pixel_failures)
{
    struct score score;
    struct startup_telemetry before, after;
    unsigned long start, end;
    int measured, timed;
    char saved_failure[sizeof(failure)];
    if (!startup_elapsed(timer, &start)) return 0;
    startup_telemetry(&before);
    measured = measure_phase(phase, RETEST_COMPARISONS, &score);
    snprintf(saved_failure, sizeof(saved_failure), "%s", failure);
    startup_telemetry(&after);
    timed = startup_elapsed(timer, &end);
    if (timed && end <= start) timed = fail("The startup timer did not advance during measurement.");
    printf("Startup row=%u role=%s phase=%d start_ms=%lu ", row, role, phase, start);
    if (timed) printf("end_ms=%lu ", end);
    else printf("end_ms=unknown ");
    printf("complete=%u wrong=%lu changed=%lu\n", measured && timed, score.wrong, score.changed);
    printf("Before: clock=0x%08lx counts=0x%08lx phase=0x%08lx; after: clock=0x%08lx counts=0x%08lx phase=0x%08lx\n",
        (unsigned long)before.clock, (unsigned long)before.counts, (unsigned long)before.phase,
        (unsigned long)after.clock, (unsigned long)after.counts, (unsigned long)after.phase);
    if (have_geometry) printf("Framing: H=%lu V=%lu.\n",
        (unsigned long)(geometry & 0xfff), (unsigned long)((geometry >> 12) & 0xfff));
    print_coverage();
    fflush(stdout);
    if (!measured) return fail(saved_failure);
    if (!timed) return 0;
    *elapsed = end;
    if (score.wrong || score.changed) ++*pixel_failures;
    return 1;
}

static int startup_test(int entry, int reverse)
{
    struct startup_timer timer = {0};
    unsigned pair, row = 0, pixel_failures = 0;
    unsigned long elapsed = 0;
    int complete = 0, restore_ok = 1, restore_needed = 0;
    char saved_failure[sizeof(failure)];
    memset(&first_failure, 0, sizeof(first_failure));
    first_failure.enabled = 1;
    printf("Startup diagnostic: %s progressive, entry=%d, minus=%d, plus=%d, order=%s.\n",
        wanted_ntsc ? "NTSC" : "PAL", entry, zz_capture_phase_wrap(entry - 28),
        zz_capture_phase_wrap(entry + 28), reverse ? "plus-first" : "minus-first");
    puts("One screen for 180 seconds, ending after a baseline check. Esc or Ctrl-C cancels.\n"
         "Times are milliseconds from timer start BEFORE screen setup, not from power-on.\n"
         "Each row reapplies its phase, discards two captures, then makes 50 comparisons.\n"
         "Pixel failures are recorded and testing continues. No phase is selected or saved.");
    print_info();
    fflush(stdout);
    if (!open_startup_timer(&timer) || !open_screen()) goto finished;
    restore_needed = 1;
    draw_message("Startup diagnostic: leave this screen in front for three minutes...");
    if (!startup_measure(&timer, ++row, "baseline", entry, &elapsed, &pixel_failures)) goto finished;
    for (pair = 0; pair < STARTUP_MAX_PAIRS && elapsed < STARTUP_DURATION_MS; ++pair) {
        int plus = (pair & 1) ^ reverse;
        int phase = zz_capture_phase_wrap(entry + (plus ? 28 : -28));
        if (!startup_measure(&timer, ++row, plus ? "plus" : "minus", phase,
                &elapsed, &pixel_failures) ||
            !startup_measure(&timer, ++row, "baseline", entry, &elapsed, &pixel_failures))
            goto finished;
    }
    if (elapsed < STARTUP_DURATION_MS) fail("Startup measurement limit reached before three minutes elapsed.");
    else complete = 1;
finished:
    snprintf(saved_failure, sizeof(saved_failure), "%s", failure);
    if (restore_needed) restore_ok = apply_phase(entry, 1);
    close_screen();
    close_startup_timer(&timer);
    if (complete) printf("Startup diagnostic completed: %u rows, %u rows with pixel errors, elapsed_ms=%lu.\n",
        row, pixel_failures, elapsed);
    else printf("Startup diagnostic stopped: %s\n", saved_failure);
    if (!restore_needed) puts("Entry phase unchanged; measurements did not start.");
    else if (restore_ok) printf("Original phase %d restored and acknowledged. Nothing saved.\n", entry);
    else printf("RESTORE FAILED: %s Current phase is unknown; cold-boot to restore saved settings.\n", failure);
    print_failure_evidence();
    return complete && restore_ok ? 0 : 20;
}

static int check_current_phase(int entry)
{
    struct score score = {0, 0};
    char saved_failure[sizeof(failure)];
    int success = 0, restore_ok;
    memset(&first_failure, 0, sizeof(first_failure));
    first_failure.enabled = 1;
    printf("Checking native %s SuperHires %s at current phase %d. Nothing is saved.\n",
        wanted_ntsc ? "NTSC" : "PAL", wanted_lace ? "interlace" : "progressive", entry);
    fflush(stdout);
    if (open_screen() && measure_phase(entry, RETEST_COMPARISONS, &score)) {
        if (score.wrong || score.changed)
            fail("Current phase has wrong or changing pixels.");
        else success = 1;
    }
    snprintf(saved_failure, sizeof(saved_failure), "%s", failure);
    restore_ok = apply_phase(entry, 1);
    close_screen();
    print_coverage();
    printf("Expected-pixel errors: %lu; changed pixels: %lu.\n", score.wrong, score.changed);
    if (!success) puts(saved_failure);
    else puts("Current phase passed the longer raw-pixel check.");
    if (restore_ok) printf("Original phase %d restored and acknowledged. Nothing saved.\n", entry);
    else printf("RESTORE FAILED: %s Current phase is unknown; cold-boot to restore saved settings.\n", failure);
    print_failure_evidence();
    return success && restore_ok ? 0 : 20;
}

static int calibrate(int entry)
{
    unsigned char clean[ZZ_CAPTURE_BINS];
    struct score scores[ZZ_CAPTURE_BINS], retest;
    struct zz_capture_eye eye;
    struct zz_capture_boundary edges[2];
    unsigned i, measured = 0;
    unsigned edge, refinement_tests = 0;
    int success = 0, restore_ok;
    int first_clean = 0, last_clean = 0;
    char message[120], saved_failure[sizeof(failure)];
    first_failure.enabled = 0; /* Sweeps deliberately visit bad phases. */
    memset(clean, 0, sizeof(clean));
    memset(scores, 0, sizeof(scores));
    printf("Testing native %s SuperHires %s. Keep the test screen in front.\n",
        wanted_ntsc ? "NTSC" : "PAL", wanted_lace ? "interlace" : "progressive");
    printf("Esc or Ctrl-C cancels and restores phase %d. Nothing is saved.\n", entry);
    fflush(stdout);
    if (!open_screen()) goto finished;
    for (i = 0; i < ZZ_CAPTURE_BINS; ++i) {
        int phase = ZZ_CAPTURE_PHASE_MIN + (int)(i * ZZ_CAPTURE_BIN_STEPS);
        snprintf(message, sizeof(message), "Checking sampling position %u of %u...",
            i + 1, ZZ_CAPTURE_BINS);
        draw_message(message);
        if (!measure_phase(phase, SWEEP_COMPARISONS, &scores[i])) goto finished;
        ++measured;
        clean[i] = scores[i].wrong == 0 && scores[i].changed == 0;
    }
    if (!zz_capture_select_eye(clean, entry, &eye)) {
        fail("No sufficiently wide clean interval. Check capture framing, the video cable, and the C28 build.");
        goto finished;
    }
    if (eye.full_circle) {
        fail("Every position passed; no sampling boundary was measured. Keep the original phase and investigate.");
        goto finished;
    }
    /* The adjacent coarse bins are already known bad. Search each 28-step
     * bad/good bracket without converting to a signed circular distance,
     * which would lose which side of the interval is being refined. */
    first_clean = ZZ_CAPTURE_PHASE_MIN + (int)(eye.start_bin * ZZ_CAPTURE_BIN_STEPS);
    last_clean = first_clean + (int)((eye.bins - 1) * ZZ_CAPTURE_BIN_STEPS);
    edges[0].origin = first_clean - (int)ZZ_CAPTURE_BIN_STEPS;
    edges[0].bad = 0;
    edges[0].good = ZZ_CAPTURE_BIN_STEPS;
    edges[1].origin = last_clean;
    edges[1].good = 0;
    edges[1].bad = ZZ_CAPTURE_BIN_STEPS;
    for (edge = 0; edge < 2; ++edge) {
        while (zz_capture_boundary_pending(&edges[edge])) {
            int clean_test;
            draw_message(edge ? "Measuring the upper edge of the clean interval..." :
                                "Measuring the lower edge of the clean interval...");
            if (!measure_phase(zz_capture_boundary_next(&edges[edge]),
                    SWEEP_COMPARISONS, &retest)) goto finished;
            ++refinement_tests;
            clean_test = retest.wrong == 0 && retest.changed == 0;
            zz_capture_boundary_record(&edges[edge], clean_test);
        }
    }
    first_clean = edges[0].origin + (int)edges[0].good;
    last_clean = edges[1].origin + (int)edges[1].good;
    eye.margin = (unsigned)(last_clean - first_clean) / 2;
    eye.phase = zz_capture_phase_wrap(first_clean + (int)eye.margin);
    draw_message("Checking the center of the clean interval for longer...");
    if (!measure_phase(eye.phase, RETEST_COMPARISONS, &retest)) goto finished;
    if (retest.wrong || retest.changed) {
        fail("The selected position did not remain correct and stable on the longer check.");
        goto finished;
    }
    success = 1;
finished:
    /* Keep the native source running while restoring, including failure
     * after opening the screen. Closing it first can remove needed frames. */
    snprintf(saved_failure, sizeof(saved_failure), "%s", failure);
    restore_ok = success || apply_phase(entry, 1);
    close_screen();
    print_coverage();
    printf("\nPhase   Expected-pixel errors   Changed pixels\n");
    for (i = 0; i < measured; ++i)
        printf("%5d   %21lu   %14lu%s\n",
            ZZ_CAPTURE_PHASE_MIN + (int)(i * ZZ_CAPTURE_BIN_STEPS),
            scores[i].wrong, scores[i].changed, clean[i] ? "  clean" : "");
    if (measured < ZZ_CAPTURE_BINS)
        printf("%u of %u positions completed; remaining positions were not scored.\n",
            measured, ZZ_CAPTURE_BINS);
    if (!success) {
        printf("%s\n", saved_failure);
        if (restore_ok) printf("Original phase %d restored and acknowledged. Nothing saved.\n", entry);
        else printf("RESTORE FAILED: %s Current phase is unknown; cold-boot to restore saved settings.\n", failure);
        return 20;
    }
    printf("\nCapture passed at phase %d: %u clean positions; tested margin %u fine steps on each side.\n",
        eye.phase, eye.bins, eye.margin);
    printf("Refined interval: %d through %d (wraps at +895/-896); %u boundary tests.\n",
        zz_capture_boundary_good_phase(&edges[0]),
        zz_capture_boundary_good_phase(&edges[1]), refinement_tests);
    printf("Long check: %u comparisons per field parity, zero wrong or changing pixels.\n",
        RETEST_COMPARISONS);
    printf("Framing: H=%lu V=%lu. Live phase changed; nothing saved.\n",
        (unsigned long)(geometry & 0xfff), (unsigned long)((geometry >> 12) & 0xfff));
    printf("Candidate setting: videocap_c28_phase = %d\n", eye.phase);
    printf("Repeat cold and warm, then verify other modes before saving this setting.\n");
    printf("To restore the entry phase: ZZCapture phase %d\n", entry);
    return 0;
}

static void usage(void)
{
    puts("ZZCapture info\n"
         "ZZCapture observe\n"
         "ZZCapture phase -896..895\n"
         "ZZCapture check pal|ntsc [lace]\n"
         "ZZCapture startup pal|ntsc [reverse]\n"
         "ZZCapture calibrate pal|ntsc [lace]\n"
         "Run Stack 32768 in this Shell before phase, check, startup or calibrate.\n"
         "Observe is read-only with respect to phase control and supports matched E7M or C28 builds.\n"
         "Phase, check, startup and calibrate require the A4000 C28 diagnostic build.");
}

int main(int argc, char **argv)
{
    ULONG status;
    int entry, target;
    if (argc < 2 || !strcmp(argv[1], "help") || !strcmp(argv[1], "?")) {
        usage();
        return argc < 2 ? 10 : 0;
    }
    if ((strcmp(argv[1], "info") || argc != 2) &&
        (strcmp(argv[1], "observe") || argc != 2) &&
        (strcmp(argv[1], "phase") || argc != 3) &&
        ((strcmp(argv[1], "calibrate") && strcmp(argv[1], "check")) || argc < 3 || argc > 4 ||
         (strcmp(argv[2], "pal") && strcmp(argv[2], "ntsc")) ||
         (argc == 4 && strcmp(argv[3], "lace"))) &&
        (strcmp(argv[1], "startup") || argc < 3 || argc > 4 ||
         (strcmp(argv[2], "pal") && strcmp(argv[2], "ntsc")) ||
         (argc == 4 && strcmp(argv[3], "reverse")))) {
        usage();
        return 10;
    }
    printf("ZZCapture %s\n", ZZ_CAPTURE_VERSION);
    if (!zz9000_find_board(&board)) {
        puts("ZZ9000 board not found.");
        return 20;
    }
    if (!strcmp(argv[1], "info")) { print_info(); return 0; }
    if (!strcmp(argv[1], "observe")) return observe_rows();
    /* Check in the small entry frame before calling calibration or libraries
     * that need more stack, and before any register write or screen setup. */
    if (!stack_ready()) { puts(failure); return 20; }
    if (!capability() || board.zorro_version != 3) {
        puts("This command requires the matched A4000 C28 diagnostic firmware.");
        return 20;
    }
    status = read32(ZZ_CAPTURE_PHASE_STATUS_REG);
    entry = zz_capture_phase_decode(status);
    if (!(status & ZZ_CAPTURE_PHASE_READY) ||
        (status & (ZZ_CAPTURE_PHASE_BUSY | ZZ_CAPTURE_PHASE_ERROR)) ||
        !zz_capture_phase_valid(entry) || !clock_ready()) {
        puts("The capture clock and phase controller must be ready and idle before starting.");
        return 20;
    }
    if (!strcmp(argv[1], "phase")) {
        char *end;
        long parsed;
        errno = 0;
        parsed = strtol(argv[2], &end, 10);
        if (errno || end == argv[2] || *end || parsed < ZZ_CAPTURE_PHASE_MIN ||
            parsed > ZZ_CAPTURE_PHASE_MAX) {
            puts("Phase must be an integer from -896 to 895.");
            return 10;
        }
        target = (int)parsed;
        if (apply_phase(target, 0)) {
            printf("Phase %d applied and acknowledged. Nothing saved.\n", target);
            return 0;
        }
        printf("%s\n", failure);
        if (apply_phase(entry, 1)) printf("Original phase %d restored.\n", entry);
        else printf("RESTORE FAILED: %s Cold-boot to restore saved settings.\n", failure);
        return 20;
    }
    wanted_ntsc = !strcmp(argv[2], "ntsc");
    wanted_lace = argc == 4 && !strcmp(argv[3], "lace");
    if (!strcmp(argv[1], "startup")) return startup_test(entry, argc == 4);
    if (!strcmp(argv[1], "check")) return check_current_phase(entry);
    return calibrate(entry);
}
