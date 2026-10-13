/*
 * ZZTop Scenes page: the active scene's master-chain controls.
 * Copyright (C) 2016-2026, Lucie L. Hartmann <lucie@mntre.com>
 *                          MNT Research GmbH, Berlin
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <exec/types.h>
#include <intuition/classusr.h>
#include <gadgets/chooser.h>
#include <gadgets/integer.h>
#include <gadgets/layout.h>
#include <gadgets/slider.h>
#include <gadgets/space.h>
#include <gadgets/string.h>
#include <proto/label.h>
#include <images/bevel.h>
#include <reaction/reaction_macros.h>
#include <proto/layout.h>
#include <proto/intuition.h>
#include <proto/chooser.h>
#include <proto/integer.h>
#include <proto/slider.h>
#include <proto/string.h>
#include <proto/space.h>

#include <stdio.h>
#include <string.h>

#include "audio.h"
#include "ui.h"
#include "zz9k/abi.h"

#define P PAGE_TUNING
#define G_SCENE 0
#define G_NAME 1
#define G_LPF 2
#define G_VOLUME 3
#define G_PREFACTOR 4
#define G_PAN 5
#define G_EQ 6
#define G_CEIL_PAULA 16
#define G_CEIL_AX 17

static Object *scene;
static Object *name;
static struct ui_slider lpf;
static struct ui_slider volume;
static struct ui_slider prefactor;
static struct ui_slider pan;
static Object *eq[AUDIO_EQ_BANDS];
static Object *ceil_paula;
static Object *ceil_ax;
static struct ui_text note;
static struct ui_text calibration_note;
static struct List *scene_labels;
static UWORD committed_volume;
static UWORD committed_prefactor;
static UWORD committed_pan;
static UWORD committed_eq[AUDIO_EQ_BANDS];
static char scene_names[AUDIO_SCENE_COUNT][AUDIO_NAME_CHARS + 1];
static const char *scene_text[AUDIO_SCENE_COUNT + 1];

/* Zeroed scene shown only if the model has not seeded the active slot yet. */
static const struct audio_scene fallback_scene;

/* Builds the active-scene chooser from the seeded model names. */
static void scene_label_text(void)
{
	UWORD index;

	for (index = 0; index < AUDIO_SCENE_COUNT; ++index) {
		const struct audio_scene *value = audio_scene(index);

		snprintf(scene_names[index], sizeof(scene_names[index]), "%s",
			value ? value->name : "Scene");
		scene_text[index] = scene_names[index];
	}
	scene_text[AUDIO_SCENE_COUNT] = NULL;
}

/* Replaces chooser nodes only after the gadget has adopted the new list. */
static void scene_labels_refresh(void)
{
	struct List *old = scene_labels;
	struct List *labels;

	scene_label_text();
	labels = ui_chooser_labels(scene_text);
	if (!labels)
		return;
	ui_set(P, scene, CHOOSER_Labels, labels, TAG_END);
	scene_labels = labels;
	ui_chooser_free_labels(old);
}

/* One equaliser band: a vertical slider over its centre frequency. */
static Object *eq_band(UWORD band, const char *label)
{
	eq[band] = (Object *)SliderObject,
		GA_ID, UI_GID(P, G_EQ + band),
		GA_RelVerify, TRUE,
		SLIDER_Min, 0,
		SLIDER_Max, 100,
		SLIDER_Orientation, SLIDER_VERTICAL,
		SLIDER_Invert, TRUE,
	End;
	return (Object *)VGroupObject,
		LAYOUT_AddChild, eq[band],
		LAYOUT_AddImage, LabelObject,
			LABEL_Text, label,
			LABEL_Justification, LJ_CENTRE,
		End,
		CHILD_WeightedHeight, 0,
	End;
}

/* Creates the compact scene page without adding a second model or window. */
static Object *create(void)
{
	static const char *const eq_label[AUDIO_EQ_BANDS] = {
		"31", "63", "125", "250", "500", "1k", "2k", "4k", "8k", "16k"
	};

	scene_label_text();
	scene_labels = ui_chooser_labels(scene_text);
	scene = (Object *)ChooserObject,
		GA_ID, UI_GID(P, G_SCENE),
		GA_RelVerify, TRUE,
		CHOOSER_Labels, scene_labels,
	End;
	name = (Object *)StringObject,
		GA_ID, UI_GID(P, G_NAME),
		GA_RelVerify, TRUE,
		STRINGA_MaxChars, AUDIO_NAME_CHARS + 1,
	End;
	ceil_paula = (Object *)IntegerObject,
		GA_ID, UI_GID(P, G_CEIL_PAULA),
		GA_RelVerify, TRUE,
		INTEGER_Minimum, 1,
		INTEGER_Maximum, 4095,
	End;
	ceil_ax = (Object *)IntegerObject,
		GA_ID, UI_GID(P, G_CEIL_AX),
		GA_RelVerify, TRUE,
		INTEGER_Minimum, 1,
		INTEGER_Maximum, 4095,
	End;
	return (Object *)VGroupObject,
		LAYOUT_SpaceOuter, TRUE,
		LAYOUT_AddChild, VGroupObject,
			LAYOUT_BevelStyle, BVS_GROUP,
			LAYOUT_Label, "Scene",
			LAYOUT_AddChild, scene, Label("Active scene"),
			LAYOUT_AddChild, name, Label("Name"),
			LAYOUT_AddChild, ui_slider_new(&lpf, UI_GID(P, G_LPF),
				1, 23900, "%ld Hz", "23900 Hz"),
				Label("Output lowpass"),
			LAYOUT_AddChild, ui_slider_new(&volume, UI_GID(P, G_VOLUME),
				0, 100, "%ld", "100"),
				Label("Volume"),
			LAYOUT_AddChild, ui_slider_new(&prefactor, UI_GID(P, G_PREFACTOR),
				0, 100, "%ld", "100"),
				Label("Pre-gain"),
			LAYOUT_AddChild, ui_slider_new(&pan, UI_GID(P, G_PAN),
				0, 100, "%ld", "100"),
				Label("Pan (50 = centre)"),
		End,
		LAYOUT_AddChild, VGroupObject,
			LAYOUT_BevelStyle, BVS_GROUP,
			LAYOUT_Label, "Equaliser (Hz)",
			LAYOUT_AddChild, HGroupObject,
				LAYOUT_EvenSize, TRUE,
				LAYOUT_AddChild, eq_band(0, eq_label[0]),
				LAYOUT_AddChild, eq_band(1, eq_label[1]),
				LAYOUT_AddChild, eq_band(2, eq_label[2]),
				LAYOUT_AddChild, eq_band(3, eq_label[3]),
				LAYOUT_AddChild, eq_band(4, eq_label[4]),
				LAYOUT_AddChild, eq_band(5, eq_label[5]),
				LAYOUT_AddChild, eq_band(6, eq_label[6]),
				LAYOUT_AddChild, eq_band(7, eq_label[7]),
				LAYOUT_AddChild, eq_band(8, eq_label[8]),
				LAYOUT_AddChild, eq_band(9, eq_label[9]),
			End,
			CHILD_MinHeight, 72,
			LAYOUT_AddChild, ui_note_new(&note, "Middle position = flat (0 dB)."),
			CHILD_WeightedHeight, 0,
		End,
		LAYOUT_AddChild, VGroupObject,
			LAYOUT_BevelStyle, BVS_GROUP,
			LAYOUT_Label, "Card calibration",
			LAYOUT_AddChild, HGroupObject,
				LAYOUT_AddChild, ceil_paula, Label("Paula clean ceiling"),
				LAYOUT_AddChild, ceil_ax, Label("AX clean ceiling"),
			End,
			LAYOUT_AddChild, ui_note_new(&calibration_note,
				"Measured for each card; they set the mix limit."),
				CHILD_WeightedHeight, 0,
		End,
		LAYOUT_AddChild, SpaceObject, End,
	End;
}

/* Fully refreshes the page from shared editor state after any model transition. */
static void sync(void)
{
	const struct audio_scene *value = audio_scene(audio_active_scene());
	BOOL ok = audio_available();
	BOOL locked = audio_calibration_pending();
	UWORD index;

	if (!value)
		value = &fallback_scene;
	ui_disable(P, scene, !ok || locked);
	ui_disable(P, name, !ok || locked);
	ui_disable(P, lpf.obj, !ok || locked);
	ui_disable(P, volume.obj, !ok || locked);
	ui_disable(P, prefactor.obj, !ok || locked);
	ui_disable(P, pan.obj, !ok || locked);
	ui_disable(P, ceil_paula, !ok || locked);
	ui_disable(P, ceil_ax, !ok || locked);
	for (index = 0; index < AUDIO_EQ_BANDS; ++index)
		ui_disable(P, eq[index], !ok || locked);
	/* Seeded defaults stay visible, controls disabled, without the plane. */
	scene_labels_refresh();
	ui_set(P, scene, CHOOSER_Selected, audio_active_scene(), TAG_END);
	ui_set(P, name, STRINGA_TextVal, value->name, TAG_END);
	ui_slider_set(P, &lpf, (LONG)value->lpf);
	ui_slider_set(P, &volume, (LONG)value->volume);
	ui_slider_set(P, &prefactor, (LONG)value->prefactor);
	ui_slider_set(P, &pan, (LONG)value->pan);
	for (index = 0; index < AUDIO_EQ_BANDS; ++index)
		ui_set(P, eq[index], SLIDER_Level, value->eq[index], TAG_END);
	ui_set(P, ceil_paula, INTEGER_Number, audio_ceiling_paula(), TAG_END);
	ui_set(P, ceil_ax, INTEGER_Number, audio_ceiling_ax(), TAG_END);
	committed_volume = value->volume;
	committed_prefactor = value->prefactor;
	committed_pan = value->pan;
	for (index = 0; index < AUDIO_EQ_BANDS; ++index)
		committed_eq[index] = value->eq[index];
	if (!ok) {
		ui_text_set(P, &note, audio_unavailable_reason());
		return;
	}
	if (locked)
		ui_text_set(P, &note,
			"Waiting for the card to confirm the new calibration");
	else if (audio_last_error()[0])
		ui_text_set(P, &note, audio_last_error());
	else
		ui_text_set(P, &note, "Middle position = flat (0 dB).");
}

/* Polls only smoothable slider drags at the legacy 100 ms cadence. */
static void tick(void)
{
	UWORD active;
	UWORD index;
	UWORD value;
	BOOL changed = FALSE;

	if (!audio_available() || audio_calibration_pending())
		return;
	active = audio_active_scene();
	/* Lowpass commits on release only (safeload); polling just keeps the
	 * printed value in step while it is dragged. */
	(void)ui_slider_poll(P, &lpf);
	value = (UWORD)ui_slider_poll(P, &volume);
	if (value != committed_volume &&
		audio_set_scene_value(active, ZZ9K_AUDIO_SCENE_PARAM_VOLUME, value)) {
		committed_volume = value;
		changed = TRUE;
	}
	value = (UWORD)ui_slider_poll(P, &prefactor);
	if (value != committed_prefactor &&
		audio_set_scene_value(active, ZZ9K_AUDIO_SCENE_PARAM_PREFACTOR, value)) {
		committed_prefactor = value;
		changed = TRUE;
	}
	value = (UWORD)ui_slider_poll(P, &pan);
	if (value != committed_pan &&
		audio_set_scene_value(active, ZZ9K_AUDIO_SCENE_PARAM_PAN, value)) {
		committed_pan = value;
		changed = TRUE;
	}
	for (index = 0; index < AUDIO_EQ_BANDS; ++index) {
		value = (UWORD)ui_get(eq[index], SLIDER_Level);
		if (value != committed_eq[index] && audio_set_scene_value(active,
			ZZ9K_AUDIO_SCENE_PARAM_EQ_BAND_1 + index, value)) {
			committed_eq[index] = value;
			changed = TRUE;
		}
	}
	if (changed)
		ui_pending_changed();
}

/* Handles final release commits, leaving output lowpass release-only by design. */
static void gadget(UWORD id, UWORD code)
{
	UWORD active = audio_active_scene();
	BOOL changed = FALSE;

	if (!audio_available())
		return;
	if (id == G_SCENE) {
		if (!audio_select_scene(code)) {
			ui_set(P, scene, CHOOSER_Selected, active, TAG_END);
			ui_status("%s", audio_last_error());
		} else {
			changed = TRUE;
		}
	} else if (id == G_NAME) {
		const char *text = (const char *)ui_get(name, STRINGA_TextVal);

		if (text)
			changed = audio_rename_scene(active, text);
	} else if (id == G_LPF) {
		/* Safeload makes a coefficient write atomic, not inaudibly slewed. */
		changed = audio_set_scene_value(active, ZZ9K_AUDIO_SCENE_PARAM_LPF, code);
	} else if (id == G_VOLUME) {
		changed = audio_set_scene_value(active, ZZ9K_AUDIO_SCENE_PARAM_VOLUME, code);
	} else if (id == G_PREFACTOR) {
		changed = audio_set_scene_value(active, ZZ9K_AUDIO_SCENE_PARAM_PREFACTOR, code);
	} else if (id == G_PAN) {
		changed = audio_set_scene_value(active, ZZ9K_AUDIO_SCENE_PARAM_PAN, code);
	} else if (id >= G_EQ && id < G_EQ + AUDIO_EQ_BANDS) {
		changed = audio_set_scene_value(active,
			ZZ9K_AUDIO_SCENE_PARAM_EQ_BAND_1 + id - G_EQ, code);
	} else if (id == G_CEIL_PAULA || id == G_CEIL_AX) {
		changed = audio_set_calibration((UWORD)ui_get(ceil_paula, INTEGER_Number),
			(UWORD)ui_get(ceil_ax, INTEGER_Number));
	}
	if (changed)
		ui_pending_changed();
	sync();
}


/* Frees page-owned chooser nodes after the gadget tree is gone. */
static void cleanup(void)
{
	ui_chooser_free_labels(scene_labels);
	scene_labels = NULL;
}
struct ui_page page_tuning = {
	"Scenes",
	"Scene editor",
	"Filter, equaliser and calibration of the audio scenes.",
	UI_PAGE_ADVANCED | UI_PAGE_CHILD,
	create,
	sync,
	NULL,
	gadget,
	tick,
	sync,
	NULL,
	cleanup,
};
