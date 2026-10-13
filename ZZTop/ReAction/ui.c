/*
 * ZZTop user interface framework (ReAction).
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/var.h>
#include <intuition/intuition.h>
#include <intuition/gadgetclass.h>
#include <intuition/icclass.h>
#include <graphics/text.h>
#include <libraries/gadtools.h>
#include <libraries/asl.h>
#include <devices/timer.h>

#include <classes/window.h>
#include <gadgets/layout.h>
#include <gadgets/button.h>
#include <gadgets/chooser.h>
#include <gadgets/checkbox.h>
#include <gadgets/string.h>
#include <gadgets/integer.h>
#include <gadgets/slider.h>
#include <gadgets/fuelgauge.h>
#include <gadgets/listbrowser.h>
#include <gadgets/space.h>
#include <images/label.h>
#include <images/bevel.h>
#include <reaction/reaction_macros.h>

#include <clib/alib_protos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/gadtools.h>
#include <proto/asl.h>
#include <proto/utility.h>
#include <proto/window.h>
#include <proto/layout.h>
#include <proto/button.h>
#include <proto/chooser.h>
#include <proto/checkbox.h>
#include <proto/string.h>
#include <proto/integer.h>
#include <proto/slider.h>
#include <proto/fuelgauge.h>
#include <proto/listbrowser.h>
#include <proto/space.h>
#include <proto/label.h>
#include <proto/bevel.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "zztop.h"
#include "ui.h"

/* ReAction class libraries. Opened in ui_open_classes; V44 is the
 * ReAction generation shipped with AmigaOS 3.5/3.9, and AmigaOS 3.2
 * carries newer revisions of the same classes. */
/* Explicit initialisers make these strong definitions: a tentative
 * (common) definition would let libnix link its own auto-open stubs,
 * which open the wrong paths for ReAction classes. */
struct Library *WindowBase = NULL, *LayoutBase = NULL, *ButtonBase = NULL,
	*ChooserBase = NULL, *CheckBoxBase = NULL, *StringBase = NULL,
	*IntegerBase = NULL, *SliderBase = NULL, *FuelGaugeBase = NULL,
	*ListBrowserBase = NULL, *SpaceBase = NULL, *LabelBase = NULL,
	*BevelBase = NULL;

extern struct Library *GadToolsBase;
extern struct Library *AslBase;

#define UI_CLASS_VERSION 44

#define GID_SIDEBAR  1
#define GID_SAVE     2
#define GID_REVERT   3

#define MENU_ABOUT    1
#define MENU_QUIT     2
#define MENU_SAVE     3
#define MENU_REVERT   4
#define MENU_ADVANCED 5

#define RAWKEY_ESCAPE 0x45

#define UI_ENV_ADVANCED "ZZTopAdvanced"

static struct Screen *screen;
static APTR visual_info;
static struct Menu *menu_strip;
static Object *window_obj;
static struct Window *window;
static Object *root_layout, *page_obj, *sidebar_obj, *save_obj, *revert_obj;
static struct ui_text title_text, summary_text, status_text;
static struct List sidebar_list;
static BOOL sidebar_list_ready;
static UWORD current_page = PAGE_STATUS;
static BOOL advanced_mode;
static UWORD busy_depth;
static struct Requester busy_req;
static BOOL quit_requested;
static char window_title[64];

static struct MsgPort *timer_port;
static struct timerequest *timer_io;
static BOOL timer_pending;
static ULONG tick_count;

/* ------------------------------------------------------------------ */
/* Class libraries                                                     */

struct class_lib {
	struct Library **base;
	const char *name;
};

static const struct class_lib class_libs[] = {
	{ &WindowBase,      "window.class" },
	{ &LayoutBase,      "gadgets/layout.gadget" },
	{ &ButtonBase,      "gadgets/button.gadget" },
	{ &ChooserBase,     "gadgets/chooser.gadget" },
	{ &CheckBoxBase,    "gadgets/checkbox.gadget" },
	{ &StringBase,      "gadgets/string.gadget" },
	{ &IntegerBase,     "gadgets/integer.gadget" },
	{ &SliderBase,      "gadgets/slider.gadget" },
	{ &FuelGaugeBase,   "gadgets/fuelgauge.gadget" },
	{ &ListBrowserBase, "gadgets/listbrowser.gadget" },
	{ &SpaceBase,       "gadgets/space.gadget" },
	{ &LabelBase,       "images/label.image" },
	{ &BevelBase,       "images/bevel.image" },
};

#define CLASS_LIB_COUNT (sizeof(class_libs) / sizeof(class_libs[0]))

BOOL ui_open_classes(char *missing, UWORD missing_size)
{
	ULONG i;
	BOOL ok = TRUE;

	missing[0] = '\0';
	for (i = 0; i < CLASS_LIB_COUNT; i++) {
		*class_libs[i].base = OpenLibrary(
			(CONST_STRPTR)class_libs[i].name, UI_CLASS_VERSION);
		if (!*class_libs[i].base) {
			size_t used = strlen(missing);
			snprintf(missing + used, missing_size - used, "%s%s",
				used ? ", " : "", class_libs[i].name);
			ok = FALSE;
		}
	}
	return ok;
}

void ui_close_classes(void)
{
	ULONG i;

	for (i = CLASS_LIB_COUNT; i-- > 0;) {
		if (*class_libs[i].base) {
			CloseLibrary(*class_libs[i].base);
			*class_libs[i].base = NULL;
		}
	}
}

/* ------------------------------------------------------------------ */
/* Gadget helpers                                                      */

#define UI_FRAME_PAGE 0xffff

UWORD ui_current_page(void)
{
	return current_page;
}

BOOL ui_page_visible(UWORD page)
{
	return window && (page == UI_FRAME_PAGE || page == current_page);
}

void ui_setA(UWORD page, Object *o, struct TagItem *tags)
{
	if (!o) return;
	if (ui_page_visible(page)) {
		if (SetGadgetAttrsA((struct Gadget *)o, window, NULL, tags))
			RefreshGList((struct Gadget *)o, window, NULL, 1);
	} else {
		SetAttrsA(o, tags);
	}
}

void ui_set(UWORD page, Object *o, ULONG tag, ...)
{
	va_list ap;
	struct TagItem tags[8];
	int n = 0;

	va_start(ap, tag);
	while (tag != TAG_END && n < 7) {
		tags[n].ti_Tag = tag;
		tags[n].ti_Data = va_arg(ap, ULONG);
		n++;
		tag = va_arg(ap, ULONG);
	}
	va_end(ap);
	tags[n].ti_Tag = TAG_END;
	tags[n].ti_Data = 0;
	ui_setA(page, o, tags);
}

ULONG ui_get(Object *o, ULONG attr)
{
	ULONG value = 0;

	if (o) GetAttr(attr, o, &value);
	return value;
}

void ui_disable(UWORD page, Object *o, BOOL disabled)
{
	if (!o) return;
	if ((ui_get(o, GA_Disabled) != 0) == (disabled != FALSE)) return;
	ui_set(page, o, GA_Disabled, disabled ? TRUE : FALSE, TAG_END);
}

static Object *text_new(struct ui_text *t, const char *initial, BOOL framed)
{
	t->cur = 0;
	snprintf(t->buf[0], UI_TEXT_MAX, "%s", initial ? initial : "");
	t->buf[1][0] = '\0';
	/* GA_Underscore 0: underscores in values (ENV:ZZ9K_MAC) are text,
	 * not keyboard-shortcut markers. */
	t->obj = NewObject(BUTTON_GetClass(), NULL,
		GA_ReadOnly, TRUE,
		GA_Underscore, 0,
		GA_Text, (ULONG)t->buf[0],
		BUTTON_Justification, BCJ_LEFT,
		BUTTON_BevelStyle, framed ? BVS_FIELD : BVS_NONE,
		BUTTON_Transparent, framed ? FALSE : TRUE,
		TAG_END);
	return t->obj;
}

Object *ui_text_new(struct ui_text *t, const char *initial)
{
	return text_new(t, initial, TRUE);
}

Object *ui_note_new(struct ui_text *t, const char *initial)
{
	return text_new(t, initial, FALSE);
}

void ui_text_set(UWORD page, struct ui_text *t, const char *text)
{
	UBYTE next;

	if (!t->obj) return;
	if (!text) text = "";
	if (strncmp(t->buf[t->cur], text, UI_TEXT_MAX - 1) == 0) return;
	next = (UBYTE)(t->cur ^ 1);
	snprintf(t->buf[next], UI_TEXT_MAX, "%s", text);
	t->cur = next;
	ui_set(page, t->obj, GA_Text, (ULONG)t->buf[next], TAG_END);
}

void ui_text_printf(UWORD page, struct ui_text *t, const char *fmt, ...)
{
	char tmp[UI_TEXT_MAX];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(tmp, sizeof(tmp), fmt, ap);
	va_end(ap);
	ui_text_set(page, t, tmp);
}

Object *ui_label(const char *text)
{
	return NewObject(LABEL_GetClass(), NULL,
		LABEL_Text, (ULONG)text,
		TAG_END);
}

Object *ui_slider_new(struct ui_slider *s, ULONG id, LONG min, LONG max,
	const char *format, const char *widest)
{
	s->format = format;
	s->shown = min - 1;
	s->obj = NewObject(SLIDER_GetClass(), NULL,
		GA_ID, id,
		GA_RelVerify, TRUE,
		SLIDER_Min, (ULONG)min,
		SLIDER_Max, (ULONG)max,
		SLIDER_Level, (ULONG)min,
		SLIDER_Orientation, SLIDER_HORIZONTAL,
		TAG_END);
	/* The value field starts with the widest text so the layout reserves
	 * its width; ui_slider_set replaces it before the window opens. */
	return NewObject(LAYOUT_GetClass(), NULL,
		LAYOUT_Orientation, LAYOUT_HORIZONTAL,
		LAYOUT_AddChild, (ULONG)s->obj,
		LAYOUT_AddChild, (ULONG)ui_note_new(&s->value, widest),
		CHILD_WeightedWidth, 0,
		TAG_END);
}

static void slider_show_value(UWORD page, struct ui_slider *s, LONG level)
{
	if (level == s->shown) return;
	s->shown = level;
	ui_text_printf(page, &s->value, s->format, (long)level);
}

void ui_slider_set(UWORD page, struct ui_slider *s, LONG level)
{
	ui_set(page, s->obj, SLIDER_Level, (ULONG)level, TAG_END);
	slider_show_value(page, s, level);
}

void ui_slider_set_max(UWORD page, struct ui_slider *s, LONG max)
{
	if ((LONG)ui_get(s->obj, SLIDER_Max) == max) return;
	ui_set(page, s->obj, SLIDER_Max, (ULONG)max, TAG_END);
}

LONG ui_slider_poll(UWORD page, struct ui_slider *s)
{
	LONG level = (LONG)ui_get(s->obj, SLIDER_Level);

	slider_show_value(page, s, level);
	return level;
}

Object *ui_gauge_new(struct ui_gauge *g, LONG max)
{
	g->cur = 0;
	g->text[0][0] = '\0';
	g->text[1][0] = '\0';
	g->obj = NewObject(FUELGAUGE_GetClass(), NULL,
		FUELGAUGE_Min, 0,
		FUELGAUGE_Max, (ULONG)max,
		FUELGAUGE_Level, 0,
		FUELGAUGE_Percent, FALSE,
		FUELGAUGE_Ticks, 0,
		FUELGAUGE_Justification, FGJ_CENTER,
		GA_Text, (ULONG)g->text[0],
		TAG_END);
	return g->obj;
}

void ui_gauge_set(UWORD page, struct ui_gauge *g, LONG level,
	const char *text)
{
	UBYTE next = (UBYTE)(g->cur ^ 1);
	char *dst = g->text[next];
	size_t i;

	/* GA_Text is a format string for the gauge: never let a '%' in. */
	for (i = 0; text && text[i] && i < sizeof(g->text[0]) - 1; i++)
		dst[i] = text[i] == '%' ? ' ' : text[i];
	dst[i] = '\0';
	if ((LONG)ui_get(g->obj, FUELGAUGE_Level) == level &&
			strcmp(dst, g->text[g->cur]) == 0)
		return;
	g->cur = next;
	ui_set(page, g->obj, FUELGAUGE_Level, (ULONG)level,
		GA_Text, (ULONG)dst, TAG_END);
}

struct Node *ui_lb_node(const char *text, ULONG userdata)
{
	struct TagItem tags[3];

	tags[0].ti_Tag = LBNCA_Text;
	tags[0].ti_Data = (ULONG)text;
	tags[1].ti_Tag = LBNA_UserData;
	tags[1].ti_Data = userdata;
	tags[2].ti_Tag = TAG_END;
	tags[2].ti_Data = 0;
	return AllocListBrowserNodeA(1, tags);
}

ULONG ui_lb_userdata(struct Node *node)
{
	struct TagItem tags[2];
	ULONG ud = 0;

	tags[0].ti_Tag = LBNA_UserData;
	tags[0].ti_Data = (ULONG)&ud;
	tags[1].ti_Tag = TAG_END;
	tags[1].ti_Data = 0;
	GetListBrowserNodeAttrsA(node, tags);
	return ud;
}

struct List *ui_chooser_labels(const char *const *texts)
{
	struct List *list = AllocMem(sizeof(*list), MEMF_PUBLIC | MEMF_CLEAR);
	int i;

	if (!list) return NULL;
	NewList(list);
	for (i = 0; texts[i]; i++) {
		struct TagItem tags[2];
		struct Node *node;

		tags[0].ti_Tag = CNA_Text;
		tags[0].ti_Data = (ULONG)texts[i];
		tags[1].ti_Tag = TAG_END;
		tags[1].ti_Data = 0;
		node = AllocChooserNodeA(tags);
		if (!node) {
			ui_chooser_free_labels(list);
			return NULL;
		}
		AddTail(list, node);
	}
	return list;
}

void ui_chooser_free_labels(struct List *list)
{
	struct Node *node;

	if (!list) return;
	while ((node = RemHead(list)))
		FreeChooserNode(node);
	FreeMem(list, sizeof(*list));
}

/* ------------------------------------------------------------------ */
/* Status bar, title and pending state                                 */

void ui_status(const char *fmt, ...)
{
	char tmp[UI_TEXT_MAX];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(tmp, sizeof(tmp), fmt, ap);
	va_end(ap);
	ui_text_set(UI_FRAME_PAGE, &status_text, tmp);
}

void ui_pending_changed(void)
{
	struct app_pending pending;
	BOOL any;
	char title[sizeof(window_title)];

	app_pending(&pending);
	any = pending.card || pending.live || pending.audio;
	ui_disable(UI_FRAME_PAGE, save_obj,
		!(pending.card || pending.audio));
	ui_disable(UI_FRAME_PAGE, revert_obj, !any);

	if (pending.card && pending.reboot)
		snprintf(title, sizeof(title), ZZTOP_NAME " - %u unsaved, "
			"reboot needed", (unsigned)pending.card);
	else if (pending.card)
		snprintf(title, sizeof(title), ZZTOP_NAME " - %u unsaved",
			(unsigned)pending.card);
	else if (pending.audio)
		snprintf(title, sizeof(title), ZZTOP_NAME " - audio unsaved");
	else if (pending.live)
		snprintf(title, sizeof(title), ZZTOP_NAME " - live changes");
	else
		snprintf(title, sizeof(title), ZZTOP_NAME);

	if (strcmp(title, window_title) != 0) {
		strcpy(window_title, title);
		if (window)
			SetWindowTitles(window, (CONST_STRPTR)window_title,
				(CONST_STRPTR)~0);
	}
}

/* ------------------------------------------------------------------ */
/* Requesters                                                          */

/* EasyRequest never wraps: break long lines at word boundaries. */
static void wrap_text(char *out, size_t size, const char *in, UWORD width)
{
	size_t o = 0;
	UWORD col = 0;

	while (*in && o + 1 < size) {
		if (*in == '\n') {
			out[o++] = *in++;
			col = 0;
			continue;
		}
		if (*in == ' ' && col > 0) {
			const char *w = in + 1;
			UWORD len = 0;

			while (w[len] && w[len] != ' ' && w[len] != '\n') len++;
			if (col + 1 + len > width) {
				out[o++] = '\n';
				col = 0;
				in++;
				continue;
			}
		}
		out[o++] = *in++;
		col++;
	}
	out[o] = '\0';
}

static LONG ask_va(const char *gadgets, const char *fmt, va_list ap)
{
	static char raw[512], body[560];
	struct EasyStruct es;
	ULONG args[1];

	vsnprintf(raw, sizeof(raw), fmt, ap);
	wrap_text(body, sizeof(body), raw, 56);
	es.es_StructSize = sizeof(es);
	es.es_Flags = 0;
	es.es_Title = (UBYTE *)ZZTOP_NAME;
	es.es_TextFormat = (UBYTE *)"%s";
	es.es_GadgetFormat = (UBYTE *)gadgets;
	args[0] = (ULONG)body;
	return EasyRequestArgs(window, &es, NULL, args);
}

LONG ui_ask(const char *gadgets, const char *fmt, ...)
{
	va_list ap;
	LONG r;

	va_start(ap, fmt);
	r = ask_va(gadgets, fmt, ap);
	va_end(ap);
	return r;
}

void ui_error(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	ask_va("OK", fmt, ap);
	va_end(ap);
}

BOOL ui_pick_file(const char *title, const char *pattern, char *path,
	UWORD path_size)
{
	struct FileRequester *fr;
	BOOL ok = FALSE;

	if (!AslBase) {
		ui_error("asl.library is needed for the file requester.");
		return FALSE;
	}
	fr = AllocAslRequestTags(ASL_FileRequest,
		ASLFR_TitleText, (ULONG)title,
		ASLFR_DoPatterns, pattern ? TRUE : FALSE,
		ASLFR_InitialPattern, (ULONG)(pattern ? pattern : "#?"),
		ASLFR_Window, (ULONG)window,
		ASLFR_SleepWindow, TRUE,
		TAG_END);
	if (!fr) return FALSE;
	if (AslRequestTags(fr, TAG_END)) {
		strncpy(path, (const char *)fr->fr_Drawer, path_size - 1);
		path[path_size - 1] = '\0';
		if (!AddPart((STRPTR)path, fr->fr_File, path_size)) {
			ui_error("That file path is too long.");
		} else {
			ok = TRUE;
		}
	}
	FreeAslRequest(fr);
	return ok;
}

/* ------------------------------------------------------------------ */
/* Busy state and bounded waits                                        */

void ui_busy(BOOL busy)
{
	if (busy) {
		if (busy_depth++ == 0 && window) {
			InitRequester(&busy_req);
			Request(&busy_req, window);
			SetWindowPointer(window, WA_BusyPointer, TRUE, TAG_END);
		}
	} else if (busy_depth && --busy_depth == 0 && window) {
		EndRequest(&busy_req, window);
		SetWindowPointer(window, WA_Pointer, NULL, TAG_END);
	}
}

/* ------------------------------------------------------------------ */
/* Front-end services for the shared core (frontend.h)                */

void fe_wait_begin(struct fe_wait *wait, BOOL *cancel)
{
	wait->cancel = cancel;
	NewList(&wait->deferred);
}

int fe_wait_keep_running(void *ctx)
{
	struct fe_wait *wait = ctx;
	struct IntuiMessage *msg;

	if (!window) return !*wait->cancel;
	while ((msg = (struct IntuiMessage *)GetMsg(window->UserPort))) {
		if (msg->Class == IDCMP_CLOSEWINDOW ||
				(msg->Class == IDCMP_RAWKEY &&
				 msg->Code == RAWKEY_ESCAPE)) {
			*wait->cancel = TRUE;
			ReplyMsg((struct Message *)msg);
		} else {
			AddTail(&wait->deferred, (struct Node *)msg);
		}
	}
	return !*wait->cancel;
}

void fe_wait_end(struct fe_wait *wait)
{
	struct Message *msg;

	/* Deferred messages go back to the port; the main loop processes
	 * them exactly once through WM_HANDLEINPUT. */
	while ((msg = (struct Message *)RemHead(&wait->deferred)))
		PutMsg(window->UserPort, msg);
}

void fe_status(const char *fmt, ...)
{
	char tmp[UI_TEXT_MAX];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(tmp, sizeof(tmp), fmt, ap);
	va_end(ap);
	ui_status("%s", tmp);
}

LONG fe_ask(const char *gadgets, const char *fmt, ...)
{
	va_list ap;
	LONG r;

	va_start(ap, fmt);
	r = ask_va(gadgets, fmt, ap);
	va_end(ap);
	return r;
}

void fe_busy(BOOL busy)
{
	ui_busy(busy);
}

void fe_flush(void)
{
	ui_flush();
}

void fe_models_changed(void)
{
	UWORD i;

	for (i = 0; i < PAGE_COUNT; i++)
		if (ui_pages[i]->sync) ui_pages[i]->sync();
	ui_pending_changed();
}

void fe_pending_changed(void)
{
	ui_pending_changed();
}

void fe_show(enum fe_place place)
{
	ui_show_page(place == FE_PLACE_CAPTURE ? PAGE_CAPTURE : PAGE_SYSTEM);
}

ULONG fe_ticks(void)
{
	return ui_ticks();
}

struct Window *fe_window(void)
{
	return ui_window();
}

struct Screen *fe_screen(void)
{
	return ui_screen();
}

/* ------------------------------------------------------------------ */
/* Timer                                                               */

static BOOL timer_open(void)
{
	timer_port = CreateMsgPort();
	if (!timer_port) return FALSE;
	timer_io = (struct timerequest *)CreateIORequest(timer_port,
		sizeof(struct timerequest));
	if (!timer_io) return FALSE;
	if (OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_VBLANK,
			(struct IORequest *)timer_io, 0)) {
		DeleteIORequest((struct IORequest *)timer_io);
		timer_io = NULL;
		return FALSE;
	}
	return TRUE;
}

static void timer_arm(void)
{
	timer_io->tr_node.io_Command = TR_ADDREQUEST;
	timer_io->tr_time.tv_secs = 0;
	timer_io->tr_time.tv_micro = 100000;
	SendIO((struct IORequest *)timer_io);
	timer_pending = TRUE;
}

static void timer_close(void)
{
	if (timer_io) {
		if (timer_pending) {
			AbortIO((struct IORequest *)timer_io);
			WaitIO((struct IORequest *)timer_io);
			timer_pending = FALSE;
		}
		CloseDevice((struct IORequest *)timer_io);
		DeleteIORequest((struct IORequest *)timer_io);
		timer_io = NULL;
	}
	if (timer_port) {
		DeleteMsgPort(timer_port);
		timer_port = NULL;
	}
}

ULONG ui_ticks(void)
{
	return tick_count;
}

/* ------------------------------------------------------------------ */
/* Page list                                                           */

static BOOL page_listed(UWORD page)
{
	return advanced_mode || !(ui_pages[page]->flags & UI_PAGE_ADVANCED);
}

/* The nearest listed page above an advanced one, for when advanced mode
 * is switched off while it is showing (a child's parent, or the page
 * above a top-level advanced page such as Diagnostics). */
static UWORD page_parent(UWORD page)
{
	while (page > 0 && !page_listed(page)) page--;
	return page;
}

static void sidebar_free(void)
{
	struct Node *node;

	if (!sidebar_list_ready) return;
	while ((node = RemHead(&sidebar_list)))
		FreeListBrowserNode(node);
}

static void sidebar_build(void)
{
	static char child_names[PAGE_COUNT][32];
	UWORD page;

	if (!sidebar_list_ready) {
		NewList(&sidebar_list);
		sidebar_list_ready = TRUE;
	}
	for (page = 0; page < PAGE_COUNT; page++) {
		const char *name = ui_pages[page]->name;
		struct Node *node;

		if (!page_listed(page)) continue;
		if (ui_pages[page]->flags & UI_PAGE_CHILD) {
			snprintf(child_names[page], sizeof(child_names[page]),
				"   %s", name);
			name = child_names[page];
		}
		node = ui_lb_node(name, page);
		if (node) AddTail(&sidebar_list, node);
	}
}

static LONG sidebar_index_of(UWORD page)
{
	struct Node *node;
	LONG index = 0;

	for (node = sidebar_list.lh_Head; node->ln_Succ;
			node = node->ln_Succ, index++) {
		ULONG ud = 0;
		ud = ui_lb_userdata(node);
		if (ud == page) return index;
	}
	return -1;
}

static void sidebar_select(UWORD page)
{
	LONG index = sidebar_index_of(page);

	if (index >= 0)
		ui_set(UI_FRAME_PAGE, sidebar_obj,
			LISTBROWSER_Selected, (ULONG)index,
			LISTBROWSER_MakeVisible, (ULONG)index, TAG_END);
}

static void sidebar_rebuild(void)
{
	if (window)
		SetGadgetAttrs((struct Gadget *)sidebar_obj, window, NULL,
			LISTBROWSER_Labels, ~0UL, TAG_END);
	else
		SetAttrs(sidebar_obj, LISTBROWSER_Labels, ~0UL, TAG_END);
	sidebar_free();
	sidebar_build();
	ui_set(UI_FRAME_PAGE, sidebar_obj,
		LISTBROWSER_Labels, (ULONG)&sidebar_list, TAG_END);
	sidebar_select(current_page);
}

void ui_flush(void)
{
	if (ui_pages[current_page]->flush) ui_pages[current_page]->flush();
}

void ui_show_page(UWORD page)
{
	UWORD old = current_page;

	if (page >= PAGE_COUNT) return;
	if (!page_listed(page)) ui_set_advanced(TRUE);
	if (page != old && ui_pages[old]->hide) ui_pages[old]->hide();
	current_page = page;
	ui_text_set(UI_FRAME_PAGE, &title_text, ui_pages[page]->title);
	ui_text_set(UI_FRAME_PAGE, &summary_text, ui_pages[page]->summary);
	if (window) {
		SetGadgetAttrs((struct Gadget *)page_obj, window, NULL,
			PAGE_Current, (ULONG)page, TAG_END);
		RethinkLayout((struct Gadget *)root_layout, window, NULL, TRUE);
	} else {
		SetAttrs(page_obj, PAGE_Current, (ULONG)page, TAG_END);
	}
	sidebar_select(page);
	if (ui_pages[page]->show) ui_pages[page]->show();
}

BOOL ui_advanced(void)
{
	return advanced_mode;
}

static void advanced_menu_sync(void)
{
	struct MenuItem *item;

	if (!menu_strip) return;
	item = ItemAddress(menu_strip, FULLMENUNUM(2, 0, NOSUB));
	if (!item) return;
	if (window) ClearMenuStrip(window);
	if (advanced_mode) item->Flags |= CHECKED;
	else item->Flags &= ~CHECKED;
	if (window) ResetMenuStrip(window, menu_strip);
}

void ui_set_advanced(BOOL on)
{
	on = on ? TRUE : FALSE;
	if (advanced_mode == on) return;
	advanced_mode = on;
	SetVar((STRPTR)UI_ENV_ADVANCED, (STRPTR)(on ? "1" : "0"), 1,
		GVF_GLOBAL_ONLY | GVF_SAVE_VAR);
	advanced_menu_sync();
	if (!on && (ui_pages[current_page]->flags & UI_PAGE_ADVANCED))
		ui_show_page(page_parent(current_page));
	sidebar_rebuild();
}

static BOOL advanced_load(void)
{
	char value[4];

	if (GetVar((STRPTR)UI_ENV_ADVANCED, (STRPTR)value, sizeof(value),
			GVF_GLOBAL_ONLY) > 0)
		return value[0] == '1';
	return FALSE;
}

/* ------------------------------------------------------------------ */
/* Window construction                                                 */

static struct NewMenu new_menus[] = {
	{ NM_TITLE, (STRPTR)"Project",          NULL, 0, 0, NULL },
	{  NM_ITEM, (STRPTR)"About...",         (STRPTR)"?", 0, 0,
		(APTR)MENU_ABOUT },
	{  NM_ITEM, NM_BARLABEL,                NULL, 0, 0, NULL },
	{  NM_ITEM, (STRPTR)"Quit",             (STRPTR)"Q", 0, 0,
		(APTR)MENU_QUIT },
	{ NM_TITLE, (STRPTR)"Edit",             NULL, 0, 0, NULL },
	{  NM_ITEM, (STRPTR)"Save",             (STRPTR)"S", 0, 0,
		(APTR)MENU_SAVE },
	{  NM_ITEM, (STRPTR)"Revert",           (STRPTR)"R", 0, 0,
		(APTR)MENU_REVERT },
	{ NM_TITLE, (STRPTR)"Settings",         NULL, 0, 0, NULL },
	{  NM_ITEM, (STRPTR)"Advanced controls", (STRPTR)"A",
		CHECKIT | MENUTOGGLE, 0, (APTR)MENU_ADVANCED },
	{ NM_END, NULL, NULL, 0, 0, NULL }
};

/* Wide enough for every page name (advanced ones included, so toggling
 * the mode never re-lays out the window) plus the list's frame and
 * scroller. */
static UWORD sidebar_min_width(void)
{
	struct RastPort rp;
	UWORD page, widest = 0;

	InitRastPort(&rp);
	SetFont(&rp, screen->RastPort.Font);
	for (page = 0; page < PAGE_COUNT; page++) {
		const char *name = ui_pages[page]->name;
		UWORD w = (UWORD)TextLength(&rp, (CONST_STRPTR)name, strlen(name));

		if (ui_pages[page]->flags & UI_PAGE_CHILD)
			w += (UWORD)TextLength(&rp, (CONST_STRPTR)"   ", 3);
		if (w > widest) widest = w;
	}
	return (UWORD)(widest + 32);
}

static Object *build_root(void)
{
	Object *pages[PAGE_COUNT];
	Object *content, *statusbar;
	UWORD i;

	/* Every page body keeps its natural height at the top of the page
	 * area; the spacer under it takes whatever the tallest page leaves
	 * free, so rows never stretch. */
	for (i = 0; i < PAGE_COUNT; i++) {
		Object *body = ui_pages[i]->create();

		pages[i] = NULL;
		if (body)
			pages[i] = NewObject(LAYOUT_GetClass(), NULL,
				LAYOUT_Orientation, LAYOUT_VERTICAL,
				LAYOUT_AddChild, (ULONG)body,
				CHILD_WeightedHeight, 0,
				LAYOUT_AddChild, (ULONG)NewObject(SPACE_GetClass(),
					NULL, TAG_END),
				TAG_END);
		if (!pages[i]) {
			/* Nothing owns the pages built so far yet. */
			while (i-- > 0) DisposeObject(pages[i]);
			return NULL;
		}
	}

	page_obj = NewObject(PAGE_GetClass(), NULL,
		PAGE_Add, (ULONG)pages[0], PAGE_Add, (ULONG)pages[1],
		PAGE_Add, (ULONG)pages[2], PAGE_Add, (ULONG)pages[3],
		PAGE_Add, (ULONG)pages[4], PAGE_Add, (ULONG)pages[5],
		PAGE_Add, (ULONG)pages[6], PAGE_Add, (ULONG)pages[7],
		PAGE_Current, (ULONG)current_page,
		TAG_END);
	if (!page_obj) {
		for (i = 0; i < PAGE_COUNT; i++) DisposeObject(pages[i]);
		return NULL;
	}

	sidebar_build();
	sidebar_obj = NewObject(LISTBROWSER_GetClass(), NULL,
		GA_ID, GID_SIDEBAR,
		GA_RelVerify, TRUE,
		LISTBROWSER_Labels, (ULONG)&sidebar_list,
		LISTBROWSER_ShowSelected, TRUE,
		LISTBROWSER_Selected, (ULONG)sidebar_index_of(current_page),
		TAG_END);

	title_text.obj = NewObject(BUTTON_GetClass(), NULL,
		GA_ReadOnly, TRUE,
		GA_Text, (ULONG)title_text.buf[0],
		BUTTON_Justification, BCJ_LEFT,
		BUTTON_BevelStyle, BVS_NONE,
		BUTTON_Transparent, TRUE,
		BUTTON_SoftStyle, FSF_BOLD,
		TAG_END);

	content = NewObject(LAYOUT_GetClass(), NULL,
		LAYOUT_Orientation, LAYOUT_VERTICAL,
		LAYOUT_SpaceOuter, TRUE,
		LAYOUT_AddChild, (ULONG)title_text.obj,
		CHILD_WeightedHeight, 0,
		LAYOUT_AddChild, (ULONG)ui_note_new(&summary_text,
			ui_pages[current_page]->summary),
		CHILD_WeightedHeight, 0,
		LAYOUT_AddChild, (ULONG)page_obj,
		TAG_END);

	statusbar = NewObject(LAYOUT_GetClass(), NULL,
		LAYOUT_Orientation, LAYOUT_HORIZONTAL,
		LAYOUT_AddChild, (ULONG)ui_text_new(&status_text, "Ready."),
		LAYOUT_AddChild, (ULONG)(revert_obj = NewObject(
			BUTTON_GetClass(), NULL,
			GA_ID, GID_REVERT,
			GA_RelVerify, TRUE,
			GA_Text, (ULONG)"_Revert",
			GA_Disabled, TRUE,
			TAG_END)),
		CHILD_WeightedWidth, 0,
		LAYOUT_AddChild, (ULONG)(save_obj = NewObject(
			BUTTON_GetClass(), NULL,
			GA_ID, GID_SAVE,
			GA_RelVerify, TRUE,
			GA_Text, (ULONG)"_Save",
			GA_Disabled, TRUE,
			TAG_END)),
		CHILD_WeightedWidth, 0,
		TAG_END);

	return NewObject(LAYOUT_GetClass(), NULL,
		LAYOUT_Orientation, LAYOUT_VERTICAL,
		LAYOUT_SpaceOuter, TRUE,
		LAYOUT_DeferLayout, TRUE,
		LAYOUT_AddChild, (ULONG)NewObject(LAYOUT_GetClass(), NULL,
			LAYOUT_Orientation, LAYOUT_HORIZONTAL,
			LAYOUT_AddChild, (ULONG)sidebar_obj,
			CHILD_WeightedWidth, 0,
			CHILD_MinWidth, (ULONG)sidebar_min_width(),
			LAYOUT_AddChild, (ULONG)content,
			TAG_END),
		LAYOUT_AddChild, (ULONG)statusbar,
		CHILD_WeightedHeight, 0,
		TAG_END);
}

BOOL ui_too_small;

/* The window cannot shrink below its layout's minimum. On a screen that
 * cannot hold it (a 640x256 PAL Workbench), say so instead of opening a
 * clipped, unusable window: the GadTools ZZTop is laid out for that. */
static BOOL fits_screen(void)
{
	struct gpDomain d;
	LONG width, height;

	memset(&d, 0, sizeof(d));
	d.MethodID = GM_DOMAIN;
	d.gpd_RPort = &screen->RastPort;
	d.gpd_Which = GDOMAIN_MINIMUM;
	DoMethodA(root_layout, (Msg)&d);
	width = d.gpd_Domain.Width + screen->WBorLeft + screen->WBorRight;
	height = d.gpd_Domain.Height + screen->WBorTop +
		screen->Font->ta_YSize + 1 + 10;   /* bottom border, size gadget */
	return width <= screen->Width && height <= screen->Height;
}

BOOL ui_create(void)
{
	UWORD i;

	advanced_mode = advanced_load();
	for (i = 0; i < PAGE_COUNT; i++) {
		if (!page_listed(i) && i == current_page)
			current_page = page_parent(i);
	}
	snprintf(title_text.buf[0], UI_TEXT_MAX, "%s",
		ui_pages[current_page]->title);
	strcpy(window_title, ZZTOP_NAME);

	screen = LockPubScreen(NULL);
	if (!screen) return FALSE;
	visual_info = GetVisualInfo(screen, TAG_END);
	if (!visual_info) return FALSE;

	menu_strip = CreateMenus(new_menus, TAG_END);
	if (menu_strip && !LayoutMenus(menu_strip, visual_info,
			GTMN_NewLookMenus, TRUE, TAG_END)) {
		FreeMenus(menu_strip);
		menu_strip = NULL;
	}
	advanced_menu_sync();

	if (!timer_open()) return FALSE;

	root_layout = build_root();
	if (!root_layout) return FALSE;
	if (!fits_screen()) {
		ui_too_small = TRUE;
		return FALSE;
	}

	window_obj = NewObject(WINDOW_GetClass(), NULL,
		WA_Title, (ULONG)window_title,
		WA_ScreenTitle, (ULONG)(ZZTOP_NAME " " ZZTOP_RELEASE
			" - ZZ9000 control panel"),
		WA_PubScreen, (ULONG)screen,
		WA_DragBar, TRUE,
		WA_DepthGadget, TRUE,
		WA_CloseGadget, TRUE,
		WA_SizeGadget, TRUE,
		WA_Activate, TRUE,
		WA_SmartRefresh, TRUE,
		WA_NewLookMenus, TRUE,
		WA_IDCMP, IDCMP_RAWKEY | IDCMP_MENUPICK,
		WINDOW_Position, WPOS_CENTERSCREEN,
		WINDOW_MenuStrip, (ULONG)menu_strip,
		WINDOW_Layout, (ULONG)root_layout,
		TAG_END);
	if (!window_obj) return FALSE;

	window = (struct Window *)DoMethod(window_obj, WM_OPEN, NULL);
	if (!window) return FALSE;

	for (i = 0; i < PAGE_COUNT; i++)
		if (ui_pages[i]->sync) ui_pages[i]->sync();
	ui_show_page(current_page);
	ui_pending_changed();
	return TRUE;
}

struct Window *ui_window(void)
{
	return window;
}

struct Screen *ui_screen(void)
{
	return screen;
}

/* ------------------------------------------------------------------ */
/* Event loop                                                          */

static void about(void)
{
	ui_ask("OK",
		ZZTOP_NAME " " ZZTOP_RELEASE " (" ZZTOP_DATE ")\n"
		"The ZZ9000 control panel.\n\n"
		"Copyright 2016-2026 Lucie L. Hartmann, MNT Research GmbH\n"
		"Copyright 2026 Dimitris Panokostas\n"
		"Licensed under the GNU GPL v3 or later.");
}

static void handle_menu(UWORD code)
{
	while (code != MENUNULL) {
		struct MenuItem *item = ItemAddress(menu_strip, code);
		ULONG id;

		if (!item) break;
		id = (ULONG)GTMENUITEM_USERDATA(item);
		switch (id) {
		case MENU_ABOUT:
			about();
			break;
		case MENU_QUIT:
			if (app_quit_allowed()) quit_requested = TRUE;
			break;
		case MENU_SAVE:
			if (app_has_changes()) app_save();
			break;
		case MENU_REVERT:
			if (app_has_changes()) app_revert();
			break;
		case MENU_ADVANCED:
			ui_set_advanced((item->Flags & CHECKED) != 0);
			break;
		}
		code = item->NextSelect;
	}
}

static void handle_gadget(ULONG id, UWORD code)
{
	WORD page;

	switch (id) {
	case GID_SIDEBAR: {
		struct Node *node = (struct Node *)ui_get(sidebar_obj,
			LISTBROWSER_SelectedNode);
		ULONG ud = current_page;

		if (node) ud = ui_lb_userdata(node);
		if ((UWORD)ud != current_page) ui_show_page((UWORD)ud);
		return;
	}
	case GID_SAVE:
		app_save();
		return;
	case GID_REVERT:
		app_revert();
		return;
	}
	page = UI_GID_PAGE(id);
	if (page >= 0 && page < PAGE_COUNT && ui_pages[page]->gadget)
		ui_pages[page]->gadget(UI_GID_LOCAL(id), code);
}

void ui_run(void)
{
	ULONG window_sig = 0, timer_sig = 1UL << timer_port->mp_SigBit;

	GetAttr(WINDOW_SigMask, window_obj, &window_sig);
	timer_arm();

	while (!quit_requested) {
		ULONG sigs = Wait(window_sig | timer_sig | SIGBREAKF_CTRL_C);
		ULONG result;
		UWORD code;

		if (sigs & SIGBREAKF_CTRL_C) {
			if (app_quit_allowed()) break;
		}
		if ((sigs & timer_sig) && CheckIO((struct IORequest *)timer_io)) {
			WaitIO((struct IORequest *)timer_io);
			timer_pending = FALSE;
			tick_count++;
			app_tick();
			if (ui_pages[current_page]->tick)
				ui_pages[current_page]->tick();
			timer_arm();
		}
		while ((result = DoMethod(window_obj, WM_HANDLEINPUT, &code))
				!= WMHI_LASTMSG) {
			switch (result & WMHI_CLASSMASK) {
			case WMHI_CLOSEWINDOW:
				if (app_quit_allowed()) quit_requested = TRUE;
				break;
			case WMHI_GADGETUP:
				handle_gadget(result & WMHI_GADGETMASK, code);
				break;
			case WMHI_MENUPICK:
				handle_menu((UWORD)(result & WMHI_MENUMASK));
				break;
			}
			if (quit_requested) break;
		}
	}
}

void ui_destroy(void)
{
	UWORD i;

	/* Let the visible page commit gadget-held edits while its gadgets
	 * still exist; disposing the window frees the whole tree. */
	if (window_obj && ui_pages[current_page]->hide)
		ui_pages[current_page]->hide();
	if (window_obj) {
		DisposeObject(window_obj);   /* closes the window, frees layout */
		window_obj = NULL;
		window = NULL;
		root_layout = NULL;
	} else if (root_layout) {
		DisposeObject(root_layout);  /* built, but no window adopted it */
		root_layout = NULL;
	}
	for (i = 0; i < PAGE_COUNT; i++)
		if (ui_pages[i]->cleanup) ui_pages[i]->cleanup();
	sidebar_free();
	timer_close();
	if (menu_strip) {
		FreeMenus(menu_strip);
		menu_strip = NULL;
	}
	if (visual_info) {
		FreeVisualInfo(visual_info);
		visual_info = NULL;
	}
	if (screen) {
		UnlockPubScreen(NULL, screen);
		screen = NULL;
	}
}
