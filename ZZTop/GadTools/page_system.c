/*
 * ZZTop GadTools System page: boot-time settings and ENV overrides.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <exec/types.h>
#include <intuition/intuition.h>
#include <libraries/gadtools.h>
#include <proto/gadtools.h>
#include <stdio.h>
#include <string.h>

#include "board.h"
#include "settings.h"
#include "gt.h"

#define P GT_PAGE_SYSTEM

enum {
	G_SUPPORTED = 1,
	G_INT,
	G_INT_HELP,
	G_INT_ENV,
	G_INT_USE_ENV,
	G_INT_REMOVE_ENV,
	G_MAC_BUILTIN,
	G_MAC,
	G_MAC_ENV,
	G_MAC_USE_ENV,
	G_MAC_REMOVE_ENV,
	G_HDF_ENABLE,
	G_HDF,
	G_OFFSCREEN,
	G_OVERLAY,
	G_FAST_RAM,
	G_FAST_RAM_STATUS,
	G_FAST_RAM_NOTE
};

static const char *const int_labels[] = {
	"INT6 (default)",
	"INT2",
	NULL
};

static struct Gadget *g_supported;
static struct Gadget *g_int;
static struct Gadget *g_int_help;
static struct Gadget *g_int_env;
static struct Gadget *g_int_use;
static struct Gadget *g_int_remove;
static struct Gadget *g_mac_builtin;
static struct Gadget *g_mac;
static struct Gadget *g_mac_env;
static struct Gadget *g_mac_use;
static struct Gadget *g_mac_remove;
static struct Gadget *g_hdf_enable;
static struct Gadget *g_hdf;
static struct Gadget *g_offscreen;
static struct Gadget *g_overlay;
static struct Gadget *g_fast_ram;
static struct Gadget *g_fast_ram_status;
static struct Gadget *g_fast_ram_note;

static char mac_text[ZZCFG_MAC_CHARS + 3];
static char hdf_text[ZZCFG_HDF_CHARS + 5];
static char supported_text[96];
static char int_help_text[96];
static char int_env_text[96];
static char mac_env_text[96];
static char fast_ram_status_text[64];
static char fast_ram_note_text[96];
static BOOL mac_custom;

/* Commit the current MAC controls into the editable settings model. */
static void commit_mac(void)
{
	const char *text;

	if (!cfg.supported)
		return;
	text = (const char *)gt_page_get(g_mac, GTST_String);
	if (gt_page_get(g_mac_builtin, GTCB_Checked))
		settings_clear(CFG_MAC);
	else if (text && text[0])
		settings_set_text(CFG_MAC, text);
	else
		settings_reset(CFG_MAC);
	gt_pending_changed();
}

/* Commit the current SD boot controls into the editable settings model. */
static void commit_hdf(void)
{
	static char stored[ZZCFG_HDF_CHARS + 5];
	const char *text;

	if (!cfg.supported)
		return;
	text = (const char *)gt_page_get(g_hdf, GTST_String);
	if (!text || !text[0])
		text = ZZCFG_HDF_DEFAULT;
	if (!gt_page_get(g_hdf_enable, GTCB_Checked)) {
		settings_set_text(CFG_HDF, ZZCFG_HDF_OFF);
	} else {
		snprintf(stored, sizeof(stored), "%s", cfg.saved.hdf);
		zzcfg_hdf_from_gadget(stored, sizeof(stored), text);
		if (stored[0])
			settings_set_text(CFG_HDF, stored);
		else
			settings_clear(CFG_HDF);
	}
	gt_pending_changed();
}

/* Synchronise system controls and explanatory notes from current settings. */
static void sync_controls(void)
{
	BOOL enabled = cfg.supported;
	BOOL builtin;
	const char *hdf = zzcfg_hdf_gadget_text(cfg.edit.hdf);
	BOOL hdf_enabled = hdf[0] != '\0';
	BOOL fast_available = cfg.fast_ram_state != ZZCFG_FAST_RAM_UNAVAILABLE;

	if (cfg.edit.mac[0])
		mac_custom = FALSE;
	builtin = !mac_custom && cfg.edit.mac[0] == '\0';

	snprintf(mac_text, sizeof(mac_text), "%s", cfg.edit.mac);
	snprintf(hdf_text, sizeof(hdf_text), "%s", hdf);

	GT_SetGadgetAttrs(g_int, gt_render_window(g_int), NULL, GTCY_Active,
		cfg.edit.int2 ? 1 : 0, TAG_END);
	GT_SetGadgetAttrs(g_mac_builtin, gt_render_window(g_mac_builtin), NULL,
		GTCB_Checked, builtin, TAG_END);
	GT_SetGadgetAttrs(g_mac, gt_render_window(g_mac), NULL, GTST_String, (ULONG)mac_text, TAG_END);
	GT_SetGadgetAttrs(g_hdf_enable, gt_render_window(g_hdf_enable), NULL,
		GTCB_Checked, hdf_enabled, TAG_END);
	GT_SetGadgetAttrs(g_hdf, gt_render_window(g_hdf), NULL, GTST_String, (ULONG)hdf_text, TAG_END);
	GT_SetGadgetAttrs(g_offscreen, gt_render_window(g_offscreen), NULL,
		GTCB_Checked, cfg.edit.offscreen_bitmaps, TAG_END);
	GT_SetGadgetAttrs(g_overlay, gt_render_window(g_overlay), NULL,
		GTCB_Checked, cfg.edit.video_overlay, TAG_END);
	GT_SetGadgetAttrs(g_fast_ram, gt_render_window(g_fast_ram), NULL,
		GTCB_Checked, cfg.edit.fast_ram, TAG_END);

	gt_page_set_disabled(g_int, !enabled);
	gt_page_set_disabled(g_int_use, !enabled || !(cfg.env & CFG_ENV_INT2));
	gt_page_set_disabled(g_int_remove, !enabled || !(cfg.env & CFG_ENV_INT2));
	gt_page_set_disabled(g_mac_builtin, !enabled);
	gt_page_set_disabled(g_mac, !enabled || builtin);
	gt_page_set_disabled(g_mac_use, !enabled || !(cfg.env & CFG_ENV_MAC));
	gt_page_set_disabled(g_mac_remove, !enabled || !(cfg.env & CFG_ENV_MAC));
	gt_page_set_disabled(g_hdf_enable, !enabled);
	gt_page_set_disabled(g_hdf, !enabled || !hdf_enabled);
	gt_page_set_disabled(g_offscreen, !enabled);
	gt_page_set_disabled(g_overlay, !enabled);
	gt_page_set_disabled(g_fast_ram, !enabled || !fast_available);

	snprintf(supported_text, sizeof(supported_text), "%s",
		settings_system_note());
	gt_page_set_text(g_supported, supported_text);

	snprintf(int_help_text, sizeof(int_help_text), "%s",
		"Use INT2 only if INT6 locks; set jumper J4.");
	gt_page_set_text(g_int_help, int_help_text);

	if (cfg.env & CFG_ENV_INT2)
		snprintf(int_env_text, sizeof(int_env_text), "%s", "ENV:ZZ9K_INT2 overrides this.");
	else
		int_env_text[0] = '\0';
	gt_page_set_text(g_int_env, int_env_text);

	if (cfg.env & CFG_ENV_MAC) {
		snprintf(mac_env_text, sizeof(mac_env_text), "ENV:ZZ9K_MAC is %s", cfg.env_mac);
	} else if (!builtin && mac_text[0] && !settings_mac_valid(mac_text)) {
		snprintf(mac_env_text, sizeof(mac_env_text), "%s", "Use aa:bb:cc:dd:ee:ff format.");
	} else {
		mac_env_text[0] = '\0';
	}
	gt_page_set_text(g_mac_env, mac_env_text);

	settings_fast_ram_status(fast_ram_status_text,
		sizeof(fast_ram_status_text), fast_ram_note_text,
		sizeof(fast_ram_note_text));
	gt_page_set_text(g_fast_ram_status, fast_ram_status_text);
	gt_page_set_text(g_fast_ram_note, fast_ram_note_text);
}

/* Create the fixed system settings page and its dynamic readouts. */
static BOOL create(struct gt_page *page)
{
	struct gt_form f;
	static const char *const sys_labels[] = { "Interrupt", NULL };
	WORD left, width, half;
	WORD use_w, rem_w, env_note_w;
	WORD mac_chk_w, hdf_chk_w, fast_chk_w;
	struct TagItem mac_tags[] = {
		{ GTST_String, (ULONG)mac_text },
		{ GTST_MaxChars, ZZCFG_MAC_CHARS + 1 },
		{ GA_RelVerify, TRUE },
		{ TAG_DONE, 0 }
	};
	struct TagItem hdf_tags[] = {
		{ GTST_String, (ULONG)hdf_text },
		{ GTST_MaxChars, ZZCFG_HDF_CHARS + 1 },
		{ GA_RelVerify, TRUE },
		{ TAG_DONE, 0 }
	};
	struct TagItem stat_tags[] = {
		{ GTTX_Text, (ULONG)fast_ram_status_text },
		{ GTTX_CopyText, TRUE },
		{ GTTX_Border, TRUE },
		{ GTTX_Clipped, TRUE },
		{ TAG_DONE, 0 }
	};
	struct NewGadget ng;

	gt_page_begin(page);
	gt_form_init(&f, page, sys_labels);
	left = f.left;
	width = f.width;
	half = (WORD)((width - 8) / 2);

	/* Row 0: Note */
	supported_text[0] = '\0';
	g_supported = gt_add_note(page, G_SUPPORTED, left, 0, width, supported_text);

	/* Row 1: Interrupt */
	g_int = gt_form_cycle(&f, G_INT, 1, "Interrupt", int_labels);

	/* Row 2: INT Help note */
	int_help_text[0] = '\0';
	g_int_help = gt_add_note(page, G_INT_HELP, left, 2, width, int_help_text);

	/* Row 3: INT ENV note + Use it / Remove buttons */
	use_w = gt_button_width("Use it");
	rem_w = gt_button_width("Remove");
	env_note_w = (WORD)(width - use_w - rem_w - 8);
	int_env_text[0] = '\0';
	g_int_env = gt_add_note(page, G_INT_ENV, left, 3, env_note_w, int_env_text);
	g_int_use = gt_add_button(page, G_INT_USE_ENV, (WORD)(left + env_note_w + 4), 3, use_w, "Use it");
	g_int_remove = gt_add_button(page, G_INT_REMOVE_ENV,
		(WORD)(left + env_note_w + use_w + 8), 3, rem_w, "Remove");

	/* Row 4: Network (Card built-in MAC checkbox + MAC string field) */
	mac_chk_w = gt_checkbox_total_width("Card built-in MAC");
	g_mac_builtin = gt_add_checkbox(page, G_MAC_BUILTIN, left, 4, "Card built-in MAC", TRUE);
	memset(&ng, 0, sizeof(ng));
	ng.ng_LeftEdge = (WORD)(left + mac_chk_w + 4);
	ng.ng_TopEdge = gt_row(4);
	ng.ng_Width = (WORD)(width - mac_chk_w - 4);
	ng.ng_Height = gt_layout.row_height;
	ng.ng_GadgetID = GT_GID(5, G_MAC);
	g_mac = gt_page_add(page, STRING_KIND, &ng, mac_tags);

	/* Row 5: MAC ENV note + Use it / Remove buttons */
	mac_env_text[0] = '\0';
	g_mac_env = gt_add_note(page, G_MAC_ENV, left, 5, env_note_w, mac_env_text);
	g_mac_use = gt_add_button(page, G_MAC_USE_ENV, (WORD)(left + env_note_w + 4), 5, use_w, "Use it");
	g_mac_remove = gt_add_button(page, G_MAC_REMOVE_ENV,
		(WORD)(left + env_note_w + use_w + 8), 5, rem_w, "Remove");

	/* Row 6: SD card boot (Boot from SD image checkbox + HDF string field) */
	hdf_chk_w = gt_checkbox_total_width("Boot from SD image");
	g_hdf_enable = gt_add_checkbox(page, G_HDF_ENABLE, left, 6, "Boot from SD image", TRUE);
	ng.ng_LeftEdge = (WORD)(left + hdf_chk_w + 4);
	ng.ng_TopEdge = gt_row(6);
	ng.ng_Width = (WORD)(width - hdf_chk_w - 4);
	ng.ng_Height = gt_layout.row_height;
	ng.ng_GadgetID = GT_GID(5, G_HDF);
	g_hdf = gt_page_add(page, STRING_KIND, &ng, hdf_tags);

	/* Row 7: Graphics options (Off-screen bitmaps & Video overlay) */
	g_offscreen = gt_add_checkbox(page, G_OFFSCREEN, left, 7, "Off-screen card RAM", TRUE);
	g_overlay = gt_add_checkbox(page, G_OVERLAY, (WORD)(left + half + 8), 7,
		"Video overlay (ZZPlay)", TRUE);

	/* Row 8: Memory (Z3 Fast RAM checkbox + status field) */
	fast_chk_w = gt_checkbox_total_width("Z3 Fast RAM");
	g_fast_ram = gt_add_checkbox(page, G_FAST_RAM, left, 8, "Z3 Fast RAM", TRUE);
	fast_ram_status_text[0] = '\0';
	ng.ng_LeftEdge = (WORD)(left + fast_chk_w + 4);
	ng.ng_TopEdge = gt_row(8);
	ng.ng_Width = (WORD)(width - fast_chk_w - 4);
	ng.ng_Height = gt_layout.row_height;
	ng.ng_GadgetID = GT_GID(5, G_FAST_RAM_STATUS);
	g_fast_ram_status = gt_page_add(page, TEXT_KIND, &ng, stat_tags);

	/* Row 9: Fast RAM reason note */
	fast_ram_note_text[0] = '\0';
	g_fast_ram_note = gt_add_note(page, G_FAST_RAM_NOTE, left, 9, width, fast_ram_note_text);

	page->rows = 10;
	return g_supported && g_int && g_int_help && g_int_env && g_int_use &&
		g_int_remove && g_mac_builtin && g_mac && g_mac_env && g_mac_use &&
		g_mac_remove && g_hdf_enable && g_hdf && g_offscreen && g_overlay &&
		g_fast_ram && g_fast_ram_status && g_fast_ram_note;
}

/* Synchronise controls whenever the system page becomes visible. */
static void show(void)
{
	sync_controls();
}

/* Commit typed system fields before a state-changing page transition. */
static void flush(void)
{
	commit_mac();
	commit_hdf();
}

/* Apply one system-page gadget action to the settings model. */
static void gadget(UWORD id, UWORD code)
{
	switch (id) {
	case G_INT:
		if (code < 2)
			settings_set(CFG_INT2, code == 1 ? 1 : 0);
		break;
	case G_INT_USE_ENV:
		settings_env_adopt(CFG_ENV_INT2);
		gt_status("ENV:ZZ9K_INT2 copied into ZZ9000.CFG");
		break;
	case G_INT_REMOVE_ENV:
		if (gt_ask("Remove|Cancel", "Remove the ENV: and ENVARC: ZZ9K_INT2 overrides?") == 1) {
			if (settings_env_remove(CFG_ENV_INT2))
				gt_status("ENV:ZZ9K_INT2 removed");
			else
				gt_status("Could not remove ENV:ZZ9K_INT2");
		}
		break;
	case G_MAC_BUILTIN:
		mac_custom = code == 0;
		if (!mac_custom)
			mac_text[0] = '\0';
		commit_mac();
		break;
	case G_MAC:
		commit_mac();
		break;
	case G_MAC_USE_ENV:
		settings_env_adopt(CFG_ENV_MAC);
		gt_status("ENV:ZZ9K_MAC copied into ZZ9000.CFG");
		break;
	case G_MAC_REMOVE_ENV:
		if (gt_ask("Remove|Cancel", "Remove the ENV: and ENVARC: ZZ9K_MAC overrides?") == 1) {
			if (settings_env_remove(CFG_ENV_MAC))
				gt_status("ENV:ZZ9K_MAC removed");
			else
				gt_status("Could not remove ENV:ZZ9K_MAC");
		}
		break;
	case G_HDF_ENABLE:
	case G_HDF:
		commit_hdf();
		break;
	case G_OFFSCREEN:
		settings_set(CFG_OFFSCREEN, code ? 1 : 0);
		break;
	case G_OVERLAY:
		settings_set(CFG_OVERLAY, code ? 1 : 0);
		break;
	case G_FAST_RAM:
		settings_set(CFG_FAST_RAM, code ? 1 : 0);
		break;
	}
	gt_pending_changed();
	sync_controls();
}

/* Commit edited text that has not yet generated a gadget event. */
static void tick(void)
{
	/* Commit any string edit when the user switches away without Enter. */
	commit_mac();
	commit_hdf();
}

/* Synchronise system controls after a shared model transition. */
static void sync(void)
{
	sync_controls();
}

struct gt_page gt_page_system = {
	"System",
	"System settings",
	"Interrupt, network, SD boot, RTG and memory settings.",
	0,
	NULL,
	NULL,
	10,
	create,
	show,
	NULL,
	gadget,
	tick,
	sync,
	flush,
	NULL
};
