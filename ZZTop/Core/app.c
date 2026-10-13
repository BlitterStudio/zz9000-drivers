/*
 * ZZTop cross-model Save / Revert / Quit.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Save order matters. The firmware audio Save rewrites ZZ9000.CFG from its
 * cold-boot parse, so it runs first inside a settings guard that restores
 * any non-audio keys it reverted; the card-file save then re-reads that
 * result and applies the pending edits on top. The guard outlives a single
 * Save press: when the firmware is slow or the repair write fails, the
 * snapshot is kept and the next Save finishes the same transaction.
 */
#include <exec/types.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <stdio.h>
#include <string.h>

#include "zztop.h"
#include "audio.h"
#include "board.h"
#include "fwup_client.h"
#include "settings.h"
#include "frontend.h"
#include "video.h"

/* Upper bound, in ticks, for the firmware's asynchronous audio save
 * (temp file, backup rename, live rename on the SD card), measured on the
 * wall clock so slow mailbox replies count too. */
#define AUDIO_SAVE_TIMEOUT_TICKS (TICKS_PER_SECOND * 15)

/* Scanline registers as they were when ZZTop opened or last saved:
 * Revert and Discard put them back to exactly this. */
static UWORD scan_mode_anchor, scan_parity_anchor;

/* An audio save in flight or a repair still owed (see file comment). */
static struct settings_guard guard;
static BOOL audio_save_running;

void app_init(void)
{
	scan_mode_anchor = board_scanline_mode();
	scan_parity_anchor = board_scanline_parity();
}

static BOOL scanlines_live_pending(void)
{
	return board_scanline_mode() != scan_mode_anchor ||
		board_scanline_parity() != scan_parity_anchor;
}

void app_pending(struct app_pending *out)
{
	out->card = settings_dirty_count();
	out->reboot = settings_reboot_count();
	out->live = scanlines_live_pending() || video_live_pending();
	/* An unfinished audio save transaction keeps Save available. */
	out->audio = audio_pending() || audio_save_running || guard.valid;
}

BOOL app_has_changes(void)
{
	struct app_pending p;

	fe_flush();
	app_pending(&p);
	return p.card || p.live || p.audio;
}

void app_models_changed(void)
{
	fe_models_changed();
}

void app_tick(void)
{
	audio_tick();
}

/* Ticks since an arbitrary epoch, from the DOS clock. */
static ULONG now_ticks(void)
{
	struct DateStamp ds;

	DateStamp(&ds);
	return ((ULONG)ds.ds_Days * 24UL * 60UL + (ULONG)ds.ds_Minute) *
		60UL * TICKS_PER_SECOND + (ULONG)ds.ds_Tick;
}

/* Start (or resume) the firmware audio save and repair what it reverted.
 * TRUE only when the audio state is on the card and every non-audio key
 * is as it was before the save. */
static BOOL save_audio(void)
{
	static char msg[96];
	enum audio_save_state state;
	ULONG deadline;
	UWORD st;

	if (!audio_save_running && !guard.valid) {
		if (!settings_guard_begin(&guard)) {
			fe_status("Could not read ZZ9000.CFG; audio not saved");
			return FALSE;
		}
		if (!audio_save_start(msg, sizeof(msg))) {
			guard.valid = FALSE;
			fe_status("%s", msg);
			return FALSE;
		}
		audio_save_running = TRUE;
	}

	if (audio_save_running) {
		fe_status("Saving audio settings...");
		deadline = now_ticks() + AUDIO_SAVE_TIMEOUT_TICKS;
		state = audio_save_poll(msg, sizeof(msg));
		while (state == AUDIO_SAVE_RUNNING && now_ticks() < deadline) {
			Delay(5);
			state = audio_save_poll(msg, sizeof(msg));
		}
		if (state == AUDIO_SAVE_RUNNING) {
			fe_status("Audio save still running on the card; press "
				"Save again to finish");
			return FALSE;
		}
		audio_save_running = FALSE;
		if (state == AUDIO_SAVE_FAILED) {
			/* A failed firmware save may still have touched the file:
			 * repair it before reporting. */
			st = settings_guard_end(&guard);
			if (st == FWUP_OK)
				fe_status("%s", msg);
			else
				fe_status("%s; restoring other settings failed: %s",
					msg, fwup_strerror(st));
			return FALSE;
		}
	}

	st = settings_guard_end(&guard);
	if (st != FWUP_OK) {
		fe_status("Audio saved, but restoring other settings failed: "
			"%s. Press Save to retry.", fwup_strerror(st));
		return FALSE;
	}
	return TRUE;
}

BOOL app_save(void)
{
	static char msg[96];
	struct app_pending p;
	enum cfg_field bad;
	BOOL ok = TRUE;
	UWORD reboot;

	fe_flush();
	app_pending(&p);
	reboot = p.reboot;

	if (p.card) {
		if (!settings_validate(&bad, msg, sizeof(msg))) {
			fe_status("%s", msg);
			if (bad == CFG_MAC || bad == CFG_HDF)
				fe_show(FE_PLACE_SYSTEM);
			return FALSE;
		}
		if (!video_save_check(msg, sizeof(msg))) {
			fe_status("%s", msg);
			fe_show(FE_PLACE_CAPTURE);
			return FALSE;
		}
	}

	fe_busy(TRUE);
	if (p.audio) ok = save_audio();
	if (ok && p.card) {
		UWORD st;

		fe_status("Writing ZZ9000.CFG...");
		st = settings_save();
		if (st == FWUP_OK) {
			video_saved();
		} else {
			fe_status("Saving ZZ9000.CFG failed: %s", fwup_strerror(st));
			ok = FALSE;
		}
	} else if (ok && p.audio) {
		/* The guard may have rewritten the file: re-read it so the
		 * model matches the card. */
		settings_load();
	}
	if (ok) app_init();   /* the running state is now the saved state */
	fe_busy(FALSE);

	if (ok) {
		if (reboot)
			fe_status("Saved. Power-cycle the Amiga to apply %u "
				"boot-time setting%s.", (unsigned)reboot,
				reboot == 1 ? "" : "s");
		else
			fe_status("Saved.");
	}
	app_models_changed();
	return ok;
}

BOOL app_revert(void)
{
	static char msg[96];
	BOOL ok = TRUE;

	fe_flush();
	fe_busy(TRUE);
	if (!video_revert(msg, sizeof(msg))) {
		fe_status("%s", msg);
		ok = FALSE;
	}
	board_set_scanline_mode(scan_mode_anchor);
	board_set_scanline_parity(scan_parity_anchor);
	if (ok && audio_pending() && !audio_revert(msg, sizeof(msg))) {
		fe_status("%s", msg);
		ok = FALSE;
	}
	if (ok) {
		settings_load();
		video_init();
		fe_status("Reverted to the saved settings.");
	}
	fe_busy(FALSE);
	app_models_changed();
	return ok;
}

BOOL app_quit_allowed(void)
{
	struct app_pending p;
	LONG choice;

	fe_flush();
	app_pending(&p);
	if (!p.card && !p.live && !p.audio) return TRUE;

	if (p.live || p.audio) {
		/* Live changes are already on the card's hardware: they can be
		 * kept until the next reboot without saving. */
		choice = fe_ask("Save|Keep until reboot|Discard|Cancel",
			"You have unsaved changes.\n\n"
			"Save writes them to the card. Keep until reboot leaves "
			"the live changes running without saving them%s. Discard "
			"puts everything back as it was.",
			p.card ? " (other edits are lost)" : "");
		switch (choice) {
		case 1: return app_save();
		case 2: return TRUE;
		case 3: return app_revert();
		default: return FALSE;
		}
	}
	choice = fe_ask("Save|Discard|Cancel",
		"You have %u unsaved change%s.", (unsigned)p.card,
		p.card == 1 ? "" : "s");
	switch (choice) {
	case 1: return app_save();
	case 2: return TRUE;
	default: return FALSE;
	}
}
