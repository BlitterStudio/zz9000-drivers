/*
 * ZZTop GadTools Modules page: firmware optional-module status.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <exec/lists.h>
#include <exec/nodes.h>
#include <exec/types.h>
#include <libraries/gadtools.h>
#include <proto/exec.h>
#include <proto/gadtools.h>
#include <stdio.h>
#include <string.h>

#include "board.h"
#include "modules.h"
#include "gt.h"

#define P GT_PAGE_MODULES

#define MODULE_LIST_ROWS 6

enum {
	G_STATUS = 1,
	G_LIST,
	G_RESTART_NOTE,
	G_REFRESH
};

struct module_label {
	struct Node node;
	char text[96];
};

static struct List module_labels;
static struct module_label module_entries[MODULE_STATUS_MAX + 1];
static struct Gadget *g_status;
static struct Gadget *g_list;
static struct Gadget *g_restart_note;
static struct Gadget *g_refresh;
static char status_text[96] = "Refresh reads the module manifest.";

/* Initialise the Exec list that owns the GadTools label nodes. */
static void module_list_init(struct List *list)
{
	list->lh_Head = (struct Node *)&list->lh_Tail;
	list->lh_Tail = NULL;
	list->lh_TailPred = (struct Node *)&list->lh_Head;
}

/* Rebuild the status list after querying the fixed firmware manifest. */
static void refresh_modules(void)
{
	const struct module_list *list;
	UWORD count = 0;
	UWORD i;

	if (g_list)
		GT_SetGadgetAttrs(g_list, gt_render_window(g_list), NULL,
			GTLV_Labels, ~0UL, TAG_END);
	module_list_init(&module_labels);
	if ((board.fw_caps & ZZ_FW_CAP_FIRMWARE_MODULES) == 0) {
		snprintf(status_text, sizeof(status_text), "%s",
			"Module status needs firmware support.");
		snprintf(module_entries[0].text, sizeof(module_entries[0].text), "%s",
			"Firmware does not report optional modules.");
		count = 1;
	} else if (!modules_refresh()) {
		snprintf(status_text, sizeof(status_text), "%s", modules_status_text());
		snprintf(module_entries[0].text, sizeof(module_entries[0].text), "%s",
			"Module status is unavailable.");
		count = 1;
	} else {
		list = modules_current();
		snprintf(status_text, sizeof(status_text), "%s",
			"Name                 Version  State");
		if (list->count == 0) {
			snprintf(module_entries[0].text, sizeof(module_entries[0].text), "%s",
				"No optional modules in the firmware manifest.");
			count = 1;
		} else {
			count = list->count;
			for (i = 0; i < count; i++) {
				const struct module_status *entry = &list->entries[i];

				snprintf(module_entries[i].text, sizeof(module_entries[i].text),
					"%-20s v%lu  %s", entry->name,
					(unsigned long)entry->version,
					modules_state_text(entry->state));
			}
		}
	}
	for (i = 0; i < count; i++) {
		module_entries[i].node.ln_Name = module_entries[i].text;
		AddTail(&module_labels, &module_entries[i].node);
	}
	gt_page_set_text(g_status, status_text);
	gt_page_set_disabled(g_refresh,
		(board.fw_caps & ZZ_FW_CAP_FIRMWARE_MODULES) == 0);
	if (g_list)
		GT_SetGadgetAttrs(g_list, gt_render_window(g_list), NULL,
			GTLV_Labels, (ULONG)&module_labels, TAG_END);
}

/* Create the compact, scrolling status page so all manifest entries fit. */
static BOOL create(struct gt_page *page)
{
	struct NewGadget ng;
	WORD left = gt_layout.page_left;
	WORD width = gt_layout.page_width;
	WORD refresh_width;
	struct TagItem list_tags[] = {
		{ GTLV_Labels, 0 },
		{ GTLV_ShowSelected, 0 },
		{ TAG_DONE, 0 }
	};

	gt_page_begin(page);
	module_list_init(&module_labels);
	memset(module_entries, 0, sizeof(module_entries));
	g_status = gt_add_note(page, G_STATUS, left, 0, width, status_text);

	memset(&ng, 0, sizeof(ng));
	ng.ng_LeftEdge = left;
	ng.ng_TopEdge = gt_row(1);
	ng.ng_Width = width;
	ng.ng_Height = (WORD)(gt_row(MODULE_LIST_ROWS) + gt_layout.row_height -
		gt_row(1));
	ng.ng_GadgetID = GT_GID(P, G_LIST);
	list_tags[0].ti_Data = (ULONG)&module_labels;
	g_list = gt_page_add(page, LISTVIEW_KIND, &ng, list_tags);

	g_restart_note = gt_add_note(page, G_RESTART_NOTE, left, 7, width,
		"Changes to ZZ9K/ take effect after a reboot.");
	refresh_width = gt_button_width("Refresh");
	g_refresh = gt_add_button(page, G_REFRESH, left, 8, refresh_width, "Refresh");
	page->rows = 9;
	return g_status && g_list && g_restart_note && g_refresh;
}

/* Query module status whenever this page becomes visible. */
static void show(void)
{
	refresh_modules();
}

/* Refresh is the only page-local action. */
static void gadget(UWORD id, UWORD code)
{
	(void)code;
	if (id == G_REFRESH)
		refresh_modules();
}

/* Clear gadget references after GadTools disposes the page list. */
static void cleanup(void)
{
	g_status = NULL;
	g_list = NULL;
	g_restart_note = NULL;
	g_refresh = NULL;
}

struct gt_page gt_page_modules = {
	"Modules",
	"Firmware modules",
	"Optional module status reported by the firmware.",
	GT_PAGE_CHILD,
	NULL,
	NULL,
	9,
	create,
	show,
	NULL,
	gadget,
	NULL,
	NULL,
	NULL,
	cleanup
};
