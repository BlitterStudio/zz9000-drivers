/*
 * ZZTop GadTools audio page: live mixer baseline and card meter display.
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

#define P GT_PAGE_AUDIO

#define G_SCENE   1
#define G_PAULA   2
#define G_AX      3
#define G_BALANCE 4
#define G_RETRY   5

static struct Gadget *scene_gadget;
static struct Gadget *paula_gadget;
static struct Gadget *ax_gadget;
static struct Gadget *balance_gadget;
static struct Gadget *retry_gadget;
static struct Gadget *out_counts_gadget;
static struct Gadget *in_counts_gadget;
static struct Gadget *meter_note_gadget;
static struct Gadget *state_gadget;

static STRPTR scene_labels[AUDIO_SCENE_COUNT + 1];
static char scene_names[AUDIO_SCENE_COUNT][AUDIO_NAME_CHARS + 1];
static struct audio_meter meter_open_out;
static struct audio_meter meter_open_in;
static struct audio_meter output_meter;
static struct audio_meter input_meter;
static UWORD committed_paula;
static UWORD committed_ax;
static char out_counts[96];
static char in_counts[96];
static char meter_note[96];
static char state_text[96];
static char headroom_text[48];
static char output_text[48];
static char input_text[48];
static BOOL meters_changed;

/* Approximates a 16.16 peak on the visible -60..0 dBFS scale. */
static LONG audio_dbfs(ULONG fixed)
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

/* Returns a saturating counter delta after a card-side counter reset. */
static ULONG audio_meter_delta(ULONG current, ULONG opening)
{
	if (current >= opening)
		return current - opening;
	if (current != 0)
		return 1;
	return 0;
}

/* Changes one gadget attribute without redrawing a page that is hidden. */
static void audio_set_attr(struct Gadget *gadget, ULONG tag, ULONG value)
{
	struct TagItem tags[] = {
		{ tag, value },
		{ TAG_DONE, 0 }
	};
	struct Window *win = gt_render_window(gadget);

	if (gadget)
		GT_SetGadgetAttrsA(gadget, win, NULL, tags);
}

/* Updates both the bound and level while the slider is not being dragged. */
static void audio_set_slider(struct Gadget *gadget, UWORD maximum, UWORD level)
{
	struct TagItem tags[] = {
		{ GTSL_Max, maximum },
		{ GTSL_Level, level },
		{ TAG_DONE, 0 }
	};
	struct Window *win = gt_render_window(gadget);

	if (gadget)
		GT_SetGadgetAttrsA(gadget, win, NULL, tags);
}

/* Builds the permanent cycle list around the static scene-name storage. */
static void audio_scene_labels(void)
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

/* Refreshes scene text while retaining the list nodes owned by this page. */
static void audio_scene_labels_refresh(void)
{
	UWORD index;
	const struct audio_scene *value;

	for (index = 0; index < AUDIO_SCENE_COUNT; ++index) {
		value = audio_scene(index);
		snprintf(scene_names[index], sizeof(scene_names[index]), "%s",
			value ? value->name : "Scene");
	}
	audio_set_attr(scene_gadget, GTCY_Labels, (ULONG)scene_labels);
}

/* Fills and labels a framed meter bar. */
static void audio_draw_bar(WORD left, WORD top, WORD width, ULONG level,
	ULONG maximum, const char *text)
{
	struct Window *win;
	struct RastPort *rp;
	WORD fill;

	win = gt_window();
	if (!win)
		return;
	rp = win->RPort;
	if (maximum == 0)
		maximum = 1;
	if (level > maximum)
		level = maximum;
	fill = (WORD)((width - 4) * level / maximum);
	SetAPen(rp, 0);
	RectFill(rp, left, top, (WORD)(left + width - 1), (WORD)(top + gt_layout.row_height - 1));
	SetAPen(rp, 1);
	DrawBevelBox(rp, left, top, width, gt_layout.row_height, GT_VisualInfo,
		(ULONG)gt_visual_info(), TAG_DONE);
	if (fill > 0) {
		SetAPen(rp, 3);
		RectFill(rp, (WORD)(left + 2), (WORD)(top + 2), (WORD)(left + 2 + fill),
			(WORD)(top + gt_layout.row_height - 3));
	}
	SetAPen(rp, 1);
	Move(rp, (WORD)(left + 4), (WORD)(top + gt_layout.font_y - 2));
	Text(rp, (CONST_STRPTR)text, (ULONG)strlen(text));
}

/* Draws the three bars that have no GadTools equivalent. */
static void audio_draw_meters(void)
{
	ULONG boundary;
	ULONG weighted;
	WORD left, width, gap;
	WORD bar_left, bar_width;
	LONG out_left, out_right, in_left, in_right;

	if (gt_current_page() != P || !audio_available())
		return;
	left = gt_layout.page_left;
	gap = gt_layout.font_x;
	width = (WORD)((gt_layout.page_width - gap) / 2);
	boundary = audio_boundary();
	weighted = audio_weighted_level();

	bar_left = (WORD)(left + gt_button_width("Balance") + 12);
	bar_width = (WORD)(gt_layout.page_width - (bar_left - left) - gt_button_width("Retry") - 12);
	snprintf(headroom_text, sizeof(headroom_text), "Level %lu of %lu (limit)",
		(unsigned long)weighted, (unsigned long)boundary);
	audio_draw_bar(bar_left, gt_row(3), bar_width, weighted, boundary ? boundary : 1, headroom_text);

	out_left = audio_dbfs(output_meter.peak_left);
	out_right = audio_dbfs(output_meter.peak_right);
	snprintf(output_text, sizeof(output_text), "Out L %ld dB", (long)out_left);
	audio_draw_bar(left, gt_row(4), width, (ULONG)(out_left + 60), 60, output_text);
	snprintf(output_text, sizeof(output_text), "Out R %ld dB", (long)out_right);
	audio_draw_bar((WORD)(left + width + gap), gt_row(4), width,
		(ULONG)(out_right + 60), 60, output_text);

	in_left = audio_dbfs(input_meter.peak_left);
	in_right = audio_dbfs(input_meter.peak_right);
	snprintf(input_text, sizeof(input_text), "In L %ld dB", (long)in_left);
	audio_draw_bar(left, gt_row(6), width, (ULONG)(in_left + 60), 60, input_text);
	snprintf(input_text, sizeof(input_text), "In R %ld dB", (long)in_right);
	audio_draw_bar((WORD)(left + width + gap), gt_row(6), width,
		(ULONG)(in_right + 60), 60, input_text);
}

/* Creates the compact ten-row audio page. */
static BOOL audio_create(struct gt_page *page)
{
	struct gt_form f;
	static const char *const audio_labels[] = {
		"Active scene",
		"Paula level",
		"AX level",
		NULL
	};
	WORD left, width;
	WORD balance_w, retry_w;

	audio_scene_labels();
	gt_page_begin(page);
	gt_form_init(&f, page, audio_labels);
	left = f.left;
	width = f.width;

	scene_gadget = gt_form_cycle(&f, G_SCENE, 0, "Active scene", (const char *const *)scene_labels);
	paula_gadget = gt_form_slider(&f, G_PAULA, 1, "Paula level", 0, 255, "%ld", "255");
	ax_gadget = gt_form_slider(&f, G_AX, 2, "AX level", 0, 255, "%ld", "255");

	balance_w = gt_button_width("Balance");
	retry_w = gt_button_width("Retry");
	balance_gadget = gt_add_button(page, G_BALANCE, left, 3, balance_w, "Balance");
	retry_gadget = gt_add_button(page, G_RETRY, (WORD)(left + width - retry_w), 3, retry_w, "Retry");

	/* Row 5: Output counts */
	out_counts_gadget = gt_add_note(page, 0, left, 5, width, out_counts);

	/* Row 7: Input counts */
	in_counts_gadget = gt_add_note(page, 0, left, 7, width, in_counts);

	/* Row 8: Meter note */
	meter_note_gadget = gt_add_note(page, 0, left, 8, width, meter_note);

	/* Row 9: State */
	state_gadget = gt_add_note(page, 0, left, 9, width, state_text);

	page->rows = 10;
	return scene_gadget && paula_gadget && ax_gadget && balance_gadget &&
		retry_gadget && out_counts_gadget && in_counts_gadget &&
		meter_note_gadget && state_gadget;
}

/* Synchronizes controls and explanatory text from the shared audio model. */
static void audio_sync(void)
{
	BOOL available = audio_available();
	BOOL locked = audio_calibration_pending();

	gt_page_set_disabled(scene_gadget, !available || locked);
	gt_page_set_disabled(paula_gadget, !available || locked);
	gt_page_set_disabled(ax_gadget, !available || locked);
	gt_page_set_disabled(balance_gadget, !available || locked);
	gt_page_set_disabled(retry_gadget, !audio_retry_available());
	audio_scene_labels_refresh();
	audio_set_attr(scene_gadget, GTCY_Active, audio_active_scene());
	audio_set_slider(paula_gadget, audio_baseline_max_paula(), audio_baseline_paula());
	audio_set_slider(ax_gadget, audio_baseline_max_ax(), audio_baseline_ax());
	committed_paula = audio_baseline_paula();
	committed_ax = audio_baseline_ax();

	if (!available) {
		snprintf(state_text, sizeof(state_text), "%s", audio_unavailable_reason());
		snprintf(meter_note, sizeof(meter_note), "Metering needs audio control plane.");
		out_counts[0] = '\0';
		in_counts[0] = '\0';
	} else if (locked) {
		snprintf(state_text, sizeof(state_text), "Saving scene state to card...");
	} else if (audio_last_error()[0]) {
		snprintf(state_text, sizeof(state_text), "Error: %s", audio_last_error());
	} else {
		snprintf(state_text, sizeof(state_text), "Ready.");
		snprintf(meter_note, sizeof(meter_note), "%s", audio_metering_available() ?
			"Level meters active." : "Metering not supported by firmware.");
	}
	gt_page_set_text(out_counts_gadget, out_counts);
	gt_page_set_text(in_counts_gadget, in_counts);
	gt_page_set_text(meter_note_gadget, meter_note);
	gt_page_set_text(state_gadget, state_text);
	meters_changed = TRUE;
	audio_draw_meters();
}

/* Captures meter counter baselines on every page opening. */
static void audio_show(void)
{
	memset(&meter_open_out, 0, sizeof(meter_open_out));
	memset(&meter_open_in, 0, sizeof(meter_open_in));
	memset(&output_meter, 0, sizeof(output_meter));
	memset(&input_meter, 0, sizeof(input_meter));
	(void)audio_get_meter(FALSE, &meter_open_out);
	(void)audio_get_meter(TRUE, &meter_open_in);
	audio_sync();
}

/* Coalesces baseline drags and refreshes meters every third 10 Hz tick. */
static void audio_page_tick(void)
{
	ULONG limiter_events;
	LONG value;

	if (audio_available() && !audio_calibration_pending()) {
		value = (LONG)gt_page_get(paula_gadget, GTSL_Level);
		if (value != (LONG)committed_paula &&
			audio_set_baseline((UWORD)value, audio_baseline_ax())) {
			committed_paula = (UWORD)value;
			audio_set_attr(ax_gadget, GTSL_Max, audio_baseline_max_ax());
			gt_pending_changed();
		}
		value = (LONG)gt_page_get(ax_gadget, GTSL_Level);
		if (value != (LONG)committed_ax &&
			audio_set_baseline(audio_baseline_paula(), (UWORD)value)) {
			committed_ax = (UWORD)value;
			audio_set_attr(paula_gadget, GTSL_Max, audio_baseline_max_paula());
			gt_pending_changed();
		}
	}
	if (gt_ticks() % 3 != 0 || !audio_metering_available())
		return;
	(void)audio_get_meter(FALSE, &output_meter);
	(void)audio_get_meter(TRUE, &input_meter);
	limiter_events = audio_meter_delta(output_meter.limiter, meter_open_out.limiter);
	if (limiter_events == 0) {
		snprintf(out_counts, sizeof(out_counts), "Out: %lu clips, %lu underruns, limiter off",
			(unsigned long)audio_meter_delta(output_meter.clip, meter_open_out.clip),
			(unsigned long)audio_meter_delta(output_meter.underrun, meter_open_out.underrun));
	} else {
		snprintf(out_counts, sizeof(out_counts), "Out: %lu clips, %lu underruns, limit (%lu)",
			(unsigned long)audio_meter_delta(output_meter.clip, meter_open_out.clip),
			(unsigned long)audio_meter_delta(output_meter.underrun, meter_open_out.underrun),
			(unsigned long)limiter_events);
	}
	snprintf(in_counts, sizeof(in_counts), "In: %lu clips, %lu overruns",
		(unsigned long)audio_meter_delta(input_meter.clip, meter_open_in.clip),
		(unsigned long)audio_meter_delta(input_meter.overrun, meter_open_in.overrun));
	gt_page_set_text(out_counts_gadget, out_counts);
	gt_page_set_text(in_counts_gadget, in_counts);
	meters_changed = TRUE;
	audio_draw_meters();
}

/* Commits a released control and leaves the selection truthful on failure. */
static void audio_gadget(UWORD id, UWORD code)
{
	BOOL changed = FALSE;

	if (id == G_RETRY) {
		(void)audio_retry();
		audio_sync();
		return;
	}
	if (!audio_available() || audio_calibration_pending())
		return;
	if (id == G_SCENE) {
		if (code < AUDIO_SCENE_COUNT && audio_select_scene(code))
			changed = TRUE;
		else
			audio_set_attr(scene_gadget, GTCY_Active, audio_active_scene());
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
		gt_pending_changed();
	audio_sync();
}

/* Clear page-owned gadget references after their list is freed. */
static void audio_cleanup(void)
{
	scene_gadget = NULL;
	paula_gadget = NULL;
	ax_gadget = NULL;
	balance_gadget = NULL;
	retry_gadget = NULL;
	out_counts_gadget = NULL;
	in_counts_gadget = NULL;
	meter_note_gadget = NULL;
	state_gadget = NULL;
}

struct gt_page gt_page_audio = {
	"Audio",
	"Audio mixer",
	"Scene, levels and meters of the ZZ9000AX mixer.",
	0,
	NULL,
	NULL,
	10,
	audio_create,
	audio_show,
	NULL,
	audio_gadget,
	audio_page_tick,
	audio_sync,
	NULL,
	audio_cleanup
};
