/*
 * ZZTop front-end services.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The shared core (board, settings, video, audio, app) never touches a
 * toolkit. What it needs from the user interface is this small set of
 * services, implemented by the GadTools window (ZZTop/GadTools).
 */
#ifndef ZZTOP_FRONTEND_H
#define ZZTOP_FRONTEND_H

#include <exec/types.h>
#include <exec/lists.h>
#include <intuition/intuition.h>

/* One-line result on the status bar (printf format). */
void fe_status(const char *fmt, ...);

/* Requester with word-wrapped body. Returns the EasyRequest gadget
 * number (1..n-1, 0 for the last gadget). */
LONG fe_ask(const char *gadgets, const char *fmt, ...);

/* Busy pointer plus input blocking; nests. */
void fe_busy(BOOL busy);

/* Commit edits still held only in gadgets (typed text without Enter) to
 * the models, before Save, Revert or Quit decide what is pending. */
void fe_flush(void);

/* Models changed (load, save, revert, acknowledged hardware state):
 * re-read everything every view shows, then the pending summary. */
void fe_models_changed(void);

/* Recompute only the pending summary (Save/Revert state, title). */
void fe_pending_changed(void);

/* Bring the place where a Save problem can be fixed into view. */
enum fe_place {
	FE_PLACE_SYSTEM,     /* network address, SD boot image */
	FE_PLACE_CAPTURE     /* unconfirmed capture calibration */
};
void fe_show(enum fe_place place);

/* 10 Hz counter since start. */
ULONG fe_ticks(void);

/* The front end's window and screen (NULL before it opens), for the
 * native-screen tools that must hand the display back afterwards. */
struct Window *fe_window(void);
struct Screen *fe_screen(void);

/* Bounded hardware waits. While a command polls the card, keep the
 * window serviced: Close or Esc set *cancel, everything else is deferred
 * and re-queued by fe_wait_end so it is processed exactly once. Pass
 * fe_wait_keep_running and the wait as a zz_vcap_io keep_running hook. */
struct fe_wait {
	BOOL *cancel;
	struct List deferred;
};
void fe_wait_begin(struct fe_wait *wait, BOOL *cancel);
int fe_wait_keep_running(void *wait);
void fe_wait_end(struct fe_wait *wait);

#endif /* ZZTOP_FRONTEND_H */
