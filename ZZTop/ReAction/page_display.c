/*
 * ZZTop Display page: output, scanlines and the everyday picture controls.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <exec/types.h>
#include <intuition/classusr.h>
#include <gadgets/button.h>
#include <gadgets/chooser.h>
#include <gadgets/layout.h>
#include <images/bevel.h>
#include <images/label.h>
#include <reaction/reaction_macros.h>
#include <proto/button.h>
#include <proto/chooser.h>
#include <proto/intuition.h>
#include <proto/label.h>
#include <proto/layout.h>
#include <proto/space.h>
#include <stdio.h>

#include "board.h"
#include "settings.h"
#include "ui.h"
#include "video.h"

#define P PAGE_DISPLAY

enum {
	G_OUTPUT = 1, G_REFRESH, G_SCANLINES, G_PARITY, G_ALIGN, G_FIND,
	G_AUTO, G_ENV_USE, G_ENV_REMOVE
};

static const char *const output_names[] = {
	"1280x1024 - full detail", "1920x1080 - centered",
	"800x600 / 720x480 - filtered", "720x576 / 720x480 - filtered", NULL
};
static const char *const refresh_names[] = {
	"50 Hz", "60 Hz", "Match Amiga", "50 Hz PAL, 60 Hz NTSC",
	"PAL Amiga clock", "NTSC Amiga clock", NULL
};
static const char *const scan_names[] = { "Off", "Classic", "Soft", "Gradient", NULL };
static const char *const parity_names[] = { "Odd lines", "Even lines", NULL };

static Object *output_obj, *refresh_obj, *scan_obj, *parity_obj;
static Object *align_obj, *find_obj, *auto_obj, *env_use_obj, *env_remove_obj;
static struct List *output_list, *refresh_list, *scan_list, *parity_list;
static UWORD output_map[ZZCFG_VCAP_OUTPUT_COUNT], refresh_map[ZZCFG_VCAP_REFRESH_COUNT];
static const char *output_labels[ZZCFG_VCAP_OUTPUT_COUNT + 1];
static const char *refresh_labels[ZZCFG_VCAP_REFRESH_COUNT + 1];
static UWORD output_count, refresh_count;
static struct ui_text description, refresh_note, env_note, boot_note, framing;
static struct ui_text sampling, picture_note;

/* Return the first supported refresh setting for one output mode. */
static UWORD first_refresh(UWORD output)
{
	UWORD refresh;

	for (refresh = 0; refresh < ZZCFG_VCAP_REFRESH_COUNT; refresh++) {
		if (zzcfg_profile_for_output_refresh(output, refresh, board.fw_caps) !=
			ZZCFG_VCAP_PROFILE_COUNT)
			return refresh;
	}
	return ZZCFG_VCAP_REFRESH_COUNT;
}

/* Find a model value in a chooser's compact index map. */
static WORD map_index(const UWORD *map, UWORD count, UWORD value)
{
	UWORD i;

	for (i = 0; i < count; i++) {
		if (map[i] == value)
			return (WORD)i;
	}
	return -1;
}

/* Build the output chooser from firmware-supported output/refresh pairs. */
static void build_outputs(void)
{
	UWORD output;

	output_count = 0;
	for (output = 0; output < ZZCFG_VCAP_OUTPUT_COUNT; output++) {
		if (first_refresh(output) != ZZCFG_VCAP_REFRESH_COUNT) {
			output_map[output_count] = output;
			output_labels[output_count++] = output_names[output];
		}
	}
	output_labels[output_count] = NULL;
}

/* Build the refresh chooser for the currently selected output. */
static void build_refreshes(UWORD output)
{
	UWORD refresh;

	refresh_count = 0;
	for (refresh = 0; refresh < ZZCFG_VCAP_REFRESH_COUNT; refresh++) {
		if (zzcfg_profile_for_output_refresh(output, refresh, board.fw_caps) !=
			ZZCFG_VCAP_PROFILE_COUNT) {
			refresh_map[refresh_count] = refresh;
			refresh_labels[refresh_count++] = refresh_names[refresh];
		}
	}
	refresh_labels[refresh_count] = NULL;
}

/* Describe the output and refresh halves of a selected profile separately. */
static void pair_description(char *a, UWORD asize, char *b, UWORD bsize,
	UWORD output, UWORD refresh)
{
	static const char *const output_text[] = {
		"1280x1024 full SuperHires detail",
		"1920x1080 with centered native picture",
		"800x600 PAL / 720x480 NTSC, filtered",
		"720x576 PAL / 720x480 NTSC, filtered"
	};
	static const char *const rate_text[] = {
		"50 Hz output", "60 Hz output", "follows the Amiga input",
		"50 Hz PAL / 60 Hz NTSC", "PAL Amiga clock", "NTSC Amiga clock"
	};

	snprintf(a, asize, "%s", output < ZZCFG_VCAP_OUTPUT_COUNT ?
		output_text[output] : "Unknown output");
	snprintf(b, bsize, "%s (not a live measurement)",
		refresh < ZZCFG_VCAP_REFRESH_COUNT ? rate_text[refresh] : "Unknown refresh");
}

/* Rebuild dependent choices and show both profile description lines. */
static void sync_choices(void)
{
	UWORD profile = cfg.edit.videocap_profile;
	UWORD output = zzcfg_profile_output(profile);
	UWORD refresh = zzcfg_profile_refresh(profile);
	WORD oi, ri;
	char a[UI_TEXT_MAX], b[UI_TEXT_MAX];

	build_outputs();
	if (!output_count)
		return;
	oi = map_index(output_map, output_count, output);
	if (oi < 0) {
		output = output_map[0];
		oi = 0;
	}
	build_refreshes(output);
	{
		struct List *new_list = ui_chooser_labels(refresh_labels);

		if (new_list) {
			struct List *old_list = refresh_list;

			refresh_list = new_list;
			ui_set(P, refresh_obj, CHOOSER_Labels, refresh_list, TAG_END);
			ui_chooser_free_labels(old_list);
		}
	}
	ri = map_index(refresh_map, refresh_count, refresh);
	if (ri < 0) {
		refresh = refresh_map[0];
		ri = 0;
	}
	pair_description(a, sizeof(a), b, sizeof(b), output, refresh);
	ui_set(P, output_obj, CHOOSER_Selected, (ULONG)oi, TAG_END);
	ui_set(P, refresh_obj, CHOOSER_Selected, (ULONG)ri, TAG_END);
	ui_text_set(P, &description, a);
	ui_text_set(P, &refresh_note, b);
}

/* Refresh live framing, phase and action availability readouts. */
static void refresh_picture(void)
{
	struct zz_vcap_snapshot snap;
	int phase;
	char note[UI_TEXT_MAX];

	if (settings_present(&cfg.edit, CFG_CROP_H) ||
		settings_present(&cfg.edit, CFG_CROP_V)) {
		ui_text_printf(P, &framing, "Custom %u x %u (effective %u x %u)",
			(unsigned)cfg.edit.videocap_crop_h,
			(unsigned)cfg.edit.videocap_crop_v,
			video_snapshot(&snap) ? (unsigned)snap.effective_h : 0,
			video_snapshot(&snap) ? (unsigned)snap.effective_v : 0);
	} else {
		ui_text_set(P, &framing, "Automatic");
	}
	if (video_phase_state(&phase, note, sizeof(note))) {
		ui_text_printf(P, &sampling, "%s, phase %d",
			video_phase_domain() == ZZ_VCAP_PHASE_C28 ? "C28 steps" : "E7M steps",
			phase);
	} else {
		ui_text_set(P, &sampling, "Automatic");
	}
	if (!video_align_ready(note, sizeof(note))) {
		ui_disable(P, align_obj, TRUE);
		ui_disable(P, auto_obj, TRUE);
	} else {
		ui_disable(P, align_obj, FALSE);
		ui_disable(P, auto_obj, !video_geometry_supported());
		if (!video_geometry_supported()) {
			snprintf(note, sizeof(note),
				"Reset to automatic needs firmware that confirms capture window changes.");
		}
	}
	ui_disable(P, find_obj, !video_find_phase_ready(note, sizeof(note)));
	ui_text_set(P, &picture_note, note);
}

/* Create the page's fixed controls and compact page groups. */
static Object *create(void)
{
	build_outputs();
	output_list = ui_chooser_labels(output_labels);
	build_refreshes(output_count ? output_map[0] : 0);
	refresh_list = ui_chooser_labels(refresh_labels);
	scan_list = ui_chooser_labels(scan_names);
	parity_list = ui_chooser_labels(parity_names);
	output_obj = ChooserObject, GA_ID, UI_GID(P, G_OUTPUT), GA_RelVerify, TRUE,
		CHOOSER_Labels, output_list, End;
	refresh_obj = ChooserObject, GA_ID, UI_GID(P, G_REFRESH), GA_RelVerify, TRUE,
		CHOOSER_Labels, refresh_list, End;
	scan_obj = ChooserObject, GA_ID, UI_GID(P, G_SCANLINES), GA_RelVerify, TRUE,
		CHOOSER_Labels, scan_list, End;
	parity_obj = ChooserObject, GA_ID, UI_GID(P, G_PARITY), GA_RelVerify, TRUE,
		CHOOSER_Labels, parity_list, End;
	align_obj = NewObject(BUTTON_GetClass(), NULL, GA_ID, UI_GID(P, G_ALIGN),
		GA_RelVerify, TRUE, GA_Text, "_Align picture...", End;
	find_obj = NewObject(BUTTON_GetClass(), NULL, GA_ID, UI_GID(P, G_FIND),
		GA_RelVerify, TRUE, GA_Text, "_Find best phase...", End;
	auto_obj = NewObject(BUTTON_GetClass(), NULL, GA_ID, UI_GID(P, G_AUTO),
		GA_RelVerify, TRUE, GA_Text, "_Reset to automatic", End;
	env_use_obj = NewObject(BUTTON_GetClass(), NULL, GA_ID, UI_GID(P, G_ENV_USE),
		GA_RelVerify, TRUE, GA_Text, "_Use it", End;
	env_remove_obj = NewObject(BUTTON_GetClass(), NULL,
		GA_ID, UI_GID(P, G_ENV_REMOVE), GA_RelVerify, TRUE,
		GA_Text, "_Remove", End;
	return VGroupObject, LAYOUT_SpaceOuter, TRUE,
		LAYOUT_AddChild, VGroupObject, LAYOUT_BevelStyle, BVS_GROUP,
			LAYOUT_Label, "Output",
			LAYOUT_AddChild, output_obj, Label("Output"),
			LAYOUT_AddChild, refresh_obj, Label("Refresh"),
			LAYOUT_AddChild, ui_note_new(&description, ""),
			LAYOUT_AddChild, ui_note_new(&refresh_note, ""),
			LAYOUT_AddChild, ui_note_new(&boot_note,
				"Takes effect at the next power-cycle."),
			LAYOUT_AddChild, HGroupObject,
				LAYOUT_AddChild, ui_note_new(&env_note, ""),
				LAYOUT_AddChild, env_use_obj, CHILD_WeightedWidth, 0,
				LAYOUT_AddChild, env_remove_obj, CHILD_WeightedWidth, 0,
			End,
		End,
		LAYOUT_AddChild, VGroupObject, LAYOUT_BevelStyle, BVS_GROUP,
			LAYOUT_Label, "Scanlines",
			LAYOUT_AddChild, scan_obj, Label("Style"),
			LAYOUT_AddChild, parity_obj, Label("Darken"),
		End,
		LAYOUT_AddChild, VGroupObject, LAYOUT_BevelStyle, BVS_GROUP,
			LAYOUT_Label, "Picture",
			LAYOUT_AddChild, ui_text_new(&framing, ""), Label("Framing"),
			LAYOUT_AddChild, ui_text_new(&sampling, ""), Label("Sampling"),
			LAYOUT_AddChild, HGroupObject,
				LAYOUT_AddChild, align_obj,
				LAYOUT_AddChild, find_obj,
				LAYOUT_AddChild, auto_obj,
			End,
			LAYOUT_AddChild, ui_note_new(&picture_note, ""),
		End,
		LAYOUT_AddChild, SpaceObject, End,
	End;
}

/* Synchronize model values, ENV override state and live picture state. */
static void sync(void)
{
	char env[UI_TEXT_MAX];
	BOOL cfg_ok = cfg.supported;

	sync_choices();
	ui_disable(P, output_obj, !cfg_ok);
	ui_disable(P, refresh_obj, !cfg_ok);
	ui_disable(P, scan_obj, FALSE);
	ui_disable(P, parity_obj, FALSE);
	ui_set(P, scan_obj, CHOOSER_Selected, (ULONG)cfg.edit.scanline_mode, TAG_END);
	ui_set(P, parity_obj, CHOOSER_Selected, (ULONG)cfg.edit.scanline_parity, TAG_END);
	if (!cfg_ok) {
		ui_text_set(P, &refresh_note,
			"Output settings need firmware 2.3 or newer. Scanlines still work live.");
	}
	if (cfg.env & CFG_ENV_NATIVE) {
		snprintf(env, sizeof(env),
			"ENV override selects profile %u and wins over ZZ9000.CFG.",
			(unsigned)cfg.env_profile);
		ui_text_set(P, &env_note, env);
		ui_disable(P, env_use_obj, FALSE);
		ui_disable(P, env_remove_obj, FALSE);
	} else {
		ui_text_set(P, &env_note, "");
		ui_disable(P, env_use_obj, TRUE);
		ui_disable(P, env_remove_obj, TRUE);
	}
	refresh_picture();
}

/* Apply one user action, then refresh dependent controls and status. */
static void gadget(UWORD id, UWORD code)
{
	char msg[UI_TEXT_MAX];
	UWORD output, refresh, profile;

	switch (id) {
	case G_OUTPUT:
		if (code >= output_count)
			return;
		output = output_map[code];
		refresh = zzcfg_profile_refresh(cfg.edit.videocap_profile);
		if (zzcfg_profile_for_output_refresh(output, refresh, board.fw_caps) ==
			ZZCFG_VCAP_PROFILE_COUNT)
			refresh = first_refresh(output);
		profile = zzcfg_profile_for_output_refresh(output, refresh, board.fw_caps);
		if (video_set_profile(profile, msg, sizeof(msg)))
			ui_status("%s", msg);
		break;
	case G_REFRESH:
		output = zzcfg_profile_output(cfg.edit.videocap_profile);
		if (code >= refresh_count)
			return;
		profile = zzcfg_profile_for_output_refresh(output, refresh_map[code],
			board.fw_caps);
		if (video_set_profile(profile, msg, sizeof(msg)))
			ui_status("%s", msg);
		break;
	case G_SCANLINES:
		if (code < BOARD_SCANLINE_MODES) {
			board_set_scanline_mode(code);
			if (cfg.supported)
				settings_set(CFG_SCAN_MODE, code);
		}
		break;
	case G_PARITY:
		if (code < 2) {
			board_set_scanline_parity(code);
			if (cfg.supported)
				settings_set(CFG_SCAN_PARITY, code);
		}
		break;
	case G_ALIGN:
		if (video_align(msg, sizeof(msg)))
			ui_status("%s", msg);
		else
			ui_error("%s", msg);
		break;
	case G_FIND:
		if (video_find_phase(msg, sizeof(msg)))
			ui_status("%s", msg);
		else
			ui_error("%s", msg);
		break;
	case G_AUTO:
		if (video_reset_automatic(msg, sizeof(msg)))
			ui_status("%s", msg);
		else
			ui_error("%s", msg);
		break;
	case G_ENV_USE:
		settings_env_adopt(CFG_ENV_NATIVE);
		ui_status("ENV native-video setting copied into ZZ9000.CFG edits");
		break;
	case G_ENV_REMOVE:
		if (ui_ask("Remove|Cancel",
			"Remove the native-video ENV: and ENVARC: override?") == 1) {
			if (settings_env_remove(CFG_ENV_NATIVE))
				ui_status("Native-video ENV override removed");
			else
				ui_error("Could not remove the native-video ENV override");
		}
		break;
	}
	ui_pending_changed();
	sync();
}

/* Free chooser label lists after the gadget tree releases their references. */
static void cleanup(void)
{
	ui_chooser_free_labels(output_list);
	ui_chooser_free_labels(refresh_list);
	ui_chooser_free_labels(scan_list);
	ui_chooser_free_labels(parity_list);
	output_list = NULL;
	refresh_list = NULL;
	scan_list = NULL;
	parity_list = NULL;
}

/* Poll the live picture state at two hertz while the page is visible. */
static void tick(void)
{
	if (ui_ticks() % 5 == 0)
		refresh_picture();
}

struct ui_page page_display = {
	"Display", "Native video", "Scandoubler output, scanlines and picture alignment.", 0,
	create, NULL, NULL, gadget, tick, sync,
	NULL,
	cleanup,
};
