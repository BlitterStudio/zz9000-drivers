/*
 * ZZTop native-screen capture framing editor.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <exec/types.h>
#include <devices/inputevent.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <graphics/displayinfo.h>
#include <graphics/modeid.h>
#include <graphics/rastport.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <stdio.h>
#include <string.h>
#include "align.h"
#include "ui.h"

#define ALIGN_ENTER_PAD 0x43
#define ALIGN_ENTER     0x44
#define ALIGN_ESCAPE    0x45


#define ALIGN_UP        0x4c
#define ALIGN_DOWN      0x4d
#define ALIGN_RIGHT     0x4e
#define ALIGN_LEFT      0x4f

enum align_action { ALIGN_NONE, ALIGN_ADJUST, ALIGN_ACCEPT, ALIGN_RESTORE };
struct align_state {
	const struct zz_vcap_io *io;
	struct zz_vcap_snapshot entry;
	struct zz_vcap_snapshot current;
	struct zz_vcap_working working;
	struct zz_vcap_working pending_working;
	struct Screen *screen;
	struct Window *window;
	ULONG pending_raw;
	UBYTE pending_sequence;
	UWORD pending_action;
	BOOL cancel_requested;
	BOOL accepted;
	BOOL done;
	BOOL unknown;
	char message[96];
};

/* Read a 32-bit capture register through the supplied I/O contract. */
static ULONG read32(const struct zz_vcap_io *io, ULONG offset)
{
	return zz_vcap_read32(io, offset);
}

/* Take a coherent live snapshot around the capture status sequence. */
static BOOL snapshot(const struct zz_vcap_io *io, struct zz_vcap_snapshot *out)
{
	int tries;

	for (tries = 0; tries < 8; tries++) {
		ULONG before = read32(io, ZZ_VCAP_LIVE_STATUS);
		ULONG raw = read32(io, ZZ_VCAP_LIVE_APPLIED_RAW);
		ULONG crop = read32(io, ZZ_VCAP_LIVE_EFFECTIVE_CROP);
		ULONG after = read32(io, ZZ_VCAP_LIVE_STATUS);

		if (zz_vcap_snapshot_status_valid(before, after)) {
			out->status = after;
			out->raw = raw;
			zz_vcap_effective_unpack(crop, &out->effective_h, &out->effective_v);
			return TRUE;
		}
	}
	return FALSE;
}

/* Start one nonblocking crop request so Escape can queue an exact restore. */
static BOOL start_apply(struct align_state *state,
	const struct zz_vcap_working *candidate, UWORD action)
{
	struct zz_vcap_status status;
	ULONG stable;
	ULONG raw;

	raw = action == ALIGN_RESTORE ? state->entry.raw :
		zz_vcap_control_pack(&candidate->control);
	if (raw == state->current.raw) {
		if (action == ALIGN_ACCEPT) {
			state->working = *candidate;
			state->accepted = TRUE;
			state->done = TRUE;
		} else if (action == ALIGN_RESTORE) {
			state->done = TRUE;
		}
		return TRUE;
	}
	if (zz_vcap_read_stable(state->io, ZZ_VCAP_LIVE_STATUS, &stable) != ZZ_VCAP_OK)
		return FALSE;
	zz_vcap_status_unpack(stable, &status);
	if (!status.applied_valid || status.busy ||
		status.request_sequence != status.applied_sequence)
		return FALSE;
	state->pending_sequence = zz_vcap_next_sequence(status.request_sequence);
	state->io->write16(state->io->ctx, ZZ_VCAP_LIVE_STAGED_RAW_HI,
		(UWORD)(raw >> 16));
	state->io->write16(state->io->ctx, ZZ_VCAP_LIVE_STAGED_RAW_LO, (UWORD)raw);
	state->io->write16(state->io->ctx, ZZ_VCAP_LIVE_COMMIT,
		ZZ_VCAP_LIVE_COMMIT_TOKEN);
	state->pending_raw = raw;
	state->pending_action = action;
	if (candidate)
		state->pending_working = *candidate;
	snprintf(state->message, sizeof(state->message),
		"Waiting for native frame ACK; Esc queues entry restore");
	return TRUE;
}

/* Reconcile one outstanding request without blocking native-screen input. */
static void reconcile(struct align_state *state)
{
	struct zz_vcap_snapshot applied;
	struct zz_vcap_status status;
	ULONG stable;
	UWORD action;

	if (state->pending_action == ALIGN_NONE)
		return;
	if (zz_vcap_read_stable(state->io, ZZ_VCAP_LIVE_STATUS, &stable) != ZZ_VCAP_OK)
		return;
	zz_vcap_status_unpack(stable, &status);
	if (status.busy)
		return;
	if (status.request_sequence != state->pending_sequence) {
		if (status.request_sequence != status.applied_sequence)
			return;
		state->pending_action = ALIGN_NONE;
		if (!snapshot(state->io, &state->current))
			return;
		if (state->cancel_requested)
			(void)start_apply(state, NULL, ALIGN_RESTORE);
		else
			snprintf(state->message, sizeof(state->message),
				"Another writer won the request; press the key again");
		return;
	}
	if (status.applied_sequence != state->pending_sequence)
		return;
	if (!snapshot(state->io, &applied))
		return;
	action = state->pending_action;
	state->pending_action = ALIGN_NONE;
	state->current = applied;
	if (zz_vcap_request_result(stable, state->pending_sequence, applied.raw,
		state->pending_raw) != ZZ_VCAP_REQUEST_APPLIED) {
		if (state->cancel_requested || action == ALIGN_RESTORE)
			(void)start_apply(state, NULL, ALIGN_RESTORE);
		else
			snprintf(state->message, sizeof(state->message),
				"Request lost an arbiter race; press the key again");
		return;
	}
	if (action == ALIGN_RESTORE) {
		state->done = TRUE;
		return;
	}
	state->working = state->pending_working;
	if (state->cancel_requested) {
		(void)start_apply(state, NULL, ALIGN_RESTORE);
	} else if (action == ALIGN_ACCEPT) {
		state->accepted = TRUE;
		state->done = TRUE;
	} else {
		snprintf(state->message, sizeof(state->message),
			"Applied at capture-frame boundary");
	}
}

/* Queue a restore, or let a second Escape explicitly report unknown state. */
static void cancel(struct align_state *state)
{
	if (state->cancel_requested && state->pending_action == ALIGN_NONE) {
		state->unknown = TRUE;
		state->done = TRUE;
		return;
	}
	state->cancel_requested = TRUE;
	if (state->pending_action != ALIGN_NONE) {
		snprintf(state->message, sizeof(state->message),
			"Cancel pending; waiting to restore entry state (Esc again gives up)");
		return;
	}
	if (!start_apply(state, NULL, ALIGN_RESTORE)) {
		snprintf(state->message, sizeof(state->message),
			"Restore not accepted; Esc again reports state unknown");
	}
}

/* Draw a guide-line segment in the requested pen colour. */
static void line(struct RastPort *rp, WORD x1, WORD y1, WORD x2, WORD y2,
	UWORD pen)
{
	SetAPen(rp, pen);
	Move(rp, x1, y1);
	Draw(rp, x2, y2);
}

/* Redraw the native alignment screen and its current instructions. */
static void draw(struct align_state *state)
{
	struct RastPort *rp = state->window->RPort;
	WORD width = state->screen->Width;
	WORD height = state->screen->Height;
	WORD safe_x = width / 10;
	WORD safe_y = height / 10;
	char text[96];

	SetDrMd(rp, JAM1);
	SetRast(rp, 0);
	line(rp, 0, 0, width - 1, 0, 2);
	line(rp, width - 1, 0, width - 1, height - 1, 2);
	line(rp, width - 1, height - 1, 0, height - 1, 2);
	line(rp, 0, height - 1, 0, 0, 2);
	line(rp, safe_x, safe_y, width - safe_x - 1, safe_y, 3);
	line(rp, width - safe_x - 1, safe_y, width - safe_x - 1,
		height - safe_y - 1, 3);
	line(rp, width - safe_x - 1, height - safe_y - 1, safe_x,
		height - safe_y - 1, 3);
	line(rp, safe_x, height - safe_y - 1, safe_x, safe_y, 3);
	line(rp, width / 2 - 24, height / 2, width / 2 + 24, height / 2, 1);
	line(rp, width / 2, height / 2 - 24, width / 2, height / 2 + 24, 1);
	SetAPen(rp, 1);
	snprintf(text, sizeof(text), "Live Custom H:%u  V:%u",
		(unsigned)state->working.crop_h, (unsigned)state->working.crop_v);
	Move(rp, 8, 12);
	Text(rp, (STRPTR)text, strlen(text));
	Move(rp, 8, 24);
	Text(rp, (STRPTR)state->message, strlen(state->message));
	Move(rp, 8, height - 8);
	Text(rp, (STRPTR)"Arrows adjust  Shift+Arrows coarse  Enter accept  Esc cancel", 59);
}

/* Copy a bounded result message for the caller. */
static void set_message(char *out, UWORD size, const char *text)
{
	if (size) {
		snprintf(out, size, "%s", text);
		out[size - 1] = '\0';
	}
}

/* Process one raw key while preserving the pending-action state machine. */
static void key(struct align_state *state, UWORD code, UWORD qualifier)
{
	struct zz_vcap_working candidate;
	UWORD move;

	if (code & IECODE_UP_PREFIX)
		return;
	if (code == ALIGN_ESCAPE) {
		cancel(state);
		return;
	}
	if (state->pending_action != ALIGN_NONE || state->cancel_requested)
		return;
	if (code == ALIGN_ENTER || code == ALIGN_ENTER_PAD) {
		candidate = state->working;
		zz_vcap_accept(&candidate);
		if (!start_apply(state, &candidate, ALIGN_ACCEPT))
			snprintf(state->message, sizeof(state->message),
				"Request not accepted; retry or Esc to restore");
		return;
	}
	switch (code) {
	case ALIGN_LEFT:
		move = ZZ_VCAP_MOVE_LEFT;
		break;
	case ALIGN_RIGHT:
		move = ZZ_VCAP_MOVE_RIGHT;
		break;
	case ALIGN_UP:
		move = ZZ_VCAP_MOVE_UP;
		break;
	case ALIGN_DOWN:
		move = ZZ_VCAP_MOVE_DOWN;
		break;
	default:
		return;
	}
	candidate = state->working;
	if (!zz_vcap_adjust(&candidate, move,
		(qualifier & (IEQUALIFIER_LSHIFT | IEQUALIFIER_RSHIFT)) != 0)) {
		snprintf(state->message, sizeof(state->message),
			"At 0..4095 limit; no hardware request sent");
		return;
	}
	if (!start_apply(state, &candidate, ALIGN_ADJUST))
		snprintf(state->message, sizeof(state->message),
			"Request not accepted; retry or Esc to restore");
}

/* Run the native-screen framing editor and restore the exact entry control. */
BOOL align_picture_run(const struct zz_vcap_io *io,
	const struct zz_vcap_snapshot *entry, struct zz_vcap_control *accepted,
	char *message, UWORD message_size)
{
	static struct ColorSpec colors[] = {
		{ 0, 0, 0, 0 }, { 1, 15, 15, 15 }, { 2, 15, 3, 3 },
		{ 3, 3, 8, 15 }, { -1, 0, 0, 0 }
	};
	struct align_state state;
	struct IntuiMessage *imsg;
	struct zz_vcap_status captured;
	ULONG screen_error = 0;
	ULONG display_id = HIRES_KEY;

	if (!io || !entry || !accepted) {
		set_message(message, message_size, "Live framing state is unavailable");
		return FALSE;
	}
	memset(&state, 0, sizeof(state));
	state.io = io;
	state.entry = *entry;
	state.current = *entry;
	if (!zz_vcap_control_unpack(entry->raw, &state.working.control)) {
		set_message(message, message_size, "Live framing state is unavailable");
		return FALSE;
	}
	state.working.crop_h = entry->effective_h;
	state.working.crop_v = entry->effective_v;
	snprintf(state.message, sizeof(state.message), "Adjust until the boxes are centred");
	zz_vcap_status_unpack(entry->status, &captured);
	if (captured.standard_valid) {
		ULONG native_id = (captured.ntsc ? NTSC_MONITOR_ID : PAL_MONITOR_ID) | HIRES_KEY;

		if (ModeNotAvailable(native_id) == 0)
			display_id = native_id;
	}
	state.screen = OpenScreenTags(NULL, SA_Type, CUSTOMSCREEN,
		SA_DisplayID, display_id, SA_Depth, 2, SA_Overscan, OSCAN_TEXT,
		SA_Colors, colors, SA_SysFont, 0, SA_Quiet, TRUE, SA_ShowTitle,
		FALSE, SA_Behind, TRUE, SA_AutoScroll, FALSE, SA_ErrorCode,
		(ULONG)&screen_error, TAG_END);
	if (!state.screen) {
		set_message(message, message_size, "Native alignment screen could not open");
		return FALSE;
	}
	state.window = OpenWindowTags(NULL, WA_CustomScreen, state.screen,
		WA_Left, 0, WA_Top, 0, WA_Width, state.screen->Width, WA_Height,
		state.screen->Height, WA_Borderless, TRUE, WA_Backdrop, TRUE,
		WA_RMBTrap, TRUE, WA_NoCareRefresh, TRUE, WA_Activate, TRUE,
		WA_RptQueue, 16, WA_IDCMP, IDCMP_RAWKEY, TAG_END);
	if (!state.window) {
		CloseScreen(state.screen);
		set_message(message, message_size, "Native alignment window could not open");
		return FALSE;
	}
	ScreenToFront(state.screen);
	draw(&state);
	while (!state.done) {
		if (state.pending_action == ALIGN_NONE)
			Wait(1UL << state.window->UserPort->mp_SigBit);
		else
			Delay(1);
		while ((imsg = (struct IntuiMessage *)GetMsg(state.window->UserPort))) {
			UWORD code = imsg->Code;
			UWORD qualifier = imsg->Qualifier;

			ReplyMsg((struct Message *)imsg);
			key(&state, code, qualifier);
		}
		reconcile(&state);
		draw(&state);
	}
	CloseWindow(state.window);
	CloseScreen(state.screen);
	if (ui_screen())
		ScreenToFront(ui_screen());
	if (ui_window())
		ActivateWindow(ui_window());
	if (state.accepted) {
		*accepted = state.working.control;
		set_message(message, message_size, "Picture alignment applied");
		return TRUE;
	}
	if (state.unknown)
		set_message(message, message_size, "Framing restore abandoned; live state is unknown");
	else
		set_message(message, message_size, "Picture alignment cancelled");
	return FALSE;
}
