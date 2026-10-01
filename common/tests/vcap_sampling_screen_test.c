/*
 * Regression for the interactive C28 sampling-calibration screen-opening
 * path in ZZTop/Sources/vcap_sampling.c.
 *
 * It compiles and drives the REAL vcap_sampling.c (VCAP_SAMPLING_TEST_IO)
 * against a faithful host surface:
 *   - ViewPort.Modes is a true UWORD (16-bit mode word), and the 32-bit
 *     ModeID is a separate GetVPModeID association. The earlier throwaway
 *     sampling smoke stored the full ID in Modes, hiding the native bug.
 *   - the screen raster (BitMap planes) is real memory the message/rect
 *     stubs write and the capture DATA_REG reads, so progress updates are
 *     verified to preserve the sampled rows.
 *   - the invisible-pointer allocation/cleanup (ZZCapture convention) is
 *     tracked and asserted.
 *
 * It proves PAL/NTSC progressive/lace screens are accepted (the run proceeds
 * past screen opening and into calibration, then is cancelled) and that
 * monitor/lace mismatch, invalid, non-native, wrong-width and wrong-depth
 * screens are rejected. It also proves cancel restoration and a complete
 * raster-backed sweep/refinement/retest with Enter acceptance.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vcap_sampling_test_platform.h"
#include "zz9000_capture_calibration.h"
#include "zz_vcap_client.h"
#include "vcap_sampling.h"

#define VCAP_TEST_ENTRY_PHASE 0

/* The 32-bit ModeIDs the cases use (monitor part | SuperHires key). */
#define TEST_PAL_PROG  (PAL_MONITOR_ID | SUPER_KEY)
#define TEST_PAL_LACE  (PAL_MONITOR_ID | SUPERLACE_KEY)
#define TEST_NTSC_PROG (NTSC_MONITOR_ID | SUPER_KEY)
#define TEST_NTSC_LACE (NTSC_MONITOR_ID | SUPERLACE_KEY)

static unsigned checks, failures;
#define CHECK(expr) do { \
    ++checks; \
    if (!(expr)) { \
        ++failures; \
        printf("FAIL %u: %s\n", (unsigned)__LINE__, #expr); \
    } \
} while (0)

/* Display-database entry (keyed by the 32-bit ModeID). */
struct disp {
    ULONG id;
    ULONG flags;
    unsigned max_depth;
    ULONG not_available;
    int present;
};

/* Card-capture / display / screen / pointer mock state. */
struct mock {
    uint32_t cap, metadata_cap, clocks, phase, snapshot, geometry;
    uint16_t target;
    unsigned commits, arms, ack_phase, ack_capture, arm_pending;
    unsigned pixel_index;
    unsigned captures, phase_writes;
    int bounded_eye, accept_pending;
    struct disp disps[8];
    int disp_n;
    int open_screen_ok, open_window_ok;
    unsigned open_screen_calls, open_window_calls, close_screen_calls,
        close_window_calls;
    int alloc_fail;
    unsigned allocs, frees;
    void *pointer;

    void *pointer_set;
    unsigned cancel_after_captures;
};
static struct mock g_mock;

/* Real screen raster (the message/rect stubs write it; the capture reads it). */
static struct Screen g_screen;
static struct Window g_window;
static struct MsgPort g_port;
static struct BitMap g_bitmap;
static UBYTE *g_planes[8];
static unsigned g_apen;

static uint32_t g_raster_pixel(int col, int row)
{
    uint32_t value = 0;
    int p;
    for (p = 0; p < 8; ++p) {
        UBYTE *plane = g_planes[p];
        if (plane[row * g_bitmap.BytesPerRow + col / 8] & (0x80 >> (col % 8)))
            value |= (1UL << p);
    }
    return value;
}

static void g_raster_set_pixel(int col, int row, uint32_t pen)
{
    int p;
    for (p = 0; p < 8; ++p) {
        UBYTE *byte = &g_planes[p][row * g_bitmap.BytesPerRow + col / 8];
        if (pen & (1U << p))
            *byte |= (0x80 >> (col % 8));
        else
            *byte &= ~(0x80 >> (col % 8));
    }
}

static void disp_set(ULONG id, ULONG flags, unsigned max_depth,
    ULONG not_available)
{
    int i;
    for (i = 0; i < g_mock.disp_n; ++i) {
        if (g_mock.disps[i].id == id) {
            g_mock.disps[i].flags = flags;
            g_mock.disps[i].max_depth = max_depth;
            g_mock.disps[i].not_available = not_available;
            g_mock.disps[i].present = 1;
            return;
        }
    }
    if (g_mock.disp_n < (int)(sizeof(g_mock.disps) / sizeof(g_mock.disps[0]))) {
        struct disp *d = &g_mock.disps[g_mock.disp_n];
        d->id = id;
        d->flags = flags;
        d->max_depth = max_depth;
        d->not_available = not_available;
        d->present = 1;
        ++g_mock.disp_n;
    }
}

/* ---- OS function stubs (the faithful surface). ---- */

void *GetMsg(void *port)
{
    static struct IntuiMessage accept;
    (void)port;
    if (!g_mock.accept_pending) return NULL;
    g_mock.accept_pending = 0;
    accept.Class = IDCMP_RAWKEY;
    accept.Code = 0x44;
    return &accept;
}
void ReplyMsg(struct Message *message) { (void)message; }

ULONG SetSignal(ULONG oldmask, ULONG newmask)
{
    (void)oldmask;
    if (newmask != 0)
        return 0; /* clear (SIGBREAKF_CTRL_C) */
    if (g_mock.cancel_after_captures &&
            g_mock.captures >= g_mock.cancel_after_captures)
        return SIGBREAKF_CTRL_C;
    return 0;
}

ULONG GetDisplayInfoData(void *handle, UBYTE *data, ULONG size, ULONG tag,
    ULONG id)
{
    (void)handle;
    int i;
    for (i = 0; i < g_mock.disp_n; ++i) {
        struct disp *d = &g_mock.disps[i];
        if (d->id != id || !d->present)
            continue;
        if (tag == DTAG_DISP) {
            struct DisplayInfo *info = (struct DisplayInfo *)data;
            info->PropertyFlags = d->flags;
            info->RedBits = 8;
            info->GreenBits = 8;
            info->BlueBits = 8;
            info->NotAvailable = d->not_available;
            return size;
        }
        if (tag == DTAG_DIMS) {
            struct DimensionInfo *dim = (struct DimensionInfo *)data;
            dim->MaxDepth = d->max_depth;
            return size;
        }
    }
    return 0;
}

ULONG ModeNotAvailable(ULONG id)
{
    int i;
    for (i = 0; i < g_mock.disp_n; ++i) {
        if (g_mock.disps[i].id == id && g_mock.disps[i].present)
            return g_mock.disps[i].not_available;
    }
    return 1; /* unknown ModeID is unavailable */
}

ULONG GetVPModeID(struct ViewPort *viewport)
{
    return viewport->ModeID;
}

struct Screen *OpenScreenTags(void *base, ...)
{
    (void)base;
    ++g_mock.open_screen_calls;
    return g_mock.open_screen_ok ? &g_screen : NULL;
}
struct Window *OpenWindowTags(void *base, ...)
{
    (void)base;
    ++g_mock.open_window_calls;
    return g_mock.open_window_ok ? &g_window : NULL;
}
int CloseScreen(struct Screen *screen)
{
    (void)screen;
    ++g_mock.close_screen_calls;
    return 1;
}
void CloseWindow(struct Window *window)
{
    (void)window;
    ++g_mock.close_window_calls;
    CHECK(g_mock.pointer_set == g_mock.pointer);
    g_mock.pointer_set = NULL;
}

void *AllocMem(ULONG bytes, ULONG flags)
{
    ++g_mock.allocs;
    if (g_mock.alloc_fail || !(flags & MEMF_CHIP)) return NULL;
    g_mock.pointer = malloc(bytes);
    if (g_mock.pointer != NULL)
        memset(g_mock.pointer, (flags & MEMF_CLEAR) ? 0 : 0xff, bytes);
    return g_mock.pointer;
}
void FreeMem(void *ptr, ULONG bytes)
{
    (void)bytes;
    ++g_mock.frees;
    CHECK(g_mock.pointer_set != ptr); /* No sprite DMA owner remains. */
    if (g_mock.pointer == ptr) g_mock.pointer = NULL;
    free(ptr);
}
void SetPointer(struct Window *window, void *pointer, UWORD hotx,
    UWORD hoty, UWORD width, UWORD height)
{
    (void)window; (void)hotx; (void)hoty; (void)width; (void)height;

    g_mock.pointer_set = pointer;
}

void LoadRGB32(struct ViewPort *viewport, const ULONG *colors)
{
    (void)viewport; (void)colors;
}
void WaitBlit(void) {}
void ScreenToFront(struct Screen *screen) { (void)screen; }
void ActivateWindow(struct Window *window)
{
    if (window == &g_window)
        g_window.Flags |= WFLG_WINDOWACTIVE;
}
void WaitTOF(void) {}
void SetDrMd(struct RastPort *rp, unsigned mode) { (void)rp; (void)mode; }
void SetAPen(struct RastPort *rp, unsigned pen) { (void)rp; g_apen = pen; }
void RectFill(struct RastPort *rp, int x0, int y0, int x1, int y1)
{
    struct BitMap *bm = rp->BitMap;
    int x, y;
    if (bm == NULL)
        return;
    for (y = y0; y <= y1; ++y)
        for (x = x0; x <= x1; ++x)
            g_raster_set_pixel(x, y, g_apen);
}
void Move(struct RastPort *rp, int x, int y) { (void)rp; (void)x; (void)y; }
void Text(struct RastPort *rp, const char *text, ULONG length)
{
    (void)rp; (void)text; (void)length;
}
void Wait(ULONG signals) { (void)signals; g_mock.accept_pending = 1; }
void Delay(ULONG ticks) { (void)ticks; }

/* ---- Card-capture MMIO mock (clean, phase-acked, snapshot-acked). ---- */

static uint32_t *mock_register(uint32_t offset)
{
    switch (offset) {
    case ZZ_CAPTURE_CAP_REG: return &g_mock.cap;
    case ZZ_CAPTURE_METADATA_CAP_REG: return &g_mock.metadata_cap;
    case ZZ_CAPTURE_CLOCK_STATUS_REG: return &g_mock.clocks;
    case ZZ_CAPTURE_PHASE_STATUS_REG: return &g_mock.phase;
    case ZZ_CAPTURE_SNAPSHOT_REG: return &g_mock.snapshot;
    case ZZ_CAPTURE_GEOMETRY_REG: return &g_mock.geometry;
    default: return 0;
    }
}

static uint16_t mock_read16(void *ctx, uint32_t offset)
{
    (void)ctx;
    if (offset == ZZ_CAPTURE_DATA_REG || offset == ZZ_CAPTURE_DATA_REG + 2) {
        int row = 24 + (int)(g_mock.pixel_index / ZZ_CAPTURE_COLUMNS);
        int col = (int)(g_mock.pixel_index % ZZ_CAPTURE_COLUMNS);
        uint32_t pixel = zz_capture_pattern_rgb(g_raster_pixel(col, row));
        int phase = zz_capture_phase_decode((uint16_t)g_mock.phase);
        if (g_mock.pointer_set == NULL && row == 24 && col == 80)
            pixel ^= 0xffffffUL; /* Default visible pointer intersects a sample. */
        if (g_mock.bounded_eye && (phase < -140 || phase > 140)) pixel ^= 1;
        return (offset & 2U) ? (uint16_t)pixel : (uint16_t)(pixel >> 16);
    }
    {
        uint32_t *value = mock_register(offset & ~3U);
        uint32_t raw = value ? *value : 0;
        return (offset & 2U) ? (uint16_t)raw : (uint16_t)(raw >> 16);
    }
}

static void mock_write16(void *ctx, uint32_t offset, uint16_t value)
{
    (void)ctx;
    if (offset == ZZ_CAPTURE_PHASE_TARGET_REG) {
        g_mock.target = value;
    } else if (offset == ZZ_CAPTURE_PHASE_COMMIT_REG) {
        ++g_mock.commits;
        ++g_mock.phase_writes;
    } else if (offset == ZZ_CAPTURE_ARM_REG) {
        ++g_mock.arms;
        g_mock.snapshot ^= ZZ_CAPTURE_SNAPSHOT_ARM;
        g_mock.snapshot &= ~ZZ_CAPTURE_SNAPSHOT_VALID;
        g_mock.snapshot |= ZZ_CAPTURE_SNAPSHOT_BUSY;
        g_mock.arm_pending = 1;
    } else if (offset == ZZ_CAPTURE_ADDRESS_REG) {
        g_mock.pixel_index = value;
        if (value == ZZ_CAPTURE_SAMPLES - 1)
            ++g_mock.captures;
    }
}

static void mock_delay(void *ctx, unsigned ticks)
{
    (void)ctx; (void)ticks;
    if (g_mock.commits && g_mock.ack_phase) {
        g_mock.phase = ZZ_CAPTURE_PHASE_READY | ZZ_CAPTURE_PHASE_DONE |
            (g_mock.target & 0x0fffU);
        g_mock.commits = 0;
    }
    if (g_mock.arm_pending && g_mock.ack_capture) {
        g_mock.snapshot &= ~ZZ_CAPTURE_SNAPSHOT_BUSY;
        g_mock.snapshot |= ZZ_CAPTURE_SNAPSHOT_VALID;
        g_mock.snapshot += 1UL << 16;
        if (g_mock.snapshot & ZZ_CAPTURE_SNAPSHOT_LACE)
            g_mock.snapshot ^= ZZ_CAPTURE_SNAPSHOT_PARITY;
        g_mock.arm_pending = 0;
    }
}

static struct zz_vcap_io mock_io(void)
{
    struct zz_vcap_io io;
    io.ctx = 0;
    io.read16 = mock_read16;
    io.write16 = mock_write16;
    io.delay = mock_delay;
    io.control_ctx = 0;
    io.keep_running = 0; /* vcap_sampling_run installs its own. */
    return io;
}

/* ---- Test cases. ---- */

static void setup_case(int ntsc, int lace)
{
    unsigned p, height = (ntsc ? 200U : 256U) * (lace ? 2U : 1U);
    ULONG id = (ntsc ? NTSC_MONITOR_ID : PAL_MONITOR_ID) |
        (lace ? SUPERLACE_KEY : SUPER_KEY);
    ULONG flags = (ntsc ? 0 : DIPF_IS_PAL) | (lace ? DIPF_IS_LACE : 0);

    for (p = 0; p < 8; ++p) {
        free(g_planes[p]);
        g_planes[p] = calloc(height, 160);
        if (g_planes[p] == NULL) exit(2);
    }
    memset(&g_mock, 0, sizeof(g_mock));
    g_mock.cap = ZZ_CAPTURE_CAP_C28;
    g_mock.metadata_cap = ZZ_CAPTURE_METADATA_CAP;
    g_mock.clocks = ZZ_CAPTURE_CLOCK_FREQUENCY | ZZ_CAPTURE_CLOCK_LOCKED |
        ZZ_CAPTURE_CLOCK_READY | ZZ_CAPTURE_CLOCK_C28;
    g_mock.phase = ZZ_CAPTURE_PHASE_READY | ZZ_CAPTURE_PHASE_DONE |
        zz_capture_phase_encode(VCAP_TEST_ENTRY_PHASE);
    g_mock.snapshot = ZZ_CAPTURE_SNAPSHOT_VALID |
        (lace ? ZZ_CAPTURE_SNAPSHOT_LACE : 0) |
        (ntsc ? ZZ_CAPTURE_SNAPSHOT_NTSC : 0) | (1UL << 16);
    g_mock.geometry = 0x00345067UL;
    g_mock.ack_phase = g_mock.ack_capture = 1;
    g_mock.cancel_after_captures = 13;
    g_mock.open_screen_ok = g_mock.open_window_ok = 1;
    g_screen.Width = 1280;
    g_screen.Height = height;
    g_screen.ViewPort.ModeID = (uint32_t)id;
    g_screen.ViewPort.Modes = (UWORD)id;
    g_screen.RastPort.BitMap = &g_bitmap;
    g_bitmap.Depth = 8;
    g_bitmap.BytesPerRow = 160;
    for (p = 0; p < 8; ++p) g_bitmap.Planes[p] = g_planes[p];
    g_window.Width = 1280;
    g_window.Height = height;
    g_window.Flags = 0; /* Activation is required after fronting the screen. */
    g_window.UserPort = &g_port;
    g_window.RPort = &g_screen.RastPort;
    g_port.mp_SigBit = 0;
    disp_set(id, flags, 8, 0);
}

static int run_sampling(int ntsc, int lace, int *selected,
    unsigned char quality[ZZ_CAPTURE_BINS], char failure[128])
{
    struct zz_vcap_io io = mock_io();
    return vcap_sampling_run(&io, ntsc, lace, VCAP_TEST_ENTRY_PHASE,
        selected, quality, failure, 128);
}

static void check_cancelled_screen(int ntsc, int lace)
{
    char failure[128];
    unsigned char quality[ZZ_CAPTURE_BINS];
    int selected = 123, row, col;
    unsigned damaged = 0;
    CHECK(run_sampling(ntsc, lace, &selected, quality, failure) == 0);
    CHECK(failure[0] == '\0');
    CHECK(selected == 123); /* Cancel does not publish a selection. */
    CHECK(g_mock.captures >= 13 && g_mock.phase_writes >= 2);
    CHECK(g_mock.allocs == 1 && g_mock.frees == 1 && g_mock.pointer == NULL);
    CHECK(g_mock.close_window_calls == 1 && g_mock.close_screen_calls == 1);
    CHECK(zz_capture_phase_decode((uint16_t)g_mock.phase) == VCAP_TEST_ENTRY_PHASE);
    for (row = 24; row < 28; ++row)
        for (col = 0; col < 1280; ++col)
            damaged += g_raster_pixel(col, row) != (uint32_t)(col & 255);
    CHECK(damaged == 0);
}

static void check_rejected(unsigned opened, unsigned window_opened,
    unsigned allocations)
{
    char failure[128];
    unsigned char quality[ZZ_CAPTURE_BINS];
    int selected = 123;
    CHECK(run_sampling(0, 0, &selected, quality, failure) == -1);
    CHECK(selected == 123);
    CHECK(g_mock.phase_writes == 0 && g_mock.captures == 0);
    CHECK(g_mock.open_screen_calls == opened && g_mock.close_screen_calls == opened);
    CHECK(g_mock.open_window_calls == window_opened &&
        g_mock.close_window_calls == window_opened);
    CHECK(g_mock.allocs == allocations && g_mock.frees == 0 && g_mock.pointer == NULL);
}

int main(void)
{
    unsigned ntsc, lace, i;
    char failure[128];
    unsigned char quality[ZZ_CAPTURE_BINS] = {0};
    int selected = 123;

    for (ntsc = 0; ntsc < 2; ++ntsc) for (lace = 0; lace < 2; ++lace) {
        printf("%s %s screen opening/cancel\n", ntsc ? "NTSC" : "PAL",
            lace ? "lace" : "progressive");
        setup_case(ntsc, lace);
        check_cancelled_screen(ntsc, lace);
    }
    setup_case(0, 0);
    g_screen.ViewPort.ModeID = SUPER_KEY; /* Default native monitor association. */
    disp_set(SUPER_KEY, DIPF_IS_PAL, 8, 0);
    check_cancelled_screen(0, 0);

    setup_case(0, 0);
    g_screen.ViewPort.ModeID = TEST_NTSC_PROG;
    disp_set(TEST_NTSC_PROG, 0, 8, 0);
    check_rejected(1, 0, 0);
    setup_case(0, 0);
    g_screen.ViewPort.ModeID = TEST_PAL_LACE;
    disp_set(TEST_PAL_LACE, DIPF_IS_PAL | DIPF_IS_LACE, 8, 0);
    check_rejected(1, 0, 0);
    setup_case(0, 0);
    g_screen.ViewPort.ModeID = UINT32_MAX; /* GetVPModeID failure. */
    check_rejected(1, 0, 0);
    setup_case(0, 0);
    g_screen.ViewPort.ModeID = PAL_MONITOR_ID | 0x8000UL; /* Hires, logical width 1280. */
    disp_set(g_screen.ViewPort.ModeID, DIPF_IS_PAL, 8, 0);
    check_rejected(1, 0, 0);
    setup_case(0, 0);
    disp_set(TEST_PAL_PROG, DIPF_IS_PAL | DIPF_IS_FOREIGN, 8, 0);
    check_rejected(1, 0, 0);
    setup_case(0, 0);
    g_screen.Width = 640;
    check_rejected(1, 0, 0);
    setup_case(0, 0);
    g_bitmap.Depth = 4;
    check_rejected(1, 0, 0);
    setup_case(0, 0);
    disp_set(TEST_PAL_PROG, DIPF_IS_PAL, 4, 0);
    check_rejected(1, 0, 0);
    setup_case(0, 0);
    disp_set(TEST_PAL_PROG, DIPF_IS_PAL, 8, 1);
    check_rejected(0, 0, 0);
    setup_case(0, 0);
    g_mock.alloc_fail = 1;
    check_rejected(1, 1, 1);

    /* Complete the real sweep, refinement, retest and Enter acceptance.
     * Raster-backed captures are clean only in the interval [-140,140]. */
    setup_case(0, 0);
    g_mock.bounded_eye = 1;
    g_mock.cancel_after_captures = 0;
    CHECK(run_sampling(0, 0, &selected, quality, failure) == 1);
    CHECK(selected == 0 && failure[0] == '\0');
    CHECK(zz_capture_phase_decode((uint16_t)g_mock.phase) == selected);
    for (i = 0; i < ZZ_CAPTURE_BINS; ++i) {
        int phase = ZZ_CAPTURE_PHASE_MIN + (int)(i * ZZ_CAPTURE_BIN_STEPS);
        CHECK(quality[i] == (phase >= -140 && phase <= 140));
    }
    CHECK(g_mock.close_window_calls == 1 && g_mock.close_screen_calls == 1);
    CHECK(g_mock.allocs == 1 && g_mock.frees == 1 && g_mock.pointer == NULL);
    for (i = 0; i < 8; ++i) free(g_planes[i]);
    printf("vcap_sampling_screen_test: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
