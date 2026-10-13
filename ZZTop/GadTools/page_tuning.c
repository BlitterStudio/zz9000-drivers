/*
 * ZZTop GadTools Scenes page: active-scene master-chain controls.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <exec/types.h>
#include <graphics/rastport.h>
#include <libraries/gadtools.h>
#include <proto/gadtools.h>
#include <proto/graphics.h>
#include <stdio.h>
#include <string.h>

#include "audio.h"
#include "gt.h"
#include "zz9k/abi.h"

#define P GT_PAGE_TUNING

#define G_SCENE 0
#define G_NAME 1
#define G_LPF 2
#define G_VOLUME 3
#define G_PREFACTOR 4
#define G_PAN 5
#define G_EQ 6
#define G_CEIL_PAULA 16
#define G_CEIL_AX 17
#define G_NOTE 18
#define G_CAL_NOTE 19

static struct Gadget *scene;
static struct Gadget *name;
static struct Gadget *lpf;
static struct Gadget *volume;
static struct Gadget *prefactor;
static struct Gadget *pan;
static struct Gadget *eq[AUDIO_EQ_BANDS];
static struct Gadget *ceil_paula;
static struct Gadget *ceil_ax;
static struct Gadget *note;
static struct Gadget *cal_note;

static char scene_names[AUDIO_SCENE_COUNT][AUDIO_NAME_CHARS + 1];
static STRPTR scene_labels[AUDIO_SCENE_COUNT + 1];
static char name_buffer[AUDIO_NAME_CHARS + 1];
static char note_buffer[96];
static char cal_note_buffer[96];
static UWORD committed_volume;
static UWORD committed_prefactor;
static UWORD committed_pan;
static UWORD committed_eq[AUDIO_EQ_BANDS];

static const struct audio_scene fallback_scene;
static const char *const eq_label[AUDIO_EQ_BANDS] = {
	"31", "63", "125", "250", "500", "1k", "2k", "4k", "8k", "16k"
};

/* Change one tuning gadget attribute without drawing hidden pages. */
static void tuning_set(struct Gadget *gadget, ULONG tag, ULONG value)
{
	struct TagItem tags[2];

	if (!gadget)
		return;
	tags[0].ti_Tag = tag;
	tags[0].ti_Data = value;
	tags[1].ti_Tag = TAG_END;
	tags[1].ti_Data = 0;
	GT_SetGadgetAttrsA(gadget, gt_render_window(gadget), NULL, tags);
}

/* Build the scene cycle labels from persistent scene-name storage. */
static void tuning_scene_labels(void)
{
	UWORD index;
	const struct audio_scene *value;

	for (index = 0; index < AUDIO_SCENE_COUNT; ++index) {
		value = audio_scene(index);
		snprintf(scene_names[index], sizeof(scene_names[index]), "%s",
			value ? value->name : "Scene");
		scene_labels[index] = (STRPTR)scene_names[index];
	}
	scene_labels[AUDIO_SCENE_COUNT] = NULL;
}

/* Enable or disable tuning controls as audio support permits. */
static void tuning_set_enabled(BOOL enabled)
{
	UWORD index;

	gt_page_set_disabled(scene, !enabled);
	gt_page_set_disabled(name, !enabled);
	gt_page_set_disabled(lpf, !enabled);
	gt_page_set_disabled(volume, !enabled);
	gt_page_set_disabled(prefactor, !enabled);
	gt_page_set_disabled(pan, !enabled);
	gt_page_set_disabled(ceil_paula, !enabled);
	gt_page_set_disabled(ceil_ax, !enabled);
	for (index = 0; index < AUDIO_EQ_BANDS; ++index)
		gt_page_set_disabled(eq[index], !enabled);
}

/* Draw the band frequencies above the equaliser sliders (row 3). */
static void tuning_draw_labels(void)
{
	struct Window *win = gt_window();
	struct RastPort *rp;
	WORD left, width, eq_w;
	UWORD index;

	if (!win || gt_current_page() != P)
		return;
	rp = win->RPort;
	left = gt_layout.page_left;
	width = gt_layout.page_width;
	eq_w = (WORD)((width - 9 * 4) / AUDIO_EQ_BANDS);
	SetAPen(rp, 1);
	for (index = 0; index < AUDIO_EQ_BANDS; ++index) {
		WORD sl_left = (WORD)(left + index * (eq_w + 4));
		WORD lbl_w = gt_text_width(eq_label[index]);
		WORD x = (WORD)(sl_left + (eq_w - lbl_w) / 2);
		WORD y = (WORD)(gt_row(3) + (gt_layout.row_height - rp->TxHeight) / 2 +
			rp->TxBaseline);
		Move(rp, x, y);
		Text(rp, (CONST_STRPTR)eq_label[index], (ULONG)strlen(eq_label[index]));
	}
}

/* Create the scene editor, equaliser, and calibration controls. */
static BOOL tuning_create(struct gt_page *page)
{
	struct NewGadget ng;
	WORD left, width, half;
	WORD left_lbl, right_lbl;
	WORD lpf_val_w, vol_val_w;
	WORD eq_w, eq_h;
	UWORD index;
	struct TagItem cycle_tags[] = {
		{ GTCY_Labels, (ULONG)scene_labels },
		{ GTCY_Active, 0 },
		{ GA_RelVerify, TRUE },
		{ TAG_DONE, 0 }
	};
	struct TagItem str_tags[] = {
		{ GTST_String, (ULONG)name_buffer },
		{ GTST_MaxChars, AUDIO_NAME_CHARS + 1 },
		{ GA_RelVerify, TRUE },
		{ TAG_DONE, 0 }
	};
	struct TagItem eq_tags[] = {
		{ GTSL_Min, 0 },
		{ GTSL_Max, 100 },
		{ GTSL_Level, 50 },
		{ PGA_Freedom, LORIENT_VERT },
		{ GA_RelVerify, TRUE },
		{ TAG_DONE, 0 }
	};
	struct TagItem int_tags[] = {
		{ GTIN_Number, 1 },
		{ GTIN_MaxChars, 4 },
		{ GA_RelVerify, TRUE },
		{ TAG_DONE, 0 }
	};
	struct TagItem lpf_tags[] = {
		{ GTSL_Min, 1 },
		{ GTSL_Max, 23900 },
		{ GTSL_Level, 23900 },
		{ GTSL_LevelFormat, (ULONG)"%ld Hz" },
		{ GTSL_MaxLevelLen, 9 },
		{ GTSL_LevelPlace, PLACETEXT_RIGHT },
		{ GA_RelVerify, TRUE },
		{ TAG_DONE, 0 }
	};
	struct TagItem vol_tags[] = {
		{ GTSL_Min, 0 },
		{ GTSL_Max, 100 },
		{ GTSL_Level, 50 },
		{ GTSL_LevelFormat, (ULONG)"%ld" },
		{ GTSL_MaxLevelLen, 4 },
		{ GTSL_LevelPlace, PLACETEXT_RIGHT },
		{ GA_RelVerify, TRUE },
		{ TAG_DONE, 0 }
	};

	gt_page_begin(page);
	tuning_scene_labels();
	left = gt_layout.page_left;
	width = gt_layout.page_width;
	half = (WORD)((width - 8) / 2);

	left_lbl = (WORD)(gt_text_width("Pre-gain") + MAX_WORD(6, gt_layout.font_x));
	right_lbl = (WORD)(gt_text_width("Volume") + MAX_WORD(6, gt_layout.font_x));
	lpf_val_w = (WORD)(gt_text_width("24000 Hz") + MAX_WORD(8, gt_layout.font_x));
	vol_val_w = (WORD)(gt_text_width("100") + MAX_WORD(8, gt_layout.font_x));

	memset(&ng, 0, sizeof(ng));
	ng.ng_Height = gt_layout.row_height;
	ng.ng_Flags = PLACETEXT_LEFT;

	/* Row 0: Scene & Name */
	ng.ng_LeftEdge = (WORD)(left + left_lbl);
	ng.ng_TopEdge = gt_row(0);
	ng.ng_Width = (WORD)(half - left_lbl);
	ng.ng_GadgetText = (STRPTR)"Scene";
	ng.ng_GadgetID = GT_GID(P, G_SCENE);
	scene = gt_page_add(page, CYCLE_KIND, &ng, cycle_tags);

	ng.ng_LeftEdge = (WORD)(left + half + 8 + right_lbl);
	ng.ng_Width = (WORD)(half - right_lbl);
	ng.ng_GadgetText = (STRPTR)"Name";
	ng.ng_GadgetID = GT_GID(P, G_NAME);
	name = gt_page_add(page, STRING_KIND, &ng, str_tags);

	/* Row 1: Lowpass & Volume */
	ng.ng_LeftEdge = (WORD)(left + left_lbl);
	ng.ng_TopEdge = gt_row(1);
	ng.ng_Width = (WORD)(half - left_lbl - lpf_val_w);
	ng.ng_GadgetText = (STRPTR)"Lowpass";
	ng.ng_GadgetID = GT_GID(P, G_LPF);
	lpf = gt_page_add(page, SLIDER_KIND, &ng, lpf_tags);

	ng.ng_LeftEdge = (WORD)(left + half + 8 + right_lbl);
	ng.ng_Width = (WORD)(half - right_lbl - vol_val_w);
	ng.ng_GadgetText = (STRPTR)"Volume";
	ng.ng_GadgetID = GT_GID(P, G_VOLUME);
	volume = gt_page_add(page, SLIDER_KIND, &ng, vol_tags);

	/* Row 2: Pre-gain & Pan */
	ng.ng_LeftEdge = (WORD)(left + left_lbl);
	ng.ng_TopEdge = gt_row(2);
	ng.ng_Width = (WORD)(half - left_lbl - vol_val_w);
	ng.ng_GadgetText = (STRPTR)"Pre-gain";
	ng.ng_GadgetID = GT_GID(P, G_PREFACTOR);
	prefactor = gt_page_add(page, SLIDER_KIND, &ng, vol_tags);

	ng.ng_LeftEdge = (WORD)(left + half + 8 + right_lbl);
	ng.ng_Width = (WORD)(half - right_lbl - vol_val_w);
	ng.ng_GadgetText = (STRPTR)"Pan";
	ng.ng_GadgetID = GT_GID(P, G_PAN);
	pan = gt_page_add(page, SLIDER_KIND, &ng, vol_tags);

	/* Row 3: band frequencies, drawn by tuning_draw_labels(). */

	/* Rows 4-5: 10 EQ sliders */
	eq_w = (WORD)((width - 9 * 4) / AUDIO_EQ_BANDS);
	eq_h = (WORD)(gt_layout.row_height * 2);
	ng.ng_TopEdge = gt_row(4);
	ng.ng_Width = eq_w;
	ng.ng_Height = eq_h;
	ng.ng_GadgetText = NULL;
	for (index = 0; index < AUDIO_EQ_BANDS; ++index) {
		ng.ng_LeftEdge = (WORD)(left + index * (eq_w + 4));
		ng.ng_GadgetID = GT_GID(P, G_EQ + index);
		eq[index] = gt_page_add(page, SLIDER_KIND, &ng, eq_tags);
		if (!eq[index])
			return FALSE;
	}

	/* Row 6: Note */
	snprintf(note_buffer, sizeof(note_buffer), "%s", "Equaliser: middle position = flat (0 dB).");
	note = gt_add_note(page, G_NOTE, left, 6, width, note_buffer);

	/* Row 7: Calibration Heading */
	gt_add_note(page, 0, left, 7, width, (char *)"Card calibration ceilings:");

	/* Row 8: Paula ceiling & AX ceiling */
	ng.ng_TopEdge = gt_row(8);
	ng.ng_Height = gt_layout.row_height;
	ng.ng_GadgetText = (STRPTR)"Paula";
	ng.ng_LeftEdge = (WORD)(left + left_lbl);
	ng.ng_Width = (WORD)(half - left_lbl);
	ng.ng_GadgetID = GT_GID(P, G_CEIL_PAULA);
	ceil_paula = gt_page_add(page, INTEGER_KIND, &ng, int_tags);

	ng.ng_LeftEdge = (WORD)(left + half + 8 + right_lbl);
	ng.ng_Width = (WORD)(half - right_lbl);
	ng.ng_GadgetText = (STRPTR)"AX";
	ng.ng_GadgetID = GT_GID(P, G_CEIL_AX);
	ceil_ax = gt_page_add(page, INTEGER_KIND, &ng, int_tags);

	/* Row 9: Calibration note */
	snprintf(cal_note_buffer, sizeof(cal_note_buffer), "%s",
		"Measured for each card; sets mix limit.");
	cal_note = gt_add_note(page, G_CAL_NOTE, left, 9, width, cal_note_buffer);

	page->rows = 10;
	return scene && name && lpf && volume && prefactor && pan && note &&
		ceil_paula && ceil_ax && cal_note;
}

/* Synchronise scene editor values from the active audio scene. */
static void tuning_sync(void)
{
	const struct audio_scene *value;
	BOOL ok, locked;
	UWORD index;

	value = audio_scene(audio_active_scene());
	if (!value)
		value = &fallback_scene;
	ok = audio_available();
	locked = audio_calibration_pending();
	tuning_set_enabled(ok && !locked);
	tuning_scene_labels();
	tuning_set(scene, GTCY_Labels, (ULONG)scene_labels);
	tuning_set(scene, GTCY_Active, audio_active_scene());
	snprintf(name_buffer, sizeof(name_buffer), "%s", value->name);
	tuning_set(name, GTST_String, (ULONG)name_buffer);
	tuning_set(lpf, GTSL_Level, value->lpf);
	tuning_set(volume, GTSL_Level, value->volume);
	tuning_set(prefactor, GTSL_Level, value->prefactor);
	tuning_set(pan, GTSL_Level, value->pan);
	for (index = 0; index < AUDIO_EQ_BANDS; ++index)
		tuning_set(eq[index], GTSL_Level, value->eq[index]);
	tuning_set(ceil_paula, GTIN_Number, audio_ceiling_paula());
	tuning_set(ceil_ax, GTIN_Number, audio_ceiling_ax());
	committed_volume = value->volume;
	committed_prefactor = value->prefactor;
	committed_pan = value->pan;
	for (index = 0; index < AUDIO_EQ_BANDS; ++index)
		committed_eq[index] = value->eq[index];
	if (!ok)
		gt_page_set_text(note, audio_unavailable_reason());
	else if (locked)
		gt_page_set_text(note, "Waiting for card calibration confirmation...");
	else if (audio_last_error()[0])
		gt_page_set_text(note, audio_last_error());
	else
		gt_page_set_text(note, "Equaliser: middle position = flat (0 dB).");
	tuning_draw_labels();
}

/* Synchronise and decorate the tuning page when it becomes visible. */
static void tuning_show(void)
{
	tuning_sync();
	tuning_draw_labels();
}

/* Coalesce active scene drags on the page timer cadence. */
static void tuning_tick(void)
{
	UWORD index;
	LONG val;

	if (!audio_available() || audio_calibration_pending())
		return;
	val = (LONG)gt_page_get(volume, GTSL_Level);
	if (val != (LONG)committed_volume &&
		audio_set_scene_value(audio_active_scene(), ZZ9K_AUDIO_SCENE_PARAM_VOLUME, (UWORD)val)) {
		committed_volume = (UWORD)val;
		gt_pending_changed();
	}
	val = (LONG)gt_page_get(prefactor, GTSL_Level);
	if (val != (LONG)committed_prefactor &&
		audio_set_scene_value(audio_active_scene(), ZZ9K_AUDIO_SCENE_PARAM_PREFACTOR, (UWORD)val)) {
		committed_prefactor = (UWORD)val;
		gt_pending_changed();
	}
	val = (LONG)gt_page_get(pan, GTSL_Level);
	if (val != (LONG)committed_pan &&
		audio_set_scene_value(audio_active_scene(), ZZ9K_AUDIO_SCENE_PARAM_PAN, (UWORD)val)) {
		committed_pan = (UWORD)val;
		gt_pending_changed();
	}
	for (index = 0; index < AUDIO_EQ_BANDS; ++index) {
		val = (LONG)gt_page_get(eq[index], GTSL_Level);
		if (val != (LONG)committed_eq[index] &&
			audio_set_scene_value(audio_active_scene(),
				ZZ9K_AUDIO_SCENE_PARAM_EQ_BAND_1 + index, (UWORD)val)) {
			committed_eq[index] = (UWORD)val;
			gt_pending_changed();
		}
	}
}

/* Apply one scene editor gadget action to the audio model. */
static void tuning_gadget(UWORD id, UWORD code)
{
	BOOL changed = FALSE;

	if (!audio_available() || audio_calibration_pending())
		return;
	if (id == G_SCENE) {
		if (code < AUDIO_SCENE_COUNT && audio_select_scene(code))
			changed = TRUE;
		else
			tuning_set(scene, GTCY_Active, audio_active_scene());
	} else if (id == G_NAME) {
		const char *text = (const char *)gt_page_get(name, GTST_String);
		if (text && audio_rename_scene(audio_active_scene(), text))
			changed = TRUE;
	} else if (id == G_LPF) {
		changed = audio_set_scene_value(audio_active_scene(), ZZ9K_AUDIO_SCENE_PARAM_LPF, code);
	} else if (id == G_VOLUME) {
		changed = audio_set_scene_value(audio_active_scene(), ZZ9K_AUDIO_SCENE_PARAM_VOLUME, code);
	} else if (id == G_PREFACTOR) {
		changed = audio_set_scene_value(audio_active_scene(), ZZ9K_AUDIO_SCENE_PARAM_PREFACTOR, code);
	} else if (id == G_PAN) {
		changed = audio_set_scene_value(audio_active_scene(), ZZ9K_AUDIO_SCENE_PARAM_PAN, code);
	} else if (id >= G_EQ && id < G_EQ + AUDIO_EQ_BANDS) {
		changed = audio_set_scene_value(audio_active_scene(),
			ZZ9K_AUDIO_SCENE_PARAM_EQ_BAND_1 + (id - G_EQ), code);
	} else if (id == G_CEIL_PAULA || id == G_CEIL_AX) {
		changed = audio_set_calibration(
			(UWORD)gt_page_get(ceil_paula, GTIN_Number),
			(UWORD)gt_page_get(ceil_ax, GTIN_Number));
	}
	if (changed)
		gt_pending_changed();
	tuning_sync();
}

struct gt_page gt_page_tuning = {
	"Scenes",
	"Scene editor",
	"Filter, equaliser and calibration of the audio scenes.",
	GT_PAGE_ADVANCED | GT_PAGE_CHILD,
	NULL,
	NULL,
	10,
	tuning_create,
	tuning_show,
	NULL,
	tuning_gadget,
	tuning_tick,
	tuning_sync,
	NULL,
	NULL
};
