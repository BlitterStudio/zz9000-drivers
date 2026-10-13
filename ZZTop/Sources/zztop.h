/*
 * ZZTop - the ZZ9000 control panel.
 * Copyright (C) 2016-2026, Lucie L. Hartmann <lucie@mntre.com>
 *                          MNT Research GmbH, Berlin
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 *   (ZZTop V2 and all later versions)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Shared application surface. ZZTop is one ReAction window: a page list
 * on the left and the selected page on the right. Hardware access lives
 * in board.c, the ZZ9000.CFG editor model in settings.c, live capture
 * calibration in video.c, the firmware audio control plane in audio.c;
 * pages (page_*.c) only present those models. app.c owns the
 * cross-model Save / Revert / Quit decisions.
 */
#ifndef ZZTOP_H
#define ZZTOP_H

#include <exec/types.h>

#define ZZTOP_NAME    "ZZTop"
#define ZZTOP_RELEASE "3.0"
#define ZZTOP_DATE    "13.10.2026"

/* Pending-change summary across every model, for the status bar, the
 * Save/Revert buttons and the Quit decision. */
struct app_pending {
	UWORD card;     /* ZZ9000.CFG fields edited but not saved */
	UWORD reboot;   /* ...of which take effect only at the next cold boot */
	BOOL live;      /* live hardware differs from what Revert restores */
	BOOL audio;     /* firmware audio scene state differs from its save */
};

/* After the models loaded: capture the live state Revert returns to. */
void app_init(void);

void app_pending(struct app_pending *out);
BOOL app_has_changes(void);

/* Persist every pending change (audio scene state, then ZZ9000.CFG).
 * Reports progress and the outcome on the status bar. */
BOOL app_save(void);

/* Discard every unsaved change: restore live hardware to what it was
 * when ZZTop opened (or last saved) and reload the card file. */
BOOL app_revert(void);

/* TRUE when ZZTop may exit. Asks Save / Keep until reboot / Discard /
 * Cancel when changes are pending. */
BOOL app_quit_allowed(void);

/* A model changed in a way every page may display (load, save, revert,
 * external state change): re-sync all pages and the pending summary. */
void app_models_changed(void);

/* 10 Hz heartbeat for model state machines that run regardless of the
 * visible page (audio save settling, slider coalescing). */
void app_tick(void);

#endif /* ZZTOP_H */
