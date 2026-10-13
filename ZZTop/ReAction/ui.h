/*
 * ZZTop user interface framework (ReAction).
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * One window: a page list on the left, the selected page on the right,
 * a status line with Revert / Save at the bottom. Pages are plain
 * descriptor structs (struct ui_page) registered in pages.c; the
 * framework routes gadget events by id, ticks the visible page at 10 Hz
 * and re-syncs every page when a model changes.
 *
 * Rules for page code:
 * - Change gadget attributes only through ui_set()/ui_text_*(): they skip
 *   rendering for pages that are not on screen (a hidden page's gadgets
 *   must never draw over the visible one).
 * - Never block in a gadget handler for more than a few frames without
 *   ui_busy(); bounded hardware waits use fe_wait_begin()/fe_wait_end().
 * - Text pointers handed to gadgets must outlive them: use struct ui_text
 *   for anything that changes.
 */
#ifndef ZZTOP_UI_H
#define ZZTOP_UI_H

#include <exec/types.h>
#include <exec/lists.h>
#include <intuition/classusr.h>
#include <utility/tagitem.h>

#include "frontend.h"

/* Gadget ids: one block of 1000 per page, so a page owns ids
 * UI_GID(page, 0..999). Framework gadgets live below 1000. */
#define UI_GID(page, n)      ((ULONG)(((page) + 1) * 1000 + (n)))
#define UI_GID_PAGE(id)      ((WORD)((id) / 1000) - 1)
#define UI_GID_LOCAL(id)     ((UWORD)((id) % 1000))

enum ui_page_id {
	PAGE_STATUS = 0,
	PAGE_DISPLAY,
	PAGE_CAPTURE,
	PAGE_AUDIO,
	PAGE_TUNING,
	PAGE_SYSTEM,
	PAGE_FIRMWARE,
	PAGE_DIAG,
	PAGE_COUNT
};

#define UI_PAGE_ADVANCED 0x0001  /* listed only with Advanced controls on */
#define UI_PAGE_CHILD    0x0002  /* indented under the page above it */

struct ui_page {
	const char *name;      /* page list label */
	const char *title;     /* heading above the page */
	const char *summary;   /* one line under the heading: what it is for */
	UWORD flags;
	/* Build the page body (a layout object). Called once, before the
	 * window opens; returning NULL aborts startup. */
	Object *(*create)(void);
	/* The page became visible / stops being visible. */
	void (*show)(void);
	void (*hide)(void);
	/* WMHI_GADGETUP for one of this page's ids; `code` is the
	 * gadget's reported code (chooser index, checkbox state, ...). */
	void (*gadget)(UWORD local_id, UWORD code);
	/* 10 Hz while the page is visible. */
	void (*tick)(void);
	/* Models changed (load, save, revert): re-read everything shown.
	 * Called for every page, visible or not. */
	void (*sync)(void);
	/* Commit edits still held only in a gadget (a string gadget the user
	 * typed into without pressing Enter) to the model. Called for the
	 * visible page before Save, Revert and Quit decide what is pending.
	 * Optional. */
	void (*flush)(void);
	/* Free page-owned resources the gadget tree does not own (chooser
	 * label lists, list browser nodes). Called once after the window and
	 * its gadgets are disposed. Optional. */
	void (*cleanup)(void);
};

extern struct ui_page *ui_pages[PAGE_COUNT];

/* Commit the visible page's gadget-held edits (see ui_page.flush). */
void ui_flush(void);

/* ---- lifetime (main.c) ---- */
BOOL ui_open_classes(char *missing, UWORD missing_size);
void ui_close_classes(void);
BOOL ui_create(void);
void ui_run(void);
void ui_destroy(void);

/* ---- navigation and mode ---- */
UWORD ui_current_page(void);
BOOL ui_page_visible(UWORD page);
void ui_show_page(UWORD page);
BOOL ui_advanced(void);
void ui_set_advanced(BOOL on);

/* ---- gadget helpers ----
 * ui_set: SetGadgetAttrs when `page` is on screen (with refresh), plain
 * SetAttrs otherwise. */
void ui_set(UWORD page, Object *o, ULONG tag, ...);
void ui_setA(UWORD page, Object *o, struct TagItem *tags);
ULONG ui_get(Object *o, ULONG attr);
void ui_disable(UWORD page, Object *o, BOOL disabled);

/* Read-only text display with its own double buffer, so updates always
 * hand the gadget a fresh pointer and identical text is not redrawn. */
#define UI_TEXT_MAX 96
struct ui_text {
	Object *obj;
	char buf[2][UI_TEXT_MAX];
	UBYTE cur;
};
Object *ui_text_new(struct ui_text *t, const char *initial);
/* Left-aligned plain text, no frame (for notes and hints). */
Object *ui_note_new(struct ui_text *t, const char *initial);
void ui_text_set(UWORD page, struct ui_text *t, const char *text);
void ui_text_printf(UWORD page, struct ui_text *t, const char *fmt, ...);

/* Horizontal slider with its value printed to its right (works on every
 * ReAction version; the V47 SLIDER_LevelFormat does not). `format` is a
 * printf format for one long ("%ld Hz"); `widest` is the longest text
 * the value can show, used to reserve its width. Poll the slider from
 * tick() during drags: ui_slider_poll() also refreshes the value. */
struct ui_slider {
	Object *obj;
	struct ui_text value;
	const char *format;
	LONG shown;
};
Object *ui_slider_new(struct ui_slider *s, ULONG id, LONG min, LONG max,
	const char *format, const char *widest);
void ui_slider_set(UWORD page, struct ui_slider *s, LONG level);
void ui_slider_set_max(UWORD page, struct ui_slider *s, LONG max);
LONG ui_slider_poll(UWORD page, struct ui_slider *s);

/* Level bar with a short text inside it (no percentage, no ticks). */
struct ui_gauge {
	Object *obj;
	char text[2][24];
	UBYTE cur;
};
Object *ui_gauge_new(struct ui_gauge *g, LONG max);
void ui_gauge_set(UWORD page, struct ui_gauge *g, LONG level,
	const char *text);

/* Static label image for CHILD_Label. */
Object *ui_label(const char *text);

/* Exec list of chooser nodes built from a NULL-terminated array; freed
 * by ui_chooser_free_labels. */
struct List *ui_chooser_labels(const char *const *texts);
void ui_chooser_free_labels(struct List *list);

/* Single-column list browser node with user data, and its user data.
 * The build uses NO_INLINE_STDARG (the ReAction object macros need real
 * varargs functions), so class-library varargs calls such as
 * AllocListBrowserNode() do not exist: use these or the *A forms. */
struct Node *ui_lb_node(const char *text, ULONG userdata);
ULONG ui_lb_userdata(struct Node *node);

/* ---- status bar and pending state ---- */
void ui_status(const char *fmt, ...);
/* Recompute Save/Revert enablement and the pending summary. */
void ui_pending_changed(void);

/* ---- requesters ----
 * The body is word-wrapped to a sane width (EasyRequest never wraps).
 * Returns the EasyRequest gadget number (1..n-1, 0 for the last). */
LONG ui_ask(const char *gadgets, const char *fmt, ...);
void ui_error(const char *fmt, ...);
/* ASL file requester; FALSE when cancelled. */
BOOL ui_pick_file(const char *title, const char *pattern, char *path,
	UWORD path_size);

/* ---- busy state ----
 * Busy pointer plus an input-blocking requester; nests. */
void ui_busy(BOOL busy);

/* Bounded hardware waits and the other core services are declared in
 * frontend.h and implemented in ui.c. */

/* ---- time ---- */
ULONG ui_ticks(void);   /* 10 Hz counter since start */

/* The window, for modules that open requesters or screens. */
struct Window *ui_window(void);
struct Screen *ui_screen(void);

#endif /* ZZTOP_UI_H */
