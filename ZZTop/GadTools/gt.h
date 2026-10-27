/*
 * ZZTop GadTools front-end framework.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ZZTOP_GT_H
#define ZZTOP_GT_H

#include <exec/types.h>
#include <exec/lists.h>
#include <intuition/intuition.h>
#include <libraries/gadtools.h>

#include "frontend.h"

/* This front end's program name; the version is shared (Core/zztop.h). */
#define ZZTOP_NAME "ZZTop"

#define GT_GID(page, local) ((UWORD)(((page) + 1) * 100 + (local)))
#define GT_GID_PAGE(id) ((UWORD)((id) / 100 - 1))
#define GT_GID_LOCAL(id) ((UWORD)((id) % 100))
#define MAX_WORD(a, b) (((WORD)(a) > (WORD)(b)) ? (WORD)(a) : (WORD)(b))

#define GT_PAGE_ADVANCED 0x0001
#define GT_PAGE_CHILD    0x0002

enum gt_page_id {
	GT_PAGE_STATUS = 0,
	GT_PAGE_DISPLAY,
	GT_PAGE_CAPTURE,
	GT_PAGE_AUDIO,
	GT_PAGE_TUNING,
	GT_PAGE_SYSTEM,
	GT_PAGE_FIRMWARE,
	GT_PAGE_MODULES,
	GT_PAGE_DIAG,
	GT_PAGE_COUNT
};

struct gt_layout {
	const struct TextAttr *font;
	WORD font_x;
	WORD font_y;
	WORD border_left;
	WORD border_top;
	WORD width;
	WORD height;
	WORD page_left;
	WORD page_top;
	WORD page_width;
	WORD page_height;
	WORD row_height;
	WORD row_step;
	WORD section_gap;
	WORD body_top;
	WORD body_bottom;
	BOOL compact;
};

struct gt_page {
	const char *name;
	const char *title;
	const char *summary;
	UWORD flags;
	struct Gadget *gadgets;
	struct Gadget *tail;
	UWORD rows;
	BOOL (*create)(struct gt_page *page);
	void (*show)(void);
	void (*hide)(void);
	void (*gadget)(UWORD id, UWORD code);
	void (*tick)(void);
	void (*sync)(void);
	void (*flush)(void);
	void (*cleanup)(void);
};

extern struct gt_page *gt_pages[GT_PAGE_COUNT];
extern struct gt_layout gt_layout;

BOOL gt_create(void);
const char *gt_create_error(void);
void gt_run(void);
void gt_destroy(void);
void gt_flush(void);
void gt_pending_changed(void);
void gt_models_changed(void);
void gt_show_page(UWORD page);
BOOL gt_advanced(void);
void gt_set_advanced(BOOL on);
UWORD gt_current_page(void);
ULONG gt_ticks(void);
struct Window *gt_window(void);
/* The window if the gadget is attached to it (frame or visible page), else
 * NULL: pass this to GT_SetGadgetAttrs so hidden pages never draw. */
struct Window *gt_render_window(struct Gadget *gadget);
struct Screen *gt_screen(void);
APTR gt_visual_info(void);

/* Page gadget construction.  A page calls gt_page_begin once, then uses a
 * real TagItem array with gt_page_add for every gadget. */
void gt_page_begin(struct gt_page *page);
struct Gadget *gt_page_add(struct gt_page *page, ULONG kind,
	struct NewGadget *ng, struct TagItem *tags);
void gt_page_set_text(struct Gadget *gadget, const char *text);
void gt_page_set_disabled(struct Gadget *gadget, BOOL disabled);
ULONG gt_page_get(struct Gadget *gadget, ULONG tag);
WORD gt_row(UWORD row);
/* Text and button measurement helpers for form layout. */
WORD gt_text_width(const char *text);
WORD gt_max_text_width(const char *const *texts);
WORD gt_button_width(const char *text);
WORD gt_checkbox_total_width(const char *label);

/* Form layout context for consistent label/control column alignment. */
struct gt_form {
	struct gt_page *page;
	WORD left;
	WORD width;
	WORD label_col;
	WORD ctrl_left;
	WORD ctrl_width;
};

void gt_form_init(struct gt_form *form, struct gt_page *page,
	const char *const *labels);
struct Gadget *gt_form_field(struct gt_form *f, UWORD id, UWORD row,
	const char *label, char *text);
struct Gadget *gt_form_cycle(struct gt_form *f, UWORD id, UWORD row,
	const char *label, const char *const *choices);
struct Gadget *gt_form_slider(struct gt_form *f, UWORD id, UWORD row,
	const char *label, LONG min, LONG max, const char *format,
	const char *sample_max);
struct Gadget *gt_form_string(struct gt_form *f, UWORD id, UWORD row,
	const char *label, char *text, UWORD max_chars);
struct Gadget *gt_form_integer(struct gt_form *f, UWORD id, UWORD row,
	const char *label, LONG number, UWORD max_chars);
struct Gadget *gt_add_button(struct gt_page *page, UWORD id, WORD left,
	UWORD row, WORD width, const char *text);
struct Gadget *gt_add_note(struct gt_page *page, UWORD id, WORD left,
	UWORD row, WORD width, char *text);
struct Gadget *gt_add_checkbox(struct gt_page *page, UWORD id, WORD left,
	UWORD row, const char *label, BOOL checked);
void gt_draw_page(void);
void gt_status(const char *fmt, ...);
LONG gt_ask(const char *gadgets, const char *fmt, ...);
BOOL gt_pick_file(const char *title, const char *pattern, char *path,
	UWORD path_size);
void gt_busy(BOOL busy);

/* Core frontend service implementation lives in framework.c. */
void fe_status(const char *fmt, ...);
LONG fe_ask(const char *gadgets, const char *fmt, ...);
void fe_busy(BOOL busy);
void fe_flush(void);
void fe_models_changed(void);
void fe_pending_changed(void);
void fe_show(enum fe_place place);
ULONG fe_ticks(void);
struct Window *fe_window(void);
struct Screen *fe_screen(void);
void fe_wait_begin(struct fe_wait *wait, BOOL *cancel);
int fe_wait_keep_running(void *wait);
void fe_wait_end(struct fe_wait *wait);

#endif /* ZZTOP_GT_H */
