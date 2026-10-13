/*
 * ZZTop Audio page: live mixer baseline and card meter display.
 * Copyright (C) 2016-2026, Lucie L. Hartmann <lucie@mntre.com>
 *                          MNT Research GmbH, Berlin
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <exec/types.h>
#include <intuition/classusr.h>
#include <gadgets/button.h>
#include <gadgets/chooser.h>
#include <gadgets/fuelgauge.h>
#include <gadgets/layout.h>
#include <gadgets/space.h>
#include <images/bevel.h>
#include <reaction/reaction_macros.h>
#include <proto/layout.h>
#include <proto/intuition.h>
#include <proto/button.h>
#include <proto/chooser.h>
#include <proto/label.h>
#include <proto/space.h>

#include <stdio.h>
#include <string.h>

#include "audio.h"
#include "ui.h"

#define P PAGE_AUDIO
#define G_SCENE 0
#define G_PAULA 1
#define G_AX 2
#define G_BALANCE 3
#define G_RETRY 4

static Object *scene;
static Object *balance;
static Object *retry;
static struct ui_slider paula;
static struct ui_slider ax;
static struct ui_gauge headroom;
static struct ui_gauge out_l;
static struct ui_gauge out_r;
static struct ui_gauge in_l;
static struct ui_gauge in_r;
static struct ui_text out_counts;
static struct ui_text in_counts;
static struct ui_text meter_note;
static struct ui_text state;
static struct List *scene_labels;
static struct audio_meter meter_open_out;
static struct audio_meter meter_open_in;
static UWORD committed_paula;
static UWORD committed_ax;
static char scene_names[AUDIO_SCENE_COUNT][AUDIO_NAME_CHARS + 1];
static const char *scene_text[AUDIO_SCENE_COUNT + 1];

/* Approximates a 16.16 peak in fixed point on the visible -60..0 dBFS scale. */
static LONG dbfs(ULONG fixed)
{
	LONG db = 0;

	if (fixed == 0)
		return -60;
	while (fixed < 0x8000UL && db > -60) {
		fixed <<= 1;
		db -= 6;
	}
	if (fixed < 0x10000UL)
		db -= (LONG)((0x10000UL - fixed) * 6UL / 0x8000UL);
	if (db < -60)
		db = -60;
	return db;
}

/* Converts fixed dBFS into the zero-based fuel-gauge range. */
static ULONG meter_level(ULONG fixed)
{
	return (ULONG)(dbfs(fixed) + 60);
}

/* Draws one channel peak into its gauge with the level as short text. */
static void meter_show(struct ui_gauge *g, ULONG fixed)
{
	char text[16];

	if (fixed == 0)
		snprintf(text, sizeof(text), "-inf");
	else
		snprintf(text, sizeof(text), "%ld dB", (long)dbfs(fixed));
	ui_gauge_set(P, g, (LONG)meter_level(fixed), text);
}

/* Reports a saturating meter counter delta without unsigned wrap after a reset. */
static ULONG meter_delta(ULONG current, ULONG opening)
{
	if (current >= opening)
		return current - opening;
	if (current != 0)
		return 1;
	return 0;
}
/* Copies current scene names before building the one-time chooser labels. */
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

/* Creates the compact page and gives every live control a release hook. */
static Object *create(void)
{
	scene_label_text();
	scene_labels = ui_chooser_labels(scene_text);
	scene = (Object *)ChooserObject,
		GA_ID, UI_GID(P, G_SCENE),
		GA_RelVerify, TRUE,
		CHOOSER_Labels, scene_labels,
	End;
	balance = (Object *)NewObject(BUTTON_GetClass(), NULL,
		GA_ID, UI_GID(P, G_BALANCE),
		GA_RelVerify, TRUE,
		GA_Text, "_Balance",
	End;
	retry = (Object *)NewObject(BUTTON_GetClass(), NULL,
		GA_ID, UI_GID(P, G_RETRY),
		GA_RelVerify, TRUE,
		GA_Text, "_Retry",
	End;
	return (Object *)VGroupObject,
		LAYOUT_SpaceOuter, TRUE,
		LAYOUT_AddChild, VGroupObject,
			LAYOUT_BevelStyle, BVS_GROUP,
			LAYOUT_Label, "Scene",
			LAYOUT_AddChild, scene, Label("Active scene"),
		End,
		LAYOUT_AddChild, VGroupObject,
			LAYOUT_BevelStyle, BVS_GROUP,
			LAYOUT_Label, "Mix",
			LAYOUT_AddChild, ui_slider_new(&paula, UI_GID(P, G_PAULA),
				0, 255, "%ld", "255"),
				Label("Paula level"),
			LAYOUT_AddChild, ui_slider_new(&ax, UI_GID(P, G_AX),
				0, 255, "%ld", "255"),
				Label("AX level"),
			LAYOUT_AddChild, HGroupObject,
				LAYOUT_AddChild, balance,
					CHILD_WeightedWidth, 0,
				LAYOUT_AddChild, ui_gauge_new(&headroom, 1),
			End,
			Label("Mix limit"),
		End,
		LAYOUT_AddChild, VGroupObject,
			LAYOUT_BevelStyle, BVS_GROUP,
			LAYOUT_Label, "Meters",
			LAYOUT_AddChild, HGroupObject,
				LAYOUT_AddChild, ui_gauge_new(&out_l, 60),
				LAYOUT_AddChild, ui_gauge_new(&out_r, 60),
			End,
			Label("Output"),
			LAYOUT_AddChild, ui_note_new(&out_counts, ""),
				CHILD_WeightedHeight, 0,
			LAYOUT_AddChild, HGroupObject,
				LAYOUT_AddChild, ui_gauge_new(&in_l, 60),
				LAYOUT_AddChild, ui_gauge_new(&in_r, 60),
			End,
			Label("Input"),
			LAYOUT_AddChild, ui_note_new(&in_counts, ""),
				CHILD_WeightedHeight, 0,
			LAYOUT_AddChild, ui_note_new(&meter_note, ""),
				CHILD_WeightedHeight, 0,
		End,
		LAYOUT_AddChild, HGroupObject,
			LAYOUT_AddChild, ui_note_new(&state,
				"Changes apply live; Save keeps them after a power-cycle."),
			LAYOUT_AddChild, retry,
				CHILD_WeightedWidth, 0,
		End,
		LAYOUT_AddChild, SpaceObject, End,
	End;
}

/* Synchronizes every control from the shared audio model and its gate. */
static void sync(void)
{
	BOOL ok = audio_available();
	BOOL locked = audio_calibration_pending();
	ULONG weighted = audio_weighted_level();
	ULONG limit = audio_boundary();
	char level[32];

	ui_disable(P, scene, !ok || locked);
	ui_disable(P, paula.obj, !ok || locked);
	ui_disable(P, ax.obj, !ok || locked);
	ui_disable(P, balance, !ok || locked);
	ui_disable(P, retry, !audio_retry_available());
	/* Seeded model values keep the disabled display truthful when offline. */
	scene_labels_refresh();
	ui_set(P, scene, CHOOSER_Selected, audio_active_scene(), TAG_END);
	ui_slider_set_max(P, &paula, (LONG)audio_baseline_max_paula());
	ui_slider_set(P, &paula, (LONG)audio_baseline_paula());
	ui_slider_set_max(P, &ax, (LONG)audio_baseline_max_ax());
	ui_slider_set(P, &ax, (LONG)audio_baseline_ax());
	committed_paula = audio_baseline_paula();
	committed_ax = audio_baseline_ax();
	/* The mix limit moves with the card calibration, so the bound is live. */
	ui_set(P, headroom.obj, FUELGAUGE_Max, limit ? limit : 1, TAG_END);
	if (!ok) {
		ui_gauge_set(P, &headroom, 0, "");
		ui_text_set(P, &state, audio_unavailable_reason());
		ui_text_set(P, &meter_note, "Metering needs the audio control plane.");
		return;
	}
	snprintf(level, sizeof(level), "Level %lu of %lu (limit)",
		(unsigned long)weighted, (unsigned long)limit);
	ui_gauge_set(P, &headroom, (LONG)weighted, level);
	if (locked)
		ui_text_set(P, &state,
			"Waiting for the card to confirm the new calibration");
	else if (audio_last_error()[0])
		ui_text_set(P, &state, audio_last_error());
	else
		ui_text_set(P, &state,
			"Changes apply live; Save keeps them after a power-cycle.");
	ui_text_set(P, &meter_note, audio_metering_available() ?
		"Level bars show -60 to 0 dBFS." :
		"Metering is not supported by this firmware.");
}

/* Captures counter baselines each time the visible page opens. */
static void show(void)
{
	memset(&meter_open_out, 0, sizeof(meter_open_out));
	memset(&meter_open_in, 0, sizeof(meter_open_in));
	(void)audio_get_meter(FALSE, &meter_open_out);
	(void)audio_get_meter(TRUE, &meter_open_in);
	sync();
}

/* Polls baseline drags at 10 Hz and refreshes only every third meter tick. */
static void tick(void)
{
	struct audio_meter output;
	struct audio_meter input;
	char limiter[32];
	ULONG limiter_events;
	LONG value;

	if (audio_available() && !audio_calibration_pending()) {
		value = ui_slider_poll(P, &paula);
		if (value != (LONG)committed_paula &&
			audio_set_baseline((UWORD)value, audio_baseline_ax())) {
			committed_paula = (UWORD)value;
			/* Rewriting the dragged slider re-enters ReAction, so only bound AX. */
			ui_slider_set_max(P, &ax, (LONG)audio_baseline_max_ax());
			ui_pending_changed();
		}
		value = ui_slider_poll(P, &ax);
		if (value != (LONG)committed_ax &&
			audio_set_baseline(audio_baseline_paula(), (UWORD)value)) {
			committed_ax = (UWORD)value;
			/* Keep the active AX knob untouched while its peer bound changes. */
			ui_slider_set_max(P, &paula, (LONG)audio_baseline_max_paula());
			ui_pending_changed();
		}
	}
	if (!audio_metering_available() || (ui_ticks() % 3U) != 0)
		return;
	if (!audio_get_meter(FALSE, &output) || !audio_get_meter(TRUE, &input))
		return;
	meter_show(&out_l, output.peak_left);
	meter_show(&out_r, output.peak_right);
	meter_show(&in_l, input.peak_left);
	meter_show(&in_r, input.peak_right);
	limiter_events = meter_delta(output.limiter, meter_open_out.limiter);
	if (limiter_events == 0)
		snprintf(limiter, sizeof(limiter), "none");
	else
		snprintf(limiter, sizeof(limiter), "active (%lu events)",
			(unsigned long)limiter_events);
	ui_text_printf(P, &out_counts, "Output: %lu clips, %lu underruns, limiter %s",
		meter_delta(output.clip, meter_open_out.clip),
		meter_delta(output.underrun, meter_open_out.underrun), limiter);
	ui_text_printf(P, &in_counts, "Input: %lu clips, %lu overruns",
		meter_delta(input.clip, meter_open_in.clip),
		meter_delta(input.overrun, meter_open_in.overrun));
}

/* Commits release values, keeping scene selection visibly truthful on failure. */
static void gadget(UWORD id, UWORD code)
{
	BOOL changed = FALSE;

	if (id == G_RETRY) {
		(void)audio_retry();
		sync();
		return;
	}
	if (!audio_available())
		return;
	if (id == G_SCENE) {
		if (!audio_select_scene(code)) {
			ui_set(P, scene, CHOOSER_Selected, audio_active_scene(), TAG_END);
			ui_status("%s", audio_last_error());
		} else {
			changed = TRUE;
		}
	} else if (id == G_PAULA) {
		changed = audio_set_baseline(code, audio_baseline_ax());
		committed_paula = audio_baseline_paula();
	} else if (id == G_AX) {
		changed = audio_set_baseline(audio_baseline_paula(), code);
		committed_ax = audio_baseline_ax();
	} else if (id == G_BALANCE) {
		changed = audio_balance();
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
struct ui_page page_audio = {
	"Audio",
	"Audio mixer",
	"Scene, levels and meters of the ZZ9000AX mixer.",
	0,
	create,
	show,
	NULL,
	gadget,
	tick,
	sync,
	NULL,
	cleanup,
};
