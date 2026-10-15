/*
 * ZZTop GadTools front-end framework.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <clib/alib_protos.h>
#include <exec/types.h>
#include <exec/io.h>
#include <devices/timer.h>
#include <dos/dos.h>
#include <intuition/intuition.h>
#include <intuition/intuitionbase.h>
#include <libraries/asl.h>
#include <proto/asl.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/gadtools.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/utility.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "zztop.h"
#include "gt.h"

extern struct IntuitionBase *IntuitionBase;
extern struct Library *AslBase;

#define GID_LIST 1
#define GID_SAVE 2
#define GID_REVERT 3
#define MENU_ABOUT 1
#define MENU_QUIT 2
#define MENU_SAVE 3
#define MENU_REVERT 4
#define MENU_ADVANCED 5
#define RAWKEY_ESCAPE 0x45

struct gt_layout gt_layout;
static struct Screen *screen;
static APTR visual_info;
static struct Window *window;
static struct Menu *menu_strip;
static struct Gadget *frame_gadgets;
static struct Gadget *frame_tail;
static struct Gadget *list_gadget;
static struct Gadget *status_gadget;
static struct Gadget *save_gadget;
static struct Gadget *revert_gadget;
static struct MsgPort *timer_port;
static struct timerequest *timer_io;
static BOOL timer_pending;
static ULONG tick_count;
static UWORD current_page;
static BOOL advanced_mode;
static BOOL quit_requested;
static UWORD busy_depth;
static struct Requester busy_requester;
static char window_title[64];
static char status_text[128] = "Ready.";
static char list_names[GT_PAGE_COUNT][32];
static struct List list_labels;
static struct Node list_nodes[GT_PAGE_COUNT];
static UWORD list_map[GT_PAGE_COUNT];
static UWORD list_count;
static UBYTE advanced_env[] = "ZZTopAdvanced";
static UBYTE advanced_yes[] = "1";
static UBYTE advanced_no[] = "0";
static BOOL minimal_layout;
static WORD sidebar_width;
static char create_failure[160];

static struct NewMenu menus[] = {
	{ NM_TITLE, (STRPTR)"Project", NULL, 0, 0, NULL },
	{ NM_ITEM, (STRPTR)"About...", (STRPTR)"?", 0, 0, (APTR)MENU_ABOUT },
	{ NM_ITEM, NM_BARLABEL, NULL, 0, 0, NULL },
	{ NM_ITEM, (STRPTR)"Quit", (STRPTR)"Q", 0, 0, (APTR)MENU_QUIT },
	{ NM_TITLE, (STRPTR)"Edit", NULL, 0, 0, NULL },
	{ NM_ITEM, (STRPTR)"Save", (STRPTR)"S", 0, 0, (APTR)MENU_SAVE },
	{ NM_ITEM, (STRPTR)"Revert", (STRPTR)"R", 0, 0, (APTR)MENU_REVERT },
	{ NM_TITLE, (STRPTR)"Settings", NULL, 0, 0, NULL },
	{ NM_ITEM, (STRPTR)"Advanced controls", (STRPTR)"A", CHECKIT | MENUTOGGLE,
		0, (APTR)MENU_ADVANCED },
	{ NM_END, NULL, NULL, 0, 0, NULL }
};

/* Return the larger of two signed dimensions. */
static WORD max_word(WORD a, WORD b)
{
	return a > b ? a : b;
}

/* Record the specific startup failure reported by the entry point. */
static void set_failure(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(create_failure, sizeof(create_failure), fmt, ap);
	va_end(ap);
}

/* Return the screen height available to the complete window. */
static WORD usable_height(struct Screen *s)
{
	return (WORD)(s->Height - s->BarHeight - 1);
}

/* Measure the widest page-list name in the actual screen font. */
static WORD measure_sidebar(void)
{
	UWORD page;
	WORD widest = 0;
	WORD indent;

	indent = TextLength(&screen->RastPort, (CONST_STRPTR)"   ", 3);
	for (page = 0; page < GT_PAGE_COUNT; page++) {
		WORD width = TextLength(&screen->RastPort,
			(CONST_STRPTR)gt_pages[page]->name, strlen(gt_pages[page]->name));

		if (gt_pages[page]->flags & GT_PAGE_CHILD)
			width += indent;
		if (width > widest)
			widest = width;
	}
	return (WORD)(widest + max_word(24, gt_layout.font_x * 3));
}

/* Populate normal, compact, or title-only minimal vertical metrics. */
static void vertical_metrics(UWORD tier)
{
	minimal_layout = tier == 2;
	gt_layout.compact = tier != 0;
	if (minimal_layout) {
		gt_layout.row_height = (WORD)(gt_layout.font_y + 4);
		gt_layout.row_step = (WORD)(gt_layout.row_height + 1);
		gt_layout.section_gap = 0;
		return;
	}
	gt_layout.row_height = max_word(14, gt_layout.font_y + 6);
	gt_layout.row_step = (WORD)(gt_layout.row_height + (tier == 1 ?
		max_word(2, gt_layout.font_y / 4) : max_word(5, gt_layout.font_y / 2)));
	gt_layout.section_gap = tier == 1 ? max_word(2, gt_layout.font_y / 4) :
		max_word(5, gt_layout.font_y / 2);
}

/* Measure the fixed shell against content rather than the full public screen. */
static void measure_layout(UWORD tier)
{
	WORD screen_inner;
	WORD wanted_width;
	WORD status_bottom;

	vertical_metrics(tier);
	screen_inner = (WORD)(screen->Width - screen->WBorLeft - screen->WBorRight);
	sidebar_width = measure_sidebar();
	wanted_width = max_word(570, (WORD)(sidebar_width + 450));
	gt_layout.border_left = screen->WBorLeft;
	gt_layout.border_top = (WORD)(screen->WBorTop + gt_layout.font_y + 1);
	gt_layout.page_top = gt_layout.border_top;
	gt_layout.width = wanted_width < screen_inner ? wanted_width : screen_inner;
	gt_layout.page_left = (WORD)(gt_layout.border_left + sidebar_width + 8);
	gt_layout.page_width = (WORD)(gt_layout.width - sidebar_width - 12);
	gt_layout.body_top = (WORD)(gt_layout.border_top +
		(minimal_layout ? 0 : gt_layout.row_step * 2));
	gt_layout.body_bottom = (WORD)(gt_layout.body_top + gt_layout.row_step * 10);
	status_bottom = (WORD)(gt_layout.body_bottom + gt_layout.section_gap + gt_layout.row_height);
	gt_layout.height = (WORD)(status_bottom + (minimal_layout ? 2 : 4) - gt_layout.border_top);
	gt_layout.page_height = (WORD)(gt_layout.body_bottom - gt_layout.border_top);
}

/* Include the window title and both window borders in a physical height. */
static WORD outer_height(void)
{
	return (WORD)(screen->WBorTop + gt_layout.font_y + 1 + gt_layout.height +
		screen->WBorBottom);
}

/* Retry normal, compact, then minimal layout until the physical window fits. */
static BOOL layout_fits(void)
{
	measure_layout(0);
	if (outer_height() > usable_height(screen))
		measure_layout(1);
	if (outer_height() > usable_height(screen))
		measure_layout(2);
	if (outer_height() <= usable_height(screen))
		return TRUE;
	if (outer_height() <= screen->Height)
		return TRUE;
	return FALSE;
}

/* Return the window-relative top coordinate of a page row. */
WORD gt_row(UWORD row)
{
	return (WORD)(gt_layout.body_top + row * gt_layout.row_step);
}
/* Measure a string with the active screen's font. */
WORD gt_text_width(const char *text)
{
	if (!text || !text[0] || !screen)
		return 0;
	return (WORD)TextLength(&screen->RastPort, (CONST_STRPTR)text, strlen(text));
}

/* Measure the widest string in a NULL-terminated array. */
WORD gt_max_text_width(const char *const *texts)
{
	WORD max_w = 0;

	if (!texts)
		return 0;
	while (*texts) {
		WORD w = gt_text_width(*texts);
		if (w > max_w)
			max_w = w;
		texts++;
	}
	return max_w;
}

/* Compute a button width that comfortably fits its caption on any font. */
WORD gt_button_width(const char *text)
{
	return (WORD)(gt_text_width(text) + max_word(16, gt_layout.font_x * 2));
}

/* Compute the total horizontal footprint of a checkbox and its label. */
WORD gt_checkbox_total_width(const char *label)
{
	WORD box_w = max_word(26, gt_layout.font_y + 4);

	return (WORD)(box_w + gt_text_width(label) + max_word(8, gt_layout.font_x));
}
/* Return GadTools visual information for page gadget creation. */
APTR gt_visual_info(void)
{
	return visual_info;
}

/* Find a page's stable registry index. */
static UWORD get_page_index(struct gt_page *page)
{
	UWORD i;

	for (i = 0; i < GT_PAGE_COUNT; i++)
		if (gt_pages[i] == page)
			return i;
	return 0;
}

/* Initialise a form context with a measured label column. */
void gt_form_init(struct gt_form *form, struct gt_page *page,
	const char *const *labels)
{
	WORD max_label = gt_max_text_width(labels);

	form->page = page;
	form->left = gt_layout.page_left;
	form->width = gt_layout.page_width;
	form->label_col = max_label > 0 ? (WORD)(max_label + max_word(8, gt_layout.font_x)) : 0;
	form->ctrl_left = (WORD)(form->left + form->label_col);
	form->ctrl_width = (WORD)(form->width - form->label_col);
}

/* Add a framed text readout gadget in the form's control column. */
struct Gadget *gt_form_field(struct gt_form *f, UWORD id, UWORD row,
	const char *label, char *text)
{
	struct NewGadget ng;
	struct TagItem tags[] = {
		{ GTTX_Text, (ULONG)text },
		{ GTTX_CopyText, TRUE },
		{ GTTX_Border, TRUE },
		{ GTTX_Clipped, TRUE },
		{ TAG_DONE, 0 }
	};

	memset(&ng, 0, sizeof(ng));
	ng.ng_LeftEdge = f->ctrl_left;
	ng.ng_TopEdge = gt_row(row);
	ng.ng_Width = f->ctrl_width;
	ng.ng_Height = gt_layout.row_height;
	ng.ng_GadgetText = (STRPTR)label;
	ng.ng_GadgetID = GT_GID(get_page_index(f->page), id);
	ng.ng_Flags = PLACETEXT_LEFT;
	return gt_page_add(f->page, TEXT_KIND, &ng, tags);
}

/* Add a cycle gadget in the form's control column. */
struct Gadget *gt_form_cycle(struct gt_form *f, UWORD id, UWORD row,
	const char *label, const char *const *choices)
{
	struct NewGadget ng;
	struct TagItem tags[] = {
		{ GTCY_Labels, (ULONG)choices },
		{ GTCY_Active, 0 },
		{ GA_RelVerify, TRUE },
		{ TAG_DONE, 0 }
	};

	memset(&ng, 0, sizeof(ng));
	ng.ng_LeftEdge = f->ctrl_left;
	ng.ng_TopEdge = gt_row(row);
	ng.ng_Width = f->ctrl_width;
	ng.ng_Height = gt_layout.row_height;
	ng.ng_GadgetText = (STRPTR)label;
	ng.ng_GadgetID = GT_GID(get_page_index(f->page), id);
	ng.ng_Flags = PLACETEXT_LEFT;
	return gt_page_add(f->page, CYCLE_KIND, &ng, tags);
}

/* Add a horizontal slider gadget with a measured value column. */
struct Gadget *gt_form_slider(struct gt_form *f, UWORD id, UWORD row,
	const char *label, LONG min, LONG max, const char *format,
	const char *sample_max)
{
	struct NewGadget ng;
	WORD val_w = (WORD)(gt_text_width(sample_max) + max_word(8, gt_layout.font_x));
	WORD sl_w = (WORD)(f->ctrl_width - val_w);
	struct TagItem tags[] = {
		{ GTSL_Min, min },
		{ GTSL_Max, max },
		{ GTSL_Level, min },
		{ GTSL_LevelFormat, (ULONG)format },
		{ GTSL_MaxLevelLen, strlen(sample_max) + 1 },
		{ GTSL_LevelPlace, PLACETEXT_RIGHT },
		{ GA_RelVerify, TRUE },
		{ TAG_DONE, 0 }
	};

	memset(&ng, 0, sizeof(ng));
	ng.ng_LeftEdge = f->ctrl_left;
	ng.ng_TopEdge = gt_row(row);
	ng.ng_Width = sl_w > 40 ? sl_w : 40;
	ng.ng_Height = gt_layout.row_height;
	ng.ng_GadgetText = (STRPTR)label;
	ng.ng_GadgetID = GT_GID(get_page_index(f->page), id);
	ng.ng_Flags = PLACETEXT_LEFT;
	return gt_page_add(f->page, SLIDER_KIND, &ng, tags);
}

/* Add a string gadget in the form's control column. */
struct Gadget *gt_form_string(struct gt_form *f, UWORD id, UWORD row,
	const char *label, char *text, UWORD max_chars)
{
	struct NewGadget ng;
	struct TagItem tags[] = {
		{ GTST_String, (ULONG)text },
		{ GTST_MaxChars, max_chars },
		{ GA_RelVerify, TRUE },
		{ TAG_DONE, 0 }
	};

	memset(&ng, 0, sizeof(ng));
	ng.ng_LeftEdge = f->ctrl_left;
	ng.ng_TopEdge = gt_row(row);
	ng.ng_Width = f->ctrl_width;
	ng.ng_Height = gt_layout.row_height;
	ng.ng_GadgetText = (STRPTR)label;
	ng.ng_GadgetID = GT_GID(get_page_index(f->page), id);
	ng.ng_Flags = PLACETEXT_LEFT;
	return gt_page_add(f->page, STRING_KIND, &ng, tags);
}

/* Add an integer gadget in the form's control column. */
struct Gadget *gt_form_integer(struct gt_form *f, UWORD id, UWORD row,
	const char *label, LONG number, UWORD max_chars)
{
	struct NewGadget ng;
	struct TagItem tags[] = {
		{ GTIN_Number, number },
		{ GTIN_MaxChars, max_chars },
		{ GA_RelVerify, TRUE },
		{ TAG_DONE, 0 }
	};

	memset(&ng, 0, sizeof(ng));
	ng.ng_LeftEdge = f->ctrl_left;
	ng.ng_TopEdge = gt_row(row);
	ng.ng_Width = f->ctrl_width;
	ng.ng_Height = gt_layout.row_height;
	ng.ng_GadgetText = (STRPTR)label;
	ng.ng_GadgetID = GT_GID(get_page_index(f->page), id);
	ng.ng_Flags = PLACETEXT_LEFT;
	return gt_page_add(f->page, INTEGER_KIND, &ng, tags);
}

/* Add a button with its caption placed inside. */
struct Gadget *gt_add_button(struct gt_page *page, UWORD id, WORD left,
	UWORD row, WORD width, const char *text)
{
	struct NewGadget ng;
	struct TagItem tags[] = {
		{ GA_RelVerify, TRUE },
		{ TAG_DONE, 0 }
	};

	memset(&ng, 0, sizeof(ng));
	ng.ng_LeftEdge = left;
	ng.ng_TopEdge = gt_row(row);
	ng.ng_Width = width;
	ng.ng_Height = gt_layout.row_height;
	ng.ng_GadgetText = (STRPTR)text;
	ng.ng_GadgetID = GT_GID(get_page_index(page), id);
	ng.ng_Flags = PLACETEXT_IN;
	return gt_page_add(page, BUTTON_KIND, &ng, tags);
}

/* Add a borderless text note across any available width. */
struct Gadget *gt_add_note(struct gt_page *page, UWORD id, WORD left,
	UWORD row, WORD width, char *text)
{
	struct NewGadget ng;
	struct TagItem tags[] = {
		{ GTTX_Text, (ULONG)text },
		{ GTTX_CopyText, TRUE },
		{ GTTX_Border, FALSE },
		{ GTTX_Clipped, TRUE },
		{ TAG_DONE, 0 }
	};

	memset(&ng, 0, sizeof(ng));
	ng.ng_LeftEdge = left;
	ng.ng_TopEdge = gt_row(row);
	ng.ng_Width = width;
	ng.ng_Height = gt_layout.row_height;
	ng.ng_GadgetText = NULL;
	ng.ng_GadgetID = GT_GID(get_page_index(page), id);
	ng.ng_Flags = PLACETEXT_LEFT;
	return gt_page_add(page, TEXT_KIND, &ng, tags);
}

/* Add a checkbox with its label placed to its right. */
struct Gadget *gt_add_checkbox(struct gt_page *page, UWORD id, WORD left,
	UWORD row, const char *label, BOOL checked)
{
	struct NewGadget ng;
	struct TagItem tags[] = {
		{ GTCB_Checked, checked },
		{ GA_RelVerify, TRUE },
		{ TAG_DONE, 0 }
	};

	memset(&ng, 0, sizeof(ng));
	ng.ng_LeftEdge = left;
	ng.ng_TopEdge = gt_row(row);
	ng.ng_Width = max_word(26, gt_layout.font_y + 4);
	ng.ng_Height = gt_layout.row_height;
	ng.ng_GadgetText = (STRPTR)label;
	ng.ng_GadgetID = GT_GID(get_page_index(page), id);
	ng.ng_Flags = PLACETEXT_RIGHT;
	return gt_page_add(page, CHECKBOX_KIND, &ng, tags);
}


/* Start one page's independently removable gadget list. */
void gt_page_begin(struct gt_page *page)
{
	page->gadgets = NULL;
	page->tail = CreateContext(&page->gadgets);
}

/* Add one GadTools gadget to a page-owned list using a real tag array. */
struct Gadget *gt_page_add(struct gt_page *page, ULONG kind,
	struct NewGadget *ng, struct TagItem *tags)
{
	if (!ng->ng_TextAttr)
		ng->ng_TextAttr = gt_layout.font;
	if (!ng->ng_VisualInfo)
		ng->ng_VisualInfo = visual_info;
	page->tail = CreateGadgetA(kind, page->tail, ng, tags);
	return page->tail;
}

/* The window to render a gadget change into: only gadgets attached to it
 * (the frame and the visible page). A hidden page's gadgets get their new
 * state without drawing, and draw it when their page is shown. */
struct Window *gt_render_window(struct Gadget *gadget)
{
	struct Gadget *g;

	if (!window || !gadget)
		return NULL;
	for (g = window->FirstGadget; g; g = g->NextGadget)
		if (g == gadget)
			return window;
	return NULL;
}

/* Set a dynamic text gadget after copying its value into a static page buffer. */
void gt_page_set_text(struct Gadget *gadget, const char *text)
{
	if (!gadget)
		return;
	GT_SetGadgetAttrs(gadget, gt_render_window(gadget), NULL,
		GTTX_Text, (ULONG)text, TAG_END);
}

/* Enable or disable a page control. */
void gt_page_set_disabled(struct Gadget *gadget, BOOL disabled)
{
	if (!gadget)
		return;
	GT_SetGadgetAttrs(gadget, gt_render_window(gadget), NULL,
		GA_Disabled, disabled, TAG_END);
}

/* Read a GadTools attribute without exposing toolkit state to pages. */
ULONG gt_page_get(struct Gadget *gadget, ULONG tag)
{
	ULONG value = 0;

	if (gadget)
		GT_GetGadgetAttrs(gadget, gt_render_window(gadget), NULL, tag,
			(ULONG)&value, TAG_END);
	return value;
}

/* Draw text at a page position, cut to the page width so it never runs
 * over the window border (the copy is written to fit; this is a guard). */
static void draw_page_text(struct RastPort *rp, WORD top, const char *text)
{
	struct TextExtent extent;
	ULONG count;

	count = TextFit(rp, (CONST_STRPTR)text, strlen(text), &extent, NULL, 1,
		gt_layout.page_width, 32767);
	Move(rp, gt_layout.page_left, (WORD)(top + rp->TxBaseline));
	Text(rp, (CONST_STRPTR)text, count);
}

/* Draw headings and the current page's manual decorations. */
void gt_draw_page(void)
{
	struct RastPort *rp;

	if (!window || minimal_layout)
		return;
	rp = window->RPort;
	SetAPen(rp, 1);
	SetSoftStyle(rp, FSF_BOLD, FSF_BOLD);
	draw_page_text(rp, gt_layout.page_top, gt_pages[current_page]->title);
	SetSoftStyle(rp, FS_NORMAL, FSF_BOLD);
	draw_page_text(rp, (WORD)(gt_layout.page_top + gt_layout.row_step),
		gt_pages[current_page]->summary);
}

/* Build the visible sidebar entries (GadTools wants an Exec list of
 * Nodes) and their page-index mapping. The list must be detached from
 * the gadget while it is rebuilt. */
static void build_page_list(void)
{
	UWORD page;

	NewList(&list_labels);
	list_count = 0;
	for (page = 0; page < GT_PAGE_COUNT; page++) {
		const char *name = gt_pages[page]->name;

		if (!advanced_mode && (gt_pages[page]->flags & GT_PAGE_ADVANCED))
			continue;
		if (gt_pages[page]->flags & GT_PAGE_CHILD)
			snprintf(list_names[list_count], sizeof(list_names[list_count]), "   %s", name);
		else
			snprintf(list_names[list_count], sizeof(list_names[list_count]), "%s", name);
		memset(&list_nodes[list_count], 0, sizeof(list_nodes[list_count]));
		list_nodes[list_count].ln_Name = (char *)list_names[list_count];
		AddTail(&list_labels, &list_nodes[list_count]);
		list_map[list_count] = page;
		list_count++;
	}
}

/* Find a listed page's visual list position. */
static UWORD page_list_index(UWORD page)
{
	UWORD i;

	for (i = 0; i < list_count; i++)
		if (list_map[i] == page)
			return i;
	return 0;
}

/* Replace the list's labels after advanced mode changes. */
static void refresh_page_list(void)
{
	if (!list_gadget)
		return;
	GT_SetGadgetAttrs(list_gadget, window, NULL, GTLV_Labels, ~0UL, TAG_END);
	build_page_list();
	GT_SetGadgetAttrs(list_gadget, window, NULL, GTLV_Labels, (ULONG)&list_labels,
		GTLV_Selected, page_list_index(current_page), TAG_END);
}

/* Report a one-line status message. */
void gt_status(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(status_text, sizeof(status_text), fmt, ap);
	va_end(ap);
	gt_page_set_text(status_gadget, status_text);
}

/* Wrap a requester body at an OS-default-font-safe character width. */
static void wrap_text(char *out, UWORD size, const char *in)
{
	UWORD used = 0;
	UWORD column = 0;

	while (*in && used + 1 < size) {
		if (*in == '\n') {
			out[used++] = *in++;
			column = 0;
			continue;
		}
		if (*in == ' ' && column >= 50) {
			out[used++] = '\n';
			in++;
			column = 0;
			continue;
		}
		out[used++] = *in++;
		column++;
	}
	out[used] = '\0';
}

/* Show an EasyRequest requester with a safely wrapped body. */
static LONG ask_va(const char *gadgets, const char *fmt, va_list ap)
{
	static char raw[512];
	static char body[560];
	struct EasyStruct es;
	ULONG args[1];

	vsnprintf(raw, sizeof(raw), fmt, ap);
	wrap_text(body, sizeof(body), raw);
	es.es_StructSize = sizeof(es);
	es.es_Flags = 0;
	es.es_Title = (STRPTR)ZZTOP_NAME;
	es.es_TextFormat = (STRPTR)"%s";
	es.es_GadgetFormat = (STRPTR)gadgets;
	args[0] = (ULONG)body;
	return EasyRequestArgs(window, &es, NULL, args);
}

/* Public requester helper for pages. */
LONG gt_ask(const char *gadgets, const char *fmt, ...)
{
	va_list ap;
	LONG result;

	va_start(ap, fmt);
	result = ask_va(gadgets, fmt, ap);
	va_end(ap);
	return result;
}

/* Open the standard ASL file requester for firmware images. */
BOOL gt_pick_file(const char *title, const char *pattern, char *path,
	UWORD path_size)
{
	struct FileRequester *requester;
	BOOL ok = FALSE;

	if (!AslBase) {
		gt_ask("OK", "asl.library is needed for the file requester.");
		return FALSE;
	}
	requester = AllocAslRequestTags(ASL_FileRequest, ASLFR_TitleText, (ULONG)title,
		ASLFR_DoPatterns, pattern ? TRUE : FALSE,
		ASLFR_InitialPattern, (ULONG)(pattern ? pattern : "#?"),
		ASLFR_Window, (ULONG)window, ASLFR_SleepWindow, TRUE, TAG_END);
	if (!requester)
		return FALSE;
	if (AslRequestTags(requester, TAG_END)) {
		strncpy(path, (const char *)requester->fr_Drawer, path_size - 1);
		path[path_size - 1] = '\0';
		if (AddPart((STRPTR)path, requester->fr_File, path_size))
			ok = TRUE;
		else
			gt_ask("OK", "That file path is too long.");
	}
	FreeAslRequest(requester);
	return ok;
}

/* Nest an input-blocking requester and a V39 busy pointer. */
void gt_busy(BOOL busy)
{
	if (busy) {
		if (busy_depth++ == 0 && window) {
			InitRequester(&busy_requester);
			Request(&busy_requester, window);
			if (IntuitionBase->LibNode.lib_Version >= 39)
				SetWindowPointer(window, WA_BusyPointer, TRUE, TAG_END);
		}
	} else if (busy_depth && --busy_depth == 0 && window) {
		EndRequest(&busy_requester, window);
		if (IntuitionBase->LibNode.lib_Version >= 39)
			SetWindowPointer(window, WA_Pointer, 0, TAG_END);
	}
}

/* Recompute pending buttons and title from the shared model. */
void gt_pending_changed(void)
{
	struct app_pending pending;
	char base[64];
	BOOL any;

	app_pending(&pending);
	any = pending.card || pending.live || pending.audio;
	gt_page_set_disabled(save_gadget, !(pending.card || pending.audio));
	gt_page_set_disabled(revert_gadget, !any);
	if (minimal_layout)
		snprintf(base, sizeof(base), "%s: %s", ZZTOP_NAME,
			gt_pages[current_page]->title);
	else
		snprintf(base, sizeof(base), "%s", ZZTOP_NAME);
	if (pending.card && pending.reboot)
		snprintf(window_title, sizeof(window_title), "%s - %u unsaved, reboot needed",
			base, (unsigned)pending.card);
	else if (pending.card)
		snprintf(window_title, sizeof(window_title), "%s - %u unsaved", base,
			(unsigned)pending.card);
	else if (pending.audio)
		snprintf(window_title, sizeof(window_title), "%s - audio unsaved", base);
	else if (pending.live)
		snprintf(window_title, sizeof(window_title), "%s - live changes", base);
	else
		snprintf(window_title, sizeof(window_title), "%s", base);
	if (window)
		SetWindowTitles(window, (CONST_STRPTR)window_title, (CONST_STRPTR)~0UL);
}

/* Commit gadget-held text on the visible page. */
void gt_flush(void)
{
	if (gt_pages[current_page]->flush)
		gt_pages[current_page]->flush();
}

/* Refresh every page after a model transition. */
void gt_models_changed(void)
{
	UWORD i;

	for (i = 0; i < GT_PAGE_COUNT; i++)
		if (gt_pages[i]->sync)
			gt_pages[i]->sync();
	gt_pending_changed();
}

/* Return current advanced setting. */
BOOL gt_advanced(void)
{
	return advanced_mode;
}

/* Persist and apply Advanced controls visibility. */
void gt_set_advanced(BOOL on)
{
	struct MenuItem *item;

	on = on ? TRUE : FALSE;
	if (advanced_mode == on)
		return;
	advanced_mode = on;
	SetVar((STRPTR)advanced_env, on ? (STRPTR)advanced_yes : (STRPTR)advanced_no,
		1, GVF_GLOBAL_ONLY | GVF_SAVE_VAR);
	if (!advanced_mode && (gt_pages[current_page]->flags & GT_PAGE_ADVANCED))
		gt_show_page(current_page ? (UWORD)(current_page - 1) : 0);
	item = ItemAddress(menu_strip, FULLMENUNUM(2, 0, NOSUB));
	if (item) {
		if (window)
			ClearMenuStrip(window);
		if (advanced_mode)
			item->Flags |= CHECKED;
		else
			item->Flags &= ~CHECKED;
		if (window)
			ResetMenuStrip(window, menu_strip);
	}
	refresh_page_list();
}

/* Swap old and new independent page lists, preserving the frame controls. */
void gt_show_page(UWORD page)
{
	struct gt_page *old_page;
	struct gt_page *new_page;

	if (page >= GT_PAGE_COUNT)
		return;
	if (!advanced_mode && (gt_pages[page]->flags & GT_PAGE_ADVANCED))
		gt_set_advanced(TRUE);
	if (page == current_page && window) {
		gt_draw_page();
		return;
	}
	old_page = gt_pages[current_page];
	new_page = gt_pages[page];
	if (old_page->hide)
		old_page->hide();
	if (window && old_page->gadgets)
		RemoveGList(window, old_page->gadgets, -1);
	if (window) {
		WORD erase_l = (WORD)(gt_layout.border_left + sidebar_width + 4);
		WORD erase_r = (WORD)(gt_layout.border_left + gt_layout.width - 2);
		WORD erase_t = gt_layout.border_top;
		WORD erase_b = (WORD)(gt_layout.body_bottom + gt_layout.section_gap);
		EraseRect(window->RPort, erase_l, erase_t, erase_r, erase_b);
	}
	current_page = page;
	if (window && new_page->gadgets) {
		AddGList(window, new_page->gadgets, -1, -1, NULL);
		RefreshGList(new_page->gadgets, window, NULL, -1);
		GT_RefreshWindow(window, NULL);
	}
	refresh_page_list();
	gt_draw_page();
	if (new_page->show)
		new_page->show();
	if (minimal_layout)
		gt_status("%s", new_page->summary);
	gt_pending_changed();
}

/* Return current page id. */
UWORD gt_current_page(void)
{
	return current_page;
}

/* Return 10 Hz tick count. */
ULONG gt_ticks(void)
{
	return tick_count;
}

/* Return the active screen. */
struct Screen *gt_screen(void)
{
	return screen;
}

/* Return the active window. */
struct Window *gt_window(void)
{
	return window;
}

/* Start a timer.device request. */
static BOOL timer_open(void)
{
	timer_port = CreateMsgPort();
	if (!timer_port) {
		set_failure("Could not create the timer.device message port.");
		return FALSE;
	}
	timer_io = (struct timerequest *)CreateIORequest(timer_port, sizeof(*timer_io));
	if (!timer_io) {
		set_failure("Could not create the timer.device I/O request.");
		return FALSE;
	}
	if (OpenDevice((STRPTR)TIMERNAME, UNIT_VBLANK, (struct IORequest *)timer_io, 0)) {
		set_failure("Could not open timer.device VBLANK unit.");
		return FALSE;
	}
	return TRUE;
}

/* Arm the next 100 ms callback. */
static void timer_arm(void)
{
	timer_io->tr_node.io_Command = TR_ADDREQUEST;
	timer_io->tr_time.tv_secs = 0;
	timer_io->tr_time.tv_micro = 100000;
	SendIO((struct IORequest *)timer_io);
	timer_pending = TRUE;
}

/* Dispose timer resources in the reverse of allocation order. */
static void timer_close(void)
{
	if (timer_io) {
		if (timer_pending) {
			AbortIO((struct IORequest *)timer_io);
			WaitIO((struct IORequest *)timer_io);
		}
		CloseDevice((struct IORequest *)timer_io);
		DeleteIORequest((struct IORequest *)timer_io);
		timer_io = NULL;
	}
	if (timer_port) {
		DeleteMsgPort(timer_port);
		timer_port = NULL;
	}
	timer_pending = FALSE;
}

/* Create the persistent sidebar, status bar and Save/Revert controls. */
static BOOL create_frame(void)
{
	struct NewGadget ng;
	/* GTLV_ShowSelected takes a string gadget pointer; NULL (0) means
	 * "highlight the selected entry in place". TRUE would be a bogus
	 * pointer and makes CreateGadget fail. */
	struct TagItem list_tags[] = {
		{ GTLV_Labels, 0 },
		{ GTLV_Selected, 0 },
		{ GTLV_ShowSelected, 0 },
		{ TAG_END, 0 }
	};
	struct TagItem text_tags[] = {
		{ GTTX_Text, 0 },
		{ GTTX_Border, TRUE },
		{ GTTX_Clipped, TRUE },
		{ GTTX_CopyText, TRUE },
		{ TAG_END, 0 }
	};
	struct TagItem button_tags[] = {
		{ TAG_END, 0 }
	};

	memset(&ng, 0, sizeof(ng));
	frame_tail = CreateContext(&frame_gadgets);
	if (!frame_tail) {
		set_failure("Could not create the frame gadget context.");
		return FALSE;
	}
	ng.ng_TextAttr = gt_layout.font;
	ng.ng_VisualInfo = visual_info;
	ng.ng_Flags = PLACETEXT_IN;
	ng.ng_LeftEdge = gt_layout.border_left + 4;
	ng.ng_TopEdge = gt_layout.page_top;
	ng.ng_Width = sidebar_width;
	ng.ng_Height = gt_layout.body_bottom - gt_layout.page_top;
	ng.ng_GadgetText = NULL;
	ng.ng_GadgetID = GID_LIST;
	list_tags[0].ti_Data = (ULONG)&list_labels;
	frame_tail = CreateGadgetA(LISTVIEW_KIND, frame_tail, &ng, list_tags);
	list_gadget = frame_tail;
	if (!list_gadget) {
		set_failure("Could not create the page-list gadget (width %ld, height %ld).",
			(long)sidebar_width, (long)(gt_layout.body_bottom - gt_layout.page_top));
		return FALSE;
	}
	WORD revert_w = gt_button_width("Revert");
	WORD save_w = gt_button_width("Save");
	WORD status_w = (WORD)(gt_layout.width - revert_w - save_w - 16);

	ng.ng_LeftEdge = gt_layout.border_left + 4;
	ng.ng_TopEdge = (WORD)(gt_layout.body_bottom + gt_layout.section_gap);
	ng.ng_Width = status_w;
	ng.ng_Height = gt_layout.row_height;
	ng.ng_GadgetID = 10;
	text_tags[0].ti_Data = (ULONG)status_text;
	frame_tail = CreateGadgetA(TEXT_KIND, frame_tail, &ng, text_tags);
	status_gadget = frame_tail;
	if (!status_gadget) {
		set_failure("Could not create the status gadget (width %ld).", (long)ng.ng_Width);
		return FALSE;
	}
	ng.ng_LeftEdge = (WORD)(gt_layout.border_left + 4 + status_w + 4);
	ng.ng_Width = revert_w;
	ng.ng_GadgetText = (STRPTR)"Revert";
	ng.ng_GadgetID = GID_REVERT;
	frame_tail = CreateGadgetA(BUTTON_KIND, frame_tail, &ng, button_tags);
	revert_gadget = frame_tail;
	if (!revert_gadget) {
		set_failure("Could not create the Revert button.");
		return FALSE;
	}
	ng.ng_LeftEdge = (WORD)(gt_layout.border_left + 4 + status_w + 4 + revert_w + 4);
	ng.ng_Width = save_w;
	ng.ng_GadgetText = (STRPTR)"Save";
	ng.ng_GadgetID = GID_SAVE;
	frame_tail = CreateGadgetA(BUTTON_KIND, frame_tail, &ng, button_tags);
	save_gadget = frame_tail;
	if (!save_gadget) {
		set_failure("Could not create the Save button.");
		return FALSE;
	}
	return TRUE;
}

/* Return the startup failure captured while creating the GadTools window. */
const char *gt_create_error(void)
{
	return create_failure[0] ? create_failure : "Unknown GadTools startup failure.";
}

/* Create all pages and open the one classic GadTools window. */
BOOL gt_create(void)
{
	UWORD i;
	char setting[4];
	struct NewWindow nw;
	struct TagItem tags[] = {
		{ WA_Title, 0 },
		{ WA_ScreenTitle, 0 },
		{ WA_InnerWidth, 0 },
		{ WA_InnerHeight, 0 },
		{ WA_DragBar, TRUE },
		{ WA_DepthGadget, TRUE },
		{ WA_CloseGadget, TRUE },
		{ WA_Activate, TRUE },
		{ WA_SmartRefresh, TRUE },
		{ WA_NewLookMenus, TRUE },
		{ WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_GADGETUP | IDCMP_MENUPICK |
			IDCMP_RAWKEY | IDCMP_MOUSEMOVE },
		{ TAG_END, 0 }
	};

	create_failure[0] = '\0';
	screen = LockPubScreen(NULL);
	if (!screen) {
		set_failure("Could not lock the Workbench public screen.");
		return FALSE;
	}
	gt_layout.font = screen->Font;
	gt_layout.font_x = screen->RastPort.TxWidth ? screen->RastPort.TxWidth : 8;
	gt_layout.font_y = screen->RastPort.TxHeight ? screen->RastPort.TxHeight : 8;
	if (!layout_fits()) {
		set_failure("Screen font is %u pixels; a 256-line screen needs a font of 13 pixels or less.",
			(unsigned)gt_layout.font_y);
		return FALSE;
	}
	visual_info = GetVisualInfo(screen, TAG_END);
	if (!visual_info) {
		set_failure("Could not obtain GadTools visual information.");
		return FALSE;
	}
	advanced_mode = GetVar((STRPTR)advanced_env, (STRPTR)setting, sizeof(setting),
		GVF_GLOBAL_ONLY) > 0 && setting[0] == '1';
	build_page_list();
	menu_strip = CreateMenus(menus, TAG_END);
	if (!menu_strip) {
		set_failure("Could not create the menu strip.");
		return FALSE;
	}
	if (!LayoutMenus(menu_strip, visual_info, GTMN_NewLookMenus, TRUE, TAG_END)) {
		set_failure("Could not lay out the menu strip.");
		return FALSE;
	}
	if (advanced_mode) {
		struct MenuItem *item = ItemAddress(menu_strip, FULLMENUNUM(2, 0, NOSUB));

		if (item)
			item->Flags |= CHECKED;
	}
	for (i = 0; i < GT_PAGE_COUNT; i++) {
		if (!gt_pages[i]->create || !gt_pages[i]->create(gt_pages[i])) {
			set_failure("Could not create the %s page gadgets.", gt_pages[i]->name);
			return FALSE;
		}
	}
	if (!create_frame())
		return FALSE;
	if (!timer_open())
		return FALSE;
	if (minimal_layout)
		snprintf(window_title, sizeof(window_title), "%s: %s", ZZTOP_NAME,
			gt_pages[current_page]->title);
	else
		snprintf(window_title, sizeof(window_title), "%s", ZZTOP_NAME);
	memset(&nw, 0, sizeof(nw));
	nw.LeftEdge = (WORD)((screen->Width - gt_layout.width) / 2);
	nw.TopEdge = (WORD)((usable_height(screen) - outer_height()) / 2 + screen->BarHeight + 1);
	nw.Width = gt_layout.width;
	nw.Height = gt_layout.height;
	nw.DetailPen = 0;
	nw.BlockPen = 1;
	nw.IDCMPFlags = IDCMP_CLOSEWINDOW | IDCMP_GADGETUP | IDCMP_MENUPICK | IDCMP_RAWKEY |
		IDCMP_MOUSEMOVE;
	nw.Flags = WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_CLOSEGADGET | WFLG_ACTIVATE |
		WFLG_SMART_REFRESH;
	nw.FirstGadget = frame_gadgets;
	nw.Title = (STRPTR)window_title;
	nw.Screen = screen;
	nw.MinWidth = gt_layout.width;
	nw.MinHeight = gt_layout.height;
	nw.MaxWidth = gt_layout.width;
	nw.MaxHeight = gt_layout.height;
	nw.Type = CUSTOMSCREEN;
	tags[0].ti_Data = (ULONG)window_title;
	tags[1].ti_Data = (ULONG)(ZZTOP_NAME " " ZZTOP_RELEASE " - ZZ9000 control panel");
	tags[2].ti_Data = gt_layout.width;
	tags[3].ti_Data = gt_layout.height;
	window = OpenWindowTagList(&nw, tags);
	if (!window) {
		set_failure("Could not open the ZZTop window (%ld x %ld inner).",
			(long)gt_layout.width, (long)gt_layout.height);
		return FALSE;
	}
	SetMenuStrip(window, menu_strip);
	AddGList(window, gt_pages[current_page]->gadgets, -1, -1, NULL);
	RefreshGList(frame_gadgets, window, NULL, -1);
	RefreshGList(gt_pages[current_page]->gadgets, window, NULL, -1);
	GT_RefreshWindow(window, NULL);
	for (i = 0; i < GT_PAGE_COUNT; i++)
		if (gt_pages[i]->sync)
			gt_pages[i]->sync();
	gt_draw_page();
	if (gt_pages[current_page]->show)
		gt_pages[current_page]->show();
	if (minimal_layout)
		gt_status("%s", gt_pages[current_page]->summary);
	gt_pending_changed();
	return TRUE;
}

/* Process a complete GadTools event while the source IMsg is valid. */
static void handle_message(struct IntuiMessage *message)
{
	struct Gadget *gadget = (struct Gadget *)message->IAddress;
	ULONG class = message->Class;
	UWORD code = message->Code;
	UWORD id = gadget ? gadget->GadgetID : 0;

	GT_ReplyIMsg(message);
	if (class == IDCMP_CLOSEWINDOW) {
		if (app_quit_allowed())
			quit_requested = TRUE;
		return;
	}
	if (class == IDCMP_RAWKEY && code == RAWKEY_ESCAPE) {
		if (app_quit_allowed())
			quit_requested = TRUE;
		return;
	}
	if (class == IDCMP_MENUPICK) {
		while (code != MENUNULL) {
			struct MenuItem *item = ItemAddress(menu_strip, code);
			ULONG action = item ? (ULONG)GTMENUITEM_USERDATA(item) : 0;

			if (action == MENU_ABOUT)
				gt_ask("OK", ZZTOP_NAME " " ZZTOP_RELEASE " (" ZZTOP_DATE ")\n"
					"The ZZ9000 control panel.\n\n"
					"Copyright 2026 Dimitris Panokostas\n"
					"Licensed under the GNU GPL v3 or later.");
			else if (action == MENU_QUIT) {
				if (app_quit_allowed())
					quit_requested = TRUE;
			} else if (action == MENU_SAVE)
				app_save();
			else if (action == MENU_REVERT)
				app_revert();
			else if (action == MENU_ADVANCED)
				gt_set_advanced((item->Flags & CHECKED) != 0);
			code = item ? item->NextSelect : MENUNULL;
		}
		return;
	}
	if (class == IDCMP_GADGETUP) {
		if (id == GID_LIST) {
			ULONG selected = gt_page_get(list_gadget, GTLV_Selected);
			if (selected < list_count)
				gt_show_page(list_map[selected]);
		} else if (id == GID_SAVE)
			app_save();
		else if (id == GID_REVERT)
			app_revert();
		else if (id >= 100 && GT_GID_PAGE(id) < GT_PAGE_COUNT &&
			gt_pages[GT_GID_PAGE(id)]->gadget)
			gt_pages[GT_GID_PAGE(id)]->gadget(GT_GID_LOCAL(id), code);
	}
}

/* Service 10 Hz ticks and drain GadTools events until quit. */
void gt_run(void)
{
	ULONG timer_signal;
	ULONG window_signal;

	timer_signal = 1UL << timer_port->mp_SigBit;
	window_signal = 1UL << window->UserPort->mp_SigBit;
	timer_arm();
	while (!quit_requested) {
		ULONG signals = Wait(timer_signal | window_signal | SIGBREAKF_CTRL_C);
		struct IntuiMessage *message;

		if (signals & SIGBREAKF_CTRL_C) {
			if (app_quit_allowed())
				break;
		}
		if ((signals & timer_signal) && CheckIO((struct IORequest *)timer_io)) {
			WaitIO((struct IORequest *)timer_io);
			timer_pending = FALSE;
			tick_count++;
			app_tick();
			if (gt_pages[current_page]->tick)
				gt_pages[current_page]->tick();
			timer_arm();
		}
		while ((message = GT_GetIMsg(window->UserPort)) != NULL)
			handle_message(message);
	}
}

/* Release page and frame gadgets plus all supporting resources. */
void gt_destroy(void)
{
	UWORD i;

	if (window && gt_pages[current_page]->hide)
		gt_pages[current_page]->hide();
	if (window) {
		/* AddGList appended the visible page's gadgets after the frame
		 * list, linking the frame's last gadget to them. Detach them
		 * first, or FreeGadgets(frame_gadgets) would also free the page
		 * and the page loop below would free it again (81000005). */
		if (gt_pages[current_page]->gadgets)
			RemoveGList(window, gt_pages[current_page]->gadgets, -1);
		ClearMenuStrip(window);
		CloseWindow(window);
		window = NULL;
	}
	if (frame_gadgets) {
		FreeGadgets(frame_gadgets);
		frame_gadgets = NULL;
	}
	for (i = 0; i < GT_PAGE_COUNT; i++) {
		if (gt_pages[i]->gadgets) {
			FreeGadgets(gt_pages[i]->gadgets);
			gt_pages[i]->gadgets = NULL;
		}
		if (gt_pages[i]->cleanup)
			gt_pages[i]->cleanup();
	}
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

/* Initialise wait deferral using raw Intuition messages, before GT filtering. */
void fe_wait_begin(struct fe_wait *wait, BOOL *cancel)
{
	wait->cancel = cancel;
	NewList(&wait->deferred);
}

/* Keep a hardware wait responsive without double-replying deferred input. */
int fe_wait_keep_running(void *ctx)
{
	struct fe_wait *wait = ctx;
	struct IntuiMessage *message;

	if (!window)
		return !*wait->cancel;
	while ((message = (struct IntuiMessage *)GetMsg(window->UserPort)) != NULL) {
		if (message->Class == IDCMP_CLOSEWINDOW ||
			(message->Class == IDCMP_RAWKEY && message->Code == RAWKEY_ESCAPE)) {
			*wait->cancel = TRUE;
			ReplyMsg((struct Message *)message);
		} else {
			AddTail(&wait->deferred, (struct Node *)message);
		}
	}
	return !*wait->cancel;
}

/* Requeue deferred raw messages before the normal GT_GetIMsg event path. */
void fe_wait_end(struct fe_wait *wait)
{
	struct Message *message;

	while ((message = (struct Message *)RemHead(&wait->deferred)) != NULL)
		PutMsg(window->UserPort, message);
}

/* Forward frontend services requested by the toolkit-neutral core. */
void fe_status(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(status_text, sizeof(status_text), fmt, ap);
	va_end(ap);
	gt_page_set_text(status_gadget, status_text);
}

/* Forward a formatted requester through the frontend interface. */
LONG fe_ask(const char *gadgets, const char *fmt, ...)
{
	va_list ap;
	LONG result;

	va_start(ap, fmt);
	result = ask_va(gadgets, fmt, ap);
	va_end(ap);
	return result;
}

/* Forward busy-state changes through the frontend interface. */
void fe_busy(BOOL busy)
{
	gt_busy(busy);
}

/* Flush pending visible-page edits through the frontend interface. */
void fe_flush(void)
{
	gt_flush();
}

/* Refresh page models through the frontend interface. */
void fe_models_changed(void)
{
	gt_models_changed();
}

/* Refresh pending-state controls through the frontend interface. */
void fe_pending_changed(void)
{
	gt_pending_changed();
}

/* Show the page that corresponds to a core frontend location. */
void fe_show(enum fe_place place)
{
	gt_show_page(place == FE_PLACE_CAPTURE ? GT_PAGE_CAPTURE : GT_PAGE_SYSTEM);
}

/* Return the frontend timer tick count. */
ULONG fe_ticks(void)
{
	return gt_ticks();
}

/* Return the frontend window for core drawing helpers. */
struct Window *fe_window(void)
{
	return gt_window();
}

/* Return the frontend screen for core drawing helpers. */
struct Screen *fe_screen(void)
{
	return gt_screen();
}
