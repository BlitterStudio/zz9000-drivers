/*
 * ZZTop GadTools Display page: output, scanlines, gamma and picture controls.
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
#include "video.h"
#include "gt.h"
#include "zz9000_gamma.h"

#define P GT_PAGE_DISPLAY

enum {
	G_OUTPUT = 1,
	G_REFRESH,
	G_DESCRIPTION,
	G_REFRESH_NOTE,
	G_ENV_NOTE,
	G_ENV_USE,
	G_ENV_REMOVE,
	G_SCANLINES,
	G_PARITY,
	G_FRAMING,
	G_SAMPLING,
	G_ALIGN,
	G_FIND,
	G_AUTO,
	G_PICTURE_NOTE,
	G_GAMMA,
	G_GAMMA_VALUE
};

static const char *const output_names[] = {
	"1280x1024 - full detail",
	"1920x1080 - centered",
	"800x600 / 720x480 - filtered",
	"720x576 / 720x480 - filtered",
	NULL
};
static const char *const refresh_names[] = {
	"50 Hz",
	"60 Hz",
	"Match Amiga",
	"50 Hz PAL, 60 Hz NTSC",
	"PAL Amiga clock",
	"NTSC Amiga clock",
	NULL
};
static const char *const scan_names[] = {
	"Off",
	"Classic",
	"Soft",
	"Gradient",
	NULL
};
static const char *const parity_names[] = {
	"Odd lines",
	"Even lines",
	NULL
};

static struct Gadget *output_gadget;
static struct Gadget *refresh_gadget;
static struct Gadget *scan_gadget;
static struct Gadget *parity_gadget;
static struct Gadget *align_gadget;
static struct Gadget *find_gadget;
static struct Gadget *auto_gadget;
static struct Gadget *env_use_gadget;
static struct Gadget *env_remove_gadget;
static struct Gadget *description_gadget;
static struct Gadget *refresh_note_gadget;
static struct Gadget *env_note_gadget;
static struct Gadget *framing_gadget;
static struct Gadget *sampling_gadget;
static struct Gadget *picture_note_gadget;
static struct Gadget *gamma_gadget;
static struct Gadget *gamma_value_gadget;

static UWORD output_map[ZZCFG_VCAP_OUTPUT_COUNT];
static UWORD refresh_map[ZZCFG_VCAP_REFRESH_COUNT];
static const char *output_choices[ZZCFG_VCAP_OUTPUT_COUNT + 1];
static const char *refresh_choices[ZZCFG_VCAP_REFRESH_COUNT + 1];
static UWORD output_count;
static UWORD refresh_count;

static char description_text[128];
static char refresh_note_text[128];
/* The output and refresh the selectors show. They differ from the stored
 * profile when this firmware cannot produce it; changes start from what
 * the user sees, not from the unsupported stored value. */
static UWORD shown_output;
static UWORD shown_refresh;
static BOOL stored_unsupported;
static char env_note_text[128];
static char framing_text[64];
static char sampling_text[64];
static char picture_note_text[128];
static char action_text[128];
/* Gamma readout ("1.80", "Off", or why the slider is disabled). */
static char gamma_text[16];
/* Slider level last applied or shown; -1 forces the next sync to set it. */
static WORD gamma_level = -1;
static BOOL gamma_available;

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

/* Find a model value in a compact cycle index map. */
static WORD map_index(const UWORD *map, UWORD count, UWORD value)
{
	UWORD index;

	for (index = 0; index < count; index++) {
		if (map[index] == value)
			return (WORD)index;
	}
	return -1;
}

/* Build the output cycle from firmware-supported output and refresh pairs. */
static void build_outputs(void)
{
	UWORD output;

	output_count = 0;
	for (output = 0; output < ZZCFG_VCAP_OUTPUT_COUNT; output++) {
		if (first_refresh(output) != ZZCFG_VCAP_REFRESH_COUNT) {
			output_map[output_count] = output;
			output_choices[output_count] = output_names[output];
			output_count++;
		}
	}
	output_choices[output_count] = NULL;
}

/* Build the refresh cycle for the selected output. */
static void build_refreshes(UWORD output)
{
	UWORD refresh;

	refresh_count = 0;
	for (refresh = 0; refresh < ZZCFG_VCAP_REFRESH_COUNT; refresh++) {
		if (zzcfg_profile_for_output_refresh(output, refresh, board.fw_caps) !=
			ZZCFG_VCAP_PROFILE_COUNT) {
			refresh_map[refresh_count] = refresh;
			refresh_choices[refresh_count] = refresh_names[refresh];
			refresh_count++;
		}
	}
	refresh_choices[refresh_count] = NULL;
}

/* Describe the selected output and refresh halves of a profile. */
static void pair_description(UWORD output, UWORD refresh)
{
	static const char *const output_text[] = {
		"1280x1024 SuperHires detail",
		"1920x1080 centered native",
		"800x600 PAL / 720x480 NTSC, filtered",
		"720x576 PAL / 720x480 NTSC, filtered"
	};
	static const char *const rate_text[] = {
		"50 Hz output",
		"60 Hz output",
		"Follows Amiga input",
		"50 Hz PAL / 60 Hz NTSC",
		"PAL Amiga clock",
		"NTSC Amiga clock"
	};
	const char *output_note = "Unknown output";
	const char *rate_note = "Unknown refresh";

	if (output < ZZCFG_VCAP_OUTPUT_COUNT)
		output_note = output_text[output];
	if (refresh < ZZCFG_VCAP_REFRESH_COUNT)
		rate_note = rate_text[refresh];
	snprintf(description_text, sizeof(description_text), "%s", output_note);
	snprintf(refresh_note_text, sizeof(refresh_note_text), "%s (unmeasured)", rate_note);
}

/* Rebuild dependent output controls and show their profile descriptions. */
static void sync_choices(void)
{
	UWORD profile = cfg.edit.videocap_profile;
	UWORD output = zzcfg_profile_output(profile);
	UWORD refresh = zzcfg_profile_refresh(profile);
	WORD output_index;
	WORD refresh_index;

	build_outputs();
	if (!output_count)
		return;
	output_index = map_index(output_map, output_count, output);
	stored_unsupported = output_index < 0;
	if (output_index < 0) {
		output = output_map[0];
		output_index = 0;
	}
	build_refreshes(output);
	GT_SetGadgetAttrs(output_gadget, gt_render_window(output_gadget), NULL,
		GTCY_Labels, (ULONG)output_choices,
		GTCY_Active, (ULONG)output_index,
		TAG_END);
	GT_SetGadgetAttrs(refresh_gadget, gt_render_window(refresh_gadget), NULL,
		GTCY_Labels, (ULONG)refresh_choices,
		TAG_END);
	refresh_index = map_index(refresh_map, refresh_count, refresh);
	if (refresh_index < 0) {
		stored_unsupported = TRUE;
		refresh = refresh_map[0];
		refresh_index = 0;
	}
	GT_SetGadgetAttrs(refresh_gadget, gt_render_window(refresh_gadget), NULL,
		GTCY_Active, (ULONG)refresh_index,
		TAG_END);
	shown_output = output;
	shown_refresh = refresh;
	pair_description(output, refresh);
	if (stored_unsupported)
		snprintf(description_text, sizeof(description_text), "%s",
			"Stored output unsupported here: pick one, then Save");
	gt_page_set_text(description_gadget, description_text);
	gt_page_set_text(refresh_note_gadget, refresh_note_text);
}

/* Refresh live framing, phase, and picture-action availability readouts. */
static void refresh_picture(void)
{
	struct zz_vcap_snapshot snapshot;
	int phase;

	if (settings_present(&cfg.edit, CFG_CROP_H) ||
		settings_present(&cfg.edit, CFG_CROP_V)) {
		snprintf(framing_text, sizeof(framing_text), "Custom %ux%u (%ux%u)",
			(unsigned)cfg.edit.videocap_crop_h,
			(unsigned)cfg.edit.videocap_crop_v,
			video_snapshot(&snapshot) ? (unsigned)snapshot.effective_h : 0,
			video_snapshot(&snapshot) ? (unsigned)snapshot.effective_v : 0);
	} else {
		snprintf(framing_text, sizeof(framing_text), "%s", "Automatic");
	}
	if (video_phase_state(&phase, picture_note_text, sizeof(picture_note_text))) {
		snprintf(sampling_text, sizeof(sampling_text), "%s, phase %d",
			video_phase_domain() == ZZ_VCAP_PHASE_C28 ? "C28" : "E7M",
			phase);
	} else {
		snprintf(sampling_text, sizeof(sampling_text), "%s", "Automatic");
	}
	if (!video_align_ready(picture_note_text, sizeof(picture_note_text))) {
		gt_page_set_disabled(align_gadget, TRUE);
		gt_page_set_disabled(auto_gadget, TRUE);
	} else {
		gt_page_set_disabled(align_gadget, FALSE);
		gt_page_set_disabled(auto_gadget, !video_geometry_supported());
		if (!video_geometry_supported()) {
			snprintf(picture_note_text, sizeof(picture_note_text), "%s",
				"Reset needs FW window confirmation.");
		}
	}
	gt_page_set_disabled(find_gadget,
		!video_find_phase_ready(picture_note_text, sizeof(picture_note_text)));
	gt_page_set_text(framing_gadget, framing_text);
	gt_page_set_text(sampling_gadget, sampling_text);
	gt_page_set_text(picture_note_gadget, picture_note_text);
}

/* Show the edited gamma, or disable the slider when the card has no
 * gamma LUT (config key 35 reads 0: old firmware or bitstream). */
static void gamma_sync(void)
{
	gamma_available = board_gamma() != 0;
	gt_page_set_disabled(gamma_gadget, !gamma_available);
	if (!gamma_available) {
		snprintf(gamma_text, sizeof(gamma_text), "%s", "Not available");
		gt_page_set_text(gamma_value_gadget, gamma_text);
		return;
	}
	gamma_level = (WORD)zzgamma_level_from_hundredths(cfg.edit.gamma);
	GT_SetGadgetAttrs(gamma_gadget, gt_render_window(gamma_gadget), NULL,
		GTSL_Level, (ULONG)gamma_level,
		TAG_END);
	zzgamma_format(cfg.edit.gamma, gamma_text);
	gt_page_set_text(gamma_value_gadget, gamma_text);
}

/* Apply a slider level live and stage it for Save, like the scanline
 * cycles: a chosen 1.00 is written as an explicit gamma = 1.00. */
static void gamma_apply(WORD level)
{
	UWORD hundredths;

	if (!gamma_available || level == gamma_level)
		return;
	hundredths = (UWORD)zzgamma_hundredths_from_level(level);
	gamma_level = level;
	board_set_gamma(hundredths);
	if (cfg.supported)
		settings_set(CFG_GAMMA, hundredths);
	zzgamma_format(hundredths, gamma_text);
	gt_page_set_text(gamma_value_gadget, gamma_text);
	if (board_gamma() != hundredths)
		gt_status("The card did not apply gamma %s", gamma_text);
}

/* Create the compact, fixed display page gadget chain. */
static BOOL create(struct gt_page *page)
{
	struct gt_form f;
	static const char *const top_labels[] = { "Output", "Refresh", NULL };
	WORD left, width, half, third;
	WORD style_lbl_w, darken_lbl_w, gamma_lbl_w, frame_lbl_w, samp_lbl_w;
	WORD gamma_val_w;
	WORD use_w, rem_w, env_note_w;
	struct NewGadget ng;
	struct TagItem scan_tags[] = {
		{ GTCY_Labels, (ULONG)scan_names },
		{ GTCY_Active, 0 },
		{ GA_RelVerify, TRUE },
		{ TAG_DONE, 0 }
	};
	struct TagItem parity_tags[] = {
		{ GTCY_Labels, (ULONG)parity_names },
		{ GTCY_Active, 0 },
		{ GA_RelVerify, TRUE },
		{ TAG_DONE, 0 }
	};
	struct TagItem gamma_tags[] = {
		{ GTSL_Min, 0 },
		{ GTSL_Max, ZZGAMMA_SLIDER_MAX_LEVEL },
		{ GTSL_Level, (ZZGAMMA_HUNDREDTHS_IDENTITY - ZZGAMMA_HUNDREDTHS_MIN) /
			ZZGAMMA_SLIDER_STEP },
		{ GA_RelVerify, TRUE },
		{ TAG_DONE, 0 }
	};
	struct TagItem frame_tags[] = {
		{ GTTX_Text, (ULONG)framing_text },
		{ GTTX_CopyText, TRUE },
		{ GTTX_Border, TRUE },
		{ GTTX_Clipped, TRUE },
		{ TAG_DONE, 0 }
	};
	struct TagItem samp_tags[] = {
		{ GTTX_Text, (ULONG)sampling_text },
		{ GTTX_CopyText, TRUE },
		{ GTTX_Border, TRUE },
		{ GTTX_Clipped, TRUE },
		{ TAG_DONE, 0 }
	};

	build_outputs();
	build_refreshes(output_count ? output_map[0] : 0);
	description_text[0] = '\0';
	refresh_note_text[0] = '\0';
	env_note_text[0] = '\0';
	framing_text[0] = '\0';
	sampling_text[0] = '\0';
	picture_note_text[0] = '\0';
	gamma_text[0] = '\0';
	gamma_level = -1;

	gt_page_begin(page);
	gt_form_init(&f, page, top_labels);
	left = f.left;
	width = f.width;
	half = (WORD)((width - 8) / 2);
	third = (WORD)((width - 8) / 3);

	output_gadget = gt_form_cycle(&f, G_OUTPUT, 0, "Output", output_choices);
	refresh_gadget = gt_form_cycle(&f, G_REFRESH, 1, "Refresh", refresh_choices);
	description_gadget = gt_add_note(page, G_DESCRIPTION, left, 2, width, description_text);
	refresh_note_gadget = gt_add_note(page, G_REFRESH_NOTE, left, 3, width, refresh_note_text);

	/* Row 4: ENV note + Use it & Remove buttons */
	use_w = gt_button_width("Use it");
	rem_w = gt_button_width("Remove");
	env_note_w = (WORD)(width - use_w - rem_w - 8);
	env_note_gadget = gt_add_note(page, G_ENV_NOTE, left, 4, env_note_w, env_note_text);
	env_use_gadget = gt_add_button(page, G_ENV_USE, (WORD)(left + env_note_w + 4), 4, use_w, "Use it");
	env_remove_gadget = gt_add_button(page, G_ENV_REMOVE,
		(WORD)(left + env_note_w + use_w + 8), 4, rem_w, "Remove");

	/* Row 5: Scanlines (Style & Darken) */
	style_lbl_w = (WORD)(gt_text_width("Style") + MAX_WORD(6, gt_layout.font_x));
	darken_lbl_w = (WORD)(gt_text_width("Darken") + MAX_WORD(6, gt_layout.font_x));
	memset(&ng, 0, sizeof(ng));
	ng.ng_LeftEdge = (WORD)(left + style_lbl_w);
	ng.ng_TopEdge = gt_row(5);
	ng.ng_Width = (WORD)(half - style_lbl_w);
	ng.ng_Height = gt_layout.row_height;
	ng.ng_GadgetText = (STRPTR)"Style";
	ng.ng_GadgetID = GT_GID(1, G_SCANLINES);
	ng.ng_Flags = PLACETEXT_LEFT;
	scan_gadget = gt_page_add(page, CYCLE_KIND, &ng, scan_tags);

	ng.ng_LeftEdge = (WORD)(left + half + 8 + darken_lbl_w);
	ng.ng_Width = (WORD)(half - darken_lbl_w);
	ng.ng_GadgetText = (STRPTR)"Darken";
	ng.ng_GadgetID = GT_GID(1, G_PARITY);
	parity_gadget = gt_page_add(page, CYCLE_KIND, &ng, parity_tags);

	/* Row 6: Gamma slider and its readout */
	gamma_lbl_w = (WORD)(gt_text_width("Gamma") + MAX_WORD(6, gt_layout.font_x));
	gamma_val_w = (WORD)(gt_text_width("Not available") +
		2 * MAX_WORD(6, gt_layout.font_x));
	ng.ng_LeftEdge = (WORD)(left + gamma_lbl_w);
	ng.ng_TopEdge = gt_row(6);
	ng.ng_Width = (WORD)(width - gamma_lbl_w - gamma_val_w - 8);
	ng.ng_GadgetText = (STRPTR)"Gamma";
	ng.ng_GadgetID = GT_GID(1, G_GAMMA);
	gamma_gadget = gt_page_add(page, SLIDER_KIND, &ng, gamma_tags);
	gamma_value_gadget = gt_add_note(page, G_GAMMA_VALUE,
		(WORD)(left + width - gamma_val_w), 6, gamma_val_w, gamma_text);

	/* Row 7: Picture readouts (Framing & Sampling) */
	frame_lbl_w = (WORD)(gt_text_width("Framing") + MAX_WORD(6, gt_layout.font_x));
	samp_lbl_w = (WORD)(gt_text_width("Sampling") + MAX_WORD(6, gt_layout.font_x));
	ng.ng_LeftEdge = (WORD)(left + frame_lbl_w);
	ng.ng_TopEdge = gt_row(7);
	ng.ng_Width = (WORD)(half - frame_lbl_w);
	ng.ng_GadgetText = (STRPTR)"Framing";
	ng.ng_GadgetID = GT_GID(1, G_FRAMING);
	framing_gadget = gt_page_add(page, TEXT_KIND, &ng, frame_tags);

	ng.ng_LeftEdge = (WORD)(left + half + 8 + samp_lbl_w);
	ng.ng_Width = (WORD)(half - samp_lbl_w);
	ng.ng_GadgetText = (STRPTR)"Sampling";
	ng.ng_GadgetID = GT_GID(1, G_SAMPLING);
	sampling_gadget = gt_page_add(page, TEXT_KIND, &ng, samp_tags);

	/* Row 8: Buttons */
	align_gadget = gt_add_button(page, G_ALIGN, left, 8, third, "Align picture...");
	find_gadget = gt_add_button(page, G_FIND, (WORD)(left + third + 4), 8, third, "Find phase...");
	auto_gadget = gt_add_button(page, G_AUTO, (WORD)(left + third * 2 + 8),
		8, (WORD)(width - third * 2 - 8), "Reset auto");

	/* Row 9: Picture note */
	picture_note_gadget = gt_add_note(page, G_PICTURE_NOTE, left, 9, width, picture_note_text);

	page->rows = 10;
	return output_gadget && refresh_gadget && description_gadget &&
		refresh_note_gadget && env_note_gadget && env_use_gadget &&
		env_remove_gadget && scan_gadget && parity_gadget &&
		gamma_gadget && gamma_value_gadget &&
		framing_gadget && sampling_gadget && align_gadget &&
		find_gadget && auto_gadget && picture_note_gadget;
}

/* Synchronize settings, ENV override state, and live-picture readouts. */
static void sync(void)
{
	BOOL config_ok = cfg.supported;

	sync_choices();
	gt_page_set_disabled(output_gadget, !config_ok);
	gt_page_set_disabled(refresh_gadget, !config_ok);
	gt_page_set_disabled(scan_gadget, FALSE);
	gt_page_set_disabled(parity_gadget, FALSE);
	GT_SetGadgetAttrs(scan_gadget, gt_render_window(scan_gadget), NULL,
		GTCY_Active, (ULONG)cfg.edit.scanline_mode,
		TAG_END);
	GT_SetGadgetAttrs(parity_gadget, gt_render_window(parity_gadget), NULL,
		GTCY_Active, (ULONG)cfg.edit.scanline_parity,
		TAG_END);
	gamma_sync();
	if (!config_ok) {
		snprintf(refresh_note_text, sizeof(refresh_note_text), "%s",
			"Output settings need FW 2.3+. Scanlines work live.");
		gt_page_set_text(refresh_note_gadget, refresh_note_text);
	}
	if (cfg.env & CFG_ENV_NATIVE) {
		snprintf(env_note_text, sizeof(env_note_text),
			"ENV profile %u overrides ZZ9000.CFG.",
			(unsigned)cfg.env_profile);
		gt_page_set_disabled(env_use_gadget, FALSE);
		gt_page_set_disabled(env_remove_gadget, FALSE);
	} else {
		snprintf(env_note_text, sizeof(env_note_text), "%s",
			"Applies after power-cycle.");
		gt_page_set_disabled(env_use_gadget, TRUE);
		gt_page_set_disabled(env_remove_gadget, TRUE);
	}
	gt_page_set_text(env_note_gadget, env_note_text);
	refresh_picture();
}

/* Apply a display-page user action and refresh its dependent controls. */
static void gadget(UWORD id, UWORD code)
{
	UWORD output;
	UWORD refresh;
	UWORD profile;

	switch (id) {
	case G_OUTPUT:
		if (code >= output_count)
			return;
		output = output_map[code];
		refresh = shown_refresh;
		if (zzcfg_profile_for_output_refresh(output, refresh, board.fw_caps) ==
			ZZCFG_VCAP_PROFILE_COUNT)
			refresh = first_refresh(output);
		profile = zzcfg_profile_for_output_refresh(output, refresh, board.fw_caps);
		video_set_profile(profile, action_text, sizeof(action_text));
		gt_status("%s", action_text);
		break;
	case G_REFRESH:
		if (code >= refresh_count)
			return;
		profile = zzcfg_profile_for_output_refresh(shown_output, refresh_map[code],
			board.fw_caps);
		video_set_profile(profile, action_text, sizeof(action_text));
		gt_status("%s", action_text);
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
	case G_GAMMA:
		gamma_apply((WORD)code);
		break;
	case G_ALIGN:
		video_align(action_text, sizeof(action_text));
		gt_status("%s", action_text);
		break;
	case G_FIND:
		video_find_phase(action_text, sizeof(action_text));
		gt_status("%s", action_text);
		break;
	case G_AUTO:
		video_reset_automatic(action_text, sizeof(action_text));
		gt_status("%s", action_text);
		break;
	case G_ENV_USE:
		/* Through the profile transition, so crop measured on the old
		 * capture grid is cleared and a live preview restored. */
		if (cfg.env & CFG_ENV_NATIVE) {
			video_set_profile(cfg.env_profile, action_text, sizeof(action_text));
			gt_status("%s", action_text);
		}
		break;
	case G_ENV_REMOVE:
		if (gt_ask("Remove|Cancel",
			"Remove native-video ENV: and ENVARC: overrides?") == 1) {
			if (settings_env_remove(CFG_ENV_NATIVE))
				gt_status("Native-video ENV override removed");
			else
				gt_status("Could not remove native-video ENV override");
		}
		break;
	}
	gt_pending_changed();
	sync();
}

/* Poll live picture state at two hertz while the page is visible, and
 * follow a gamma drag at the full tick rate (GadTools reports only the
 * release to the page). */
static void tick(void)
{
	LONG level;

	if (gamma_available) {
		level = (LONG)gt_page_get(gamma_gadget, GTSL_Level);
		if (level != gamma_level) {
			gamma_apply((WORD)level);
			gt_pending_changed();
		}
	}
	if (gt_ticks() % 5 == 0)
		refresh_picture();
}

struct gt_page gt_page_display = {
	"Display",
	"Native video",
	"Scandoubler output, scanlines, gamma and picture alignment.",
	0,
	NULL,
	NULL,
	10,
	create,
	NULL,
	NULL,
	gadget,
	tick,
	sync,
	NULL,
	NULL
};
