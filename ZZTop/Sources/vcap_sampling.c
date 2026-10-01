/*
 * Interactive C28 capture-phase sampling calibration.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifdef VCAP_SAMPLING_TEST_IO
#include "vcap_sampling_test_platform.h"
#else
#include <exec/tasks.h>
#include <exec/memory.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <graphics/displayinfo.h>
#include <graphics/modeid.h>
#include <graphics/rastport.h>

#include <clib/dos_protos.h>
#include <clib/exec_protos.h>
#include <clib/graphics_protos.h>
#include <clib/intuition_protos.h>
#endif
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "zz9000_sampling_calibration.h"
#include "vcap_sampling.h"

#define VCAP_RAWKEY_KEYPAD_ENTER 0x43
#define VCAP_RAWKEY_RETURN       0x44
#define VCAP_RAWKEY_ESCAPE       0x45
#define VCAP_SAMPLING_SWEEP_COMPARISONS 10U
#define VCAP_SAMPLING_RETEST_COMPARISONS 50U

struct vcap_sampling_buffers {
    uint32_t current[ZZ_CAPTURE_SAMPLES];
    uint32_t previous[2][ZZ_CAPTURE_SAMPLES];
};

/* These buffers are deliberately static: AmigaOS process stacks are too small
 * for three full frozen-field samples. The screen is modal, so one run owns
 * them at a time. */
static struct vcap_sampling_buffers vcap_sampling_buffers;

struct vcap_sampling_run {
    struct zz_vcap_io io;
    struct Screen *screen;
    struct Window *window;
    UWORD *pointer;
    uint32_t geometry;
    unsigned geometry_valid;
    unsigned ntsc;
    unsigned lace;
    unsigned awaiting_accept;
    unsigned accepted;
    unsigned cancelled;
    unsigned ownership_lost;
};

static void vcap_sampling_set_failure(char *failure, unsigned failure_size,
    const char *message)
{
    if (failure != NULL && failure_size != 0) {
        snprintf(failure, failure_size, "%s", message);
        failure[failure_size - 1] = '\0';
    }
}

static void vcap_sampling_message(struct vcap_sampling_run *run,
    const char *message)
{
    struct RastPort *rp;

    if (run->window == NULL)
        return;
    rp = run->window->RPort;
    SetAPen(rp, 0);
    RectFill(rp, 0, 0, run->window->Width - 1, 23);
    SetAPen(rp, 255);
    SetDrMd(rp, JAM1);
    Move(rp, 12, 21);
    Text(rp, (STRPTR)message, (ULONG)strlen(message));
    WaitBlit();
}

/* This is intentionally the control callback rather than the card callback:
 * the injected IO context remains the caller's MMIO context. */
static int vcap_sampling_keep_running(void *context)
{
    struct vcap_sampling_run *run = context;
    struct IntuiMessage *message;

    if (run == NULL)
        return 0;
    if (SetSignal(0L, 0L) & SIGBREAKF_CTRL_C) {
        SetSignal(0L, SIGBREAKF_CTRL_C);
        run->cancelled = 1;
    }
    if (run->window != NULL) {
        while ((message = (struct IntuiMessage *)GetMsg(run->window->UserPort))) {
            ULONG type = message->Class;
            UWORD code = message->Code;

            ReplyMsg((struct Message *)message);
            if (type == IDCMP_RAWKEY && code == VCAP_RAWKEY_ESCAPE)
                run->cancelled = 1;
            if (run->awaiting_accept && type == IDCMP_RAWKEY &&
                    (code == VCAP_RAWKEY_RETURN ||
                     code == VCAP_RAWKEY_KEYPAD_ENTER))
                run->accepted = 1;
            if (type == IDCMP_INACTIVEWINDOW)
                run->ownership_lost = 1;
        }
        if ((run->window->Flags & WFLG_WINDOWACTIVE) == 0)
            run->ownership_lost = 1;
    }
    return !run->cancelled && !run->ownership_lost;
}

static int vcap_sampling_mode_available(ULONG display_id)
{
    struct DisplayInfo info;

    if (GetDisplayInfoData(NULL, (UBYTE *)&info, sizeof(info), DTAG_DISP,
            display_id) == 0)
        return 0;
    return info.NotAvailable == 0;
}

static int vcap_sampling_mode_matches(ULONG display_id, int ntsc, int lace)
{
    struct DisplayInfo info;
    struct DimensionInfo dimensions;

    memset(&info, 0, sizeof(info));
    memset(&dimensions, 0, sizeof(dimensions));
    return (display_id & SUPERHIRES) != 0 &&
        ModeNotAvailable(display_id) == 0 &&
        GetDisplayInfoData(NULL, (UBYTE *)&info, sizeof(info), DTAG_DISP,
            display_id) != 0 &&
        GetDisplayInfoData(NULL, (UBYTE *)&dimensions, sizeof(dimensions),
            DTAG_DIMS, display_id) != 0 &&
        !(info.PropertyFlags & (DIPF_IS_FOREIGN | DIPF_IS_HAM |
            DIPF_IS_DUALPF | DIPF_IS_EXTRAHALFBRITE | DIPF_IS_SCANDBL)) &&
        !!(info.PropertyFlags & DIPF_IS_PAL) == !ntsc &&
        !!(info.PropertyFlags & DIPF_IS_LACE) == lace &&
        info.RedBits == 8 && info.GreenBits == 8 && info.BlueBits == 8 &&
        dimensions.MaxDepth >= 8;
}

static int vcap_sampling_screen_matches(struct Screen *screen, int ntsc,
    int lace)
{
    ULONG display_id;

    display_id = GetVPModeID(&screen->ViewPort);
    return vcap_sampling_mode_matches(display_id, ntsc, lace) &&
        screen->Width == 1280 && screen->RastPort.BitMap != NULL &&
        screen->RastPort.BitMap->Depth == 8;
}

static int vcap_sampling_open_screen(struct vcap_sampling_run *run,
    int ntsc, int lace, char *failure, unsigned failure_size)
{
    ULONG colors[770];
    ULONG display_id = (ntsc ? NTSC_MONITOR_ID : PAL_MONITOR_ID) |
        (lace ? SUPERLACE_KEY : SUPER_KEY);
    ULONG error = 0;
    struct BitMap *bitmap;
    unsigned pen, plane, x, y, bit;

    if (!vcap_sampling_mode_available(display_id)) {
        vcap_sampling_set_failure(failure, failure_size,
            "Requested 256-colour SuperHires sampling mode is unavailable");
        return 0;
    }
    run->screen = OpenScreenTags(NULL, SA_Type, CUSTOMSCREEN,
        SA_DisplayID, display_id, SA_Width, 1280,
        SA_Height, (ntsc ? 200 : 256) * (lace ? 2 : 1),
        SA_Depth, 8, SA_Interleaved, FALSE, SA_Quiet, TRUE,
        SA_ShowTitle, FALSE, SA_Behind, TRUE, SA_AutoScroll, FALSE,
        SA_ErrorCode, (ULONG)&error, TAG_END);
    if (run->screen == NULL) {
        vcap_sampling_set_failure(failure, failure_size,
            "Could not open 256-colour sampling screen");
        return 0;
    }
    if (!vcap_sampling_screen_matches(run->screen, ntsc, lace)) {
        vcap_sampling_set_failure(failure, failure_size,
            "Sampling screen did not open in requested PAL/NTSC SuperHires mode");
        CloseScreen(run->screen);
        run->screen = NULL;
        return 0;
    }
    run->window = OpenWindowTags(NULL, WA_CustomScreen, (ULONG)run->screen,
        WA_Left, 0, WA_Top, 0, WA_Width, run->screen->Width,
        WA_Height, run->screen->Height, WA_Borderless, TRUE,
        WA_Backdrop, TRUE, WA_RMBTrap, TRUE, WA_NoCareRefresh, TRUE,
        WA_Activate, TRUE,
        WA_IDCMP, IDCMP_RAWKEY | IDCMP_INACTIVEWINDOW, TAG_END);
    if (run->window == NULL) {
        vcap_sampling_set_failure(failure, failure_size,
            "Could not open sampling-screen input window");
        CloseScreen(run->screen);
        run->screen = NULL;
        return 0;
    }
    run->pointer = (UWORD *)AllocMem(16, MEMF_CHIP | MEMF_CLEAR);
    if (run->pointer == NULL) {
        vcap_sampling_set_failure(failure, failure_size,
            "Could not allocate the invisible pointer.");
        CloseWindow(run->window);
        run->window = NULL;
        CloseScreen(run->screen);
        run->screen = NULL;
        return 0;
    }
    SetPointer(run->window, run->pointer, 1, 16, 0, 0);
    colors[0] = 256UL << 16;
    for (pen = 0; pen < 256; ++pen) {
        ULONG rgb = zz_capture_pattern_rgb(pen);

        colors[1 + pen * 3] = ((rgb >> 16) & 255) * 0x01010101UL;
        colors[2 + pen * 3] = ((rgb >> 8) & 255) * 0x01010101UL;
        colors[3 + pen * 3] = (rgb & 255) * 0x01010101UL;
    }
    colors[769] = 0;
    LoadRGB32(&run->screen->ViewPort, colors);
    bitmap = run->screen->RastPort.BitMap;
    WaitBlit();
    for (plane = 0; plane < 8; ++plane) {
        UBYTE *row = bitmap->Planes[plane];

        for (x = 0; x < 1280; x += 8) {
            UBYTE value = 0;

            for (bit = 0; bit < 8; ++bit)
                if ((x + bit) & (1U << plane))
                    value |= 0x80U >> bit;
            row[x / 8] = value;
        }
        for (y = 1; y < (unsigned)run->screen->Height; ++y)
            memcpy(row + y * bitmap->BytesPerRow, row, 1280 / 8);
    }
    WaitBlit();
    ScreenToFront(run->screen);
    ActivateWindow(run->window);
    /* Do not score an old frame merely because ScreenToFront queued: wait for
     * the display to show the palette/pattern and confirm we still own it. */
    WaitTOF();
    WaitTOF();
    if (!vcap_sampling_keep_running(run)) {
        vcap_sampling_set_failure(failure, failure_size,
            run->ownership_lost ?
                "Sampling screen lost front ownership" :
                "Sampling calibration cancelled");
        return 0;
    }
    return 1;
}

static void vcap_sampling_close_screen(struct vcap_sampling_run *run)
{
    if (run->window != NULL) {
        CloseWindow(run->window);
        run->window = NULL;
    }
    if (run->pointer != NULL) {
        FreeMem(run->pointer, 16);
        run->pointer = NULL;
    }
    if (run->screen != NULL) {
        CloseScreen(run->screen);
        run->screen = NULL;
    }
}

static enum zz_vcap_result vcap_sampling_capture(
    struct vcap_sampling_run *run, int phase, struct zz_vcap_capture_data *data)
{
    struct zz_vcap_capture_context context;
    enum zz_vcap_result result;

    memset(&context, 0, sizeof(context));
    context.phase = phase;
    context.require_phase = 1;
    context.ntsc = run->ntsc;
    context.lace = run->lace;
    context.have_geometry = run->geometry_valid;
    context.geometry = run->geometry;
    result = zz_vcap_capture(&run->io, &context, data);
    if (result == ZZ_VCAP_OK && !run->geometry_valid) {
        run->geometry = data->geometry;
        run->geometry_valid = 1;
    }
    return result;
}

static enum zz_vcap_result vcap_sampling_measure(
    struct vcap_sampling_run *run, int phase, int ntsc, int lace,
    unsigned comparisons, unsigned long *wrong, unsigned long *changed)
{
    struct zz_vcap_capture_data data;
    unsigned have_previous[2] = { 0, 0 };
    unsigned have_comparison[2] = { 0, 0 };
    unsigned capture, parity, actual_lace;
    unsigned limit = 6U * (comparisons + 1U);
    enum zz_vcap_result result;

    *wrong = *changed = 0;
    if (!vcap_sampling_keep_running(run))
        return ZZ_VCAP_CANCELLED;
    /* Exactly one acknowledged phase request precedes every measured point. */
    result = zz_vcap_phase_apply(&run->io, phase, 0);
    if (result != ZZ_VCAP_OK)
        return result;
    for (capture = 0; capture < 2; ++capture) {
        memset(&data, 0, sizeof(data));
        data.pixels = vcap_sampling_buffers.current;
        result = vcap_sampling_capture(run, phase, &data);
        if (result != ZZ_VCAP_OK)
            return result;
    }
    actual_lace = (data.status & ZZ_CAPTURE_SNAPSHOT_LACE) != 0;
    if (actual_lace != !!lace ||
            ((data.status & ZZ_CAPTURE_SNAPSHOT_NTSC) != 0) != !!ntsc)
        return ZZ_VCAP_STATE_CHANGED;
    for (capture = 0; capture < limit; ++capture) {
        memset(&data, 0, sizeof(data));
        data.pixels = vcap_sampling_buffers.current;
        result = vcap_sampling_capture(run, phase, &data);
        if (result != ZZ_VCAP_OK)
            return result;
        parity = actual_lace ?
            ((data.status & ZZ_CAPTURE_SNAPSHOT_PARITY) != 0) : 0;
        *wrong += zz_capture_pattern_errors(vcap_sampling_buffers.current);
        if (have_previous[parity]) {
            *changed += zz_capture_changed_pixels(vcap_sampling_buffers.current,
                vcap_sampling_buffers.previous[parity]);
            ++have_comparison[parity];
        }
        memcpy(vcap_sampling_buffers.previous[parity],
            vcap_sampling_buffers.current, sizeof(vcap_sampling_buffers.current));
        have_previous[parity] = 1;
        if (have_comparison[0] >= comparisons &&
                (!actual_lace || have_comparison[1] >= comparisons))
            return ZZ_VCAP_OK;
    }
    return ZZ_VCAP_UNSTABLE;
}

static int vcap_sampling_wait_for_accept(struct vcap_sampling_run *run,
    int phase)
{
    char message[96];

    snprintf(message, sizeof(message),
        "Phase %d applied - Enter to finish, Esc to undo", phase);
    vcap_sampling_message(run, message);
    run->awaiting_accept = 1;
    while (!run->accepted && vcap_sampling_keep_running(run))
        Wait((1UL << run->window->UserPort->mp_SigBit) | SIGBREAKF_CTRL_C);
    return run->accepted && !run->cancelled && !run->ownership_lost;
}

int vcap_sampling_run(const struct zz_vcap_io *card_io, int ntsc, int lace,
    int entry_phase, int *selected_phase,
    unsigned char quality[ZZ_CAPTURE_BINS], char *failure,
    unsigned failure_size)
{
    struct vcap_sampling_run run;
    struct zz_vcap_phase_state state;
    struct zz_capture_eye eye;
    struct zz_capture_boundary edges[2];
    unsigned char clean[ZZ_CAPTURE_BINS];
    unsigned i, edge;
    unsigned long wrong, changed;
    int first_clean, last_clean;
    int result = -1;
    int accepted = 0;
    int mutation_attempted = 0;
    enum zz_vcap_result vresult = ZZ_VCAP_OK;
    char message[96];

    if (failure != NULL && failure_size != 0)
        failure[0] = '\0';
    if (card_io == NULL || card_io->read16 == NULL || card_io->write16 == NULL ||
            card_io->delay == NULL || selected_phase == NULL || quality == NULL ||
            !zz_capture_phase_valid(entry_phase) || (ntsc != 0 && ntsc != 1) ||
            (lace != 0 && lace != 1)) {
        vcap_sampling_set_failure(failure, failure_size,
            "Invalid sampling calibration request");
        return -1;
    }
    memset(&run, 0, sizeof(run));
    memset(clean, 0, sizeof(clean));
    run.ntsc = (unsigned)ntsc;
    run.lace = (unsigned)lace;
    run.io = *card_io;
    run.io.control_ctx = &run;
    run.io.keep_running = vcap_sampling_keep_running;

    vresult = zz_vcap_phase_read(&run.io, &state);
    if (vresult != ZZ_VCAP_OK || state.domain != ZZ_VCAP_PHASE_C28 ||
            state.applied != entry_phase) {
        vcap_sampling_set_failure(failure, failure_size,
            vresult == ZZ_VCAP_OK ?
                "Sampling calibration requires the current C28 phase" :
                zz_vcap_result_text(vresult));
        goto finished;
    }
    vresult = zz_vcap_phase_check(&run.io, entry_phase);
    if (vresult != ZZ_VCAP_OK) {
        vcap_sampling_set_failure(failure, failure_size,
            zz_vcap_result_text(vresult));
        goto finished;
    }
    if (!vcap_sampling_open_screen(&run, ntsc, lace, failure, failure_size)) {
        result = run.cancelled ? 0 : -1;
        goto finished;
    }
    for (i = 0; i < ZZ_CAPTURE_BINS; ++i) {
        int phase = ZZ_CAPTURE_PHASE_MIN + (int)(i * ZZ_CAPTURE_BIN_STEPS);

        snprintf(message, sizeof(message), "Calibrating phase %u/%u...",
            i + 1, ZZ_CAPTURE_BINS);
        vcap_sampling_message(&run, message);
        mutation_attempted = 1;
        vresult = vcap_sampling_measure(&run, phase, ntsc, lace,
            VCAP_SAMPLING_SWEEP_COMPARISONS, &wrong, &changed);
        if (vresult != ZZ_VCAP_OK)
            goto failed_measurement;
        clean[i] = zz_sampling_score_clean(wrong, changed);
    }
    if (!zz_capture_select_eye(clean, entry_phase, &eye) || eye.full_circle) {
        vcap_sampling_set_failure(failure, failure_size,
            "No measured clean sampling interval");
        goto finished;
    }
    first_clean = ZZ_CAPTURE_PHASE_MIN +
        (int)(eye.start_bin * ZZ_CAPTURE_BIN_STEPS);
    last_clean = first_clean + (int)((eye.bins - 1) * ZZ_CAPTURE_BIN_STEPS);
    edges[0].origin = first_clean - (int)ZZ_CAPTURE_BIN_STEPS;
    edges[0].bad = 0;
    edges[0].good = ZZ_CAPTURE_BIN_STEPS;
    edges[1].origin = last_clean;
    edges[1].good = 0;
    edges[1].bad = ZZ_CAPTURE_BIN_STEPS;
    for (edge = 0; edge < 2; ++edge) {
        while (zz_capture_boundary_pending(&edges[edge])) {
            int phase = zz_capture_boundary_next(&edges[edge]);

            vcap_sampling_message(&run, edge ?
                "Refining upper eye boundary..." :
                "Refining lower eye boundary...");
            mutation_attempted = 1;
            vresult = vcap_sampling_measure(&run, phase, ntsc, lace,
                VCAP_SAMPLING_SWEEP_COMPARISONS, &wrong, &changed);
            if (vresult != ZZ_VCAP_OK)
                goto failed_measurement;
            zz_capture_boundary_record(&edges[edge],
                zz_sampling_score_clean(wrong, changed));
        }
    }
    eye.margin = (unsigned)((edges[1].origin + (int)edges[1].good -
        (edges[0].origin + (int)edges[0].good)) / 2);
    eye.phase = zz_capture_phase_wrap(edges[0].origin +
        (int)edges[0].good + (int)eye.margin);
    vcap_sampling_message(&run, "Retesting selected sampling phase...");
    mutation_attempted = 1;
    vresult = vcap_sampling_measure(&run, eye.phase, ntsc, lace,
        VCAP_SAMPLING_RETEST_COMPARISONS, &wrong, &changed);
    if (vresult != ZZ_VCAP_OK || wrong != 0 || changed != 0) {
        if (vresult == ZZ_VCAP_OK)
            vcap_sampling_set_failure(failure, failure_size,
                "Selected sampling phase did not remain clean");
        goto failed_measurement;
    }
    if (!vcap_sampling_wait_for_accept(&run, eye.phase)) {
        result = run.cancelled ? 0 : -1;
        if (run.ownership_lost)
            vcap_sampling_set_failure(failure, failure_size,
                "Sampling screen lost front ownership");
        goto finished;
    }
    *selected_phase = eye.phase;
    memcpy(quality, clean, sizeof(clean));
    accepted = 1;
    result = 1;
    goto finished;

failed_measurement:
    if (vresult == ZZ_VCAP_CANCELLED && run.cancelled)
        result = 0;
    else {
        if (failure == NULL || failure_size == 0 || failure[0] == '\0')
            vcap_sampling_set_failure(failure, failure_size,
                run.ownership_lost ? "Sampling screen lost front ownership" :
                zz_vcap_result_text(vresult));
        result = -1;
    }

finished:
    if (!accepted && mutation_attempted) {
        vresult = zz_vcap_phase_apply(&run.io, entry_phase, 1);
        if (vresult != ZZ_VCAP_OK) {
            vcap_sampling_set_failure(failure, failure_size,
                "Capture phase state is unknown: restoration failed");
            vcap_sampling_message(&run,
                "Capture phase state unknown - restoration failed");
            Delay(50);
            result = -2;
        }
    }
    vcap_sampling_close_screen(&run);
    return result;
}
