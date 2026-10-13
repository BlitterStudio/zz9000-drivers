/*
 * ZZTop Capture page: advanced live native-video calibration controls.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <exec/types.h>
#include <intuition/classusr.h>
#include <gadgets/button.h>
#include <gadgets/checkbox.h>
#include <gadgets/chooser.h>
#include <gadgets/integer.h>
#include <gadgets/layout.h>
#include <gadgets/space.h>
#include <images/bevel.h>
#include <images/label.h>
#include <reaction/reaction_macros.h>
#include <proto/button.h>
#include <proto/checkbox.h>
#include <proto/chooser.h>
#include <proto/integer.h>
#include <proto/intuition.h>
#include <proto/label.h>
#include <proto/layout.h>
#include <proto/space.h>
#include <stdio.h>

#include "settings.h"
#include "ui.h"
#include "video.h"

#define P PAGE_CAPTURE

enum {
	G_SAMPLE = 1, G_PHASE, G_PHASE_MINUS_28, G_PHASE_MINUS_1,
	G_PHASE_PLUS_1, G_PHASE_PLUS_28, G_CROP_H, G_CROP_H_AUTO, G_CROP_V,
	G_CROP_V_AUTO, G_WIDTH, G_WIDTH_AUTO, G_HEIGHT, G_HEIGHT_AUTO,
	G_AUTO, G_ALIGN, G_FIND
};

static const char *const sample_names[] = {
	"Average (recommended)", "Even fields (diagnostic)",
	"Odd fields (diagnostic)", NULL
};
static Object *sample_obj, *phase_obj, *phase_label_obj;
static Object *phase_minus_28_obj, *phase_minus_1_obj;
static Object *phase_plus_1_obj, *phase_plus_28_obj;
static Object *crop_h_obj, *crop_v_obj, *width_obj, *height_obj;
static Object *crop_h_auto, *crop_v_auto, *width_auto, *height_auto;
static Object *auto_obj, *align_obj, *find_obj;
static struct List *sample_list;
static struct ui_text sweep, applied, legend, gate;

/* Disable an edit field while its matching Automatic checkbox is selected. */
static void enable_value(Object *obj, BOOL automatic)
{
	ui_disable(P, obj, automatic);
}

/* Read an integer gadget's current signed value. */
static LONG field_value(Object *obj)
{
	return (LONG)ui_get(obj, INTEGER_Number);
}

/* Update the dynamic phase label for the capture engine currently detected. */
static void sync_phase_label(void)
{
	/* Labels are OM_NEW-only on this ReAction class. video_init has already
	 * detected the domain before create() chooses the static label. */
}

/* Refresh live controls, their applied values, and the single gate note. */
static void sync_live(void)
{
	struct zz_vcap_geometry_state state;
	struct zz_vcap_snapshot snap;
	char phase_reason[UI_TEXT_MAX];
	char geometry_reason[UI_TEXT_MAX];
	char action_reason[UI_TEXT_MAX];
	int phase;
	BOOL phase_ready;
	BOOL geometry_ready;
	BOOL snapshot_ready;
	BOOL h_auto = !settings_present(&cfg.edit, CFG_CROP_H);
	BOOL v_auto = !settings_present(&cfg.edit, CFG_CROP_V);
	BOOL w_auto = !settings_present(&cfg.edit, CFG_WIDTH);
	BOOL ht_auto = !settings_present(&cfg.edit, CFG_HEIGHT);
	BOOL c28;

	sync_phase_label();
	phase_ready = video_phase_state(&phase, phase_reason, sizeof(phase_reason));
	c28 = video_phase_domain() == ZZ_VCAP_PHASE_C28;
	if (phase_ready) {
		ui_set(P, phase_obj, INTEGER_Minimum, c28 ? -896 : -255,
			INTEGER_Maximum, c28 ? 895 : 255, INTEGER_Number, (ULONG)phase,
			TAG_END);
	}
	ui_disable(P, phase_obj, !phase_ready);
	ui_disable(P, phase_minus_28_obj, !phase_ready);
	ui_disable(P, phase_minus_1_obj, !phase_ready);
	ui_disable(P, phase_plus_1_obj, !phase_ready);
	ui_disable(P, phase_plus_28_obj, !phase_ready);

	ui_set(P, crop_h_auto, CHECKBOX_Checked, h_auto, TAG_END);
	ui_set(P, crop_v_auto, CHECKBOX_Checked, v_auto, TAG_END);
	ui_set(P, width_auto, CHECKBOX_Checked, w_auto, TAG_END);
	ui_set(P, height_auto, CHECKBOX_Checked, ht_auto, TAG_END);
	enable_value(crop_h_obj, h_auto);
	enable_value(crop_v_obj, v_auto);
	enable_value(width_obj, w_auto);
	enable_value(height_obj, ht_auto);
	if (video_live_supported() && !video_current_path_matches()) {
		ui_disable(P, crop_h_obj, TRUE);
		ui_disable(P, crop_v_obj, TRUE);
		ui_disable(P, crop_h_auto, TRUE);
		ui_disable(P, crop_v_auto, TRUE);
	}
	ui_set(P, crop_h_obj, INTEGER_Number, (ULONG)cfg.edit.videocap_crop_h, TAG_END);
	ui_set(P, crop_v_obj, INTEGER_Number, (ULONG)cfg.edit.videocap_crop_v, TAG_END);
	ui_set(P, width_obj, INTEGER_Number, (ULONG)cfg.edit.videocap_width, TAG_END);
	ui_set(P, height_obj, INTEGER_Number, (ULONG)cfg.edit.videocap_height, TAG_END);

	snapshot_ready = video_snapshot(&snap);
	geometry_ready = video_geometry_state(&state, geometry_reason,
		sizeof(geometry_reason));
	if (snapshot_ready && geometry_ready) {
		ui_text_printf(P, &applied, "Crop %u x %u; window %u x %u%s",
			(unsigned)snap.effective_h, (unsigned)snap.effective_v,
			(unsigned)state.applied_width, (unsigned)state.applied_height,
			(state.status & ZZ_VCAP_GEOMETRY_STATUS_PENDING) ? " (pending)" : "");
	} else if (snapshot_ready) {
		ui_text_printf(P, &applied, "Crop %u x %u; %s",
			(unsigned)snap.effective_h, (unsigned)snap.effective_v,
			geometry_reason);
	} else if (geometry_ready) {
		ui_text_printf(P, &applied, "Crop readout unavailable; window %u x %u%s",
			(unsigned)state.applied_width, (unsigned)state.applied_height,
			(state.status & ZZ_VCAP_GEOMETRY_STATUS_PENDING) ? " (pending)" : "");
	} else {
		ui_text_set(P, &applied, geometry_reason);
	}
	video_sweep_text(action_reason, sizeof(action_reason));
	ui_text_set(P, &sweep, action_reason);

	if (!video_live_supported()) {
		ui_text_set(P, &gate, video_unavailable_reason());
		ui_disable(P, align_obj, TRUE);
		ui_disable(P, find_obj, TRUE);
		ui_disable(P, auto_obj, TRUE);
	} else if (!phase_ready) {
		if (!video_geometry_supported()) {
			ui_text_printf(P, &gate, "%s Window and reset need firmware confirmation.",
				phase_reason);
		} else {
			ui_text_set(P, &gate, phase_reason);
		}
		ui_disable(P, align_obj, !video_align_ready(action_reason,
			sizeof(action_reason)));
		ui_disable(P, find_obj, TRUE);
		ui_disable(P, auto_obj, !video_geometry_supported());
	} else if (!video_geometry_supported()) {
		ui_text_set(P, &gate,
			"Capture window and Reset need firmware that confirms capture window changes.");
		ui_disable(P, auto_obj, TRUE);
		ui_disable(P, align_obj, !video_align_ready(action_reason,
			sizeof(action_reason)));
		ui_disable(P, find_obj, !video_find_phase_ready(action_reason,
			sizeof(action_reason)));
	} else {
		if (!video_align_ready(action_reason, sizeof(action_reason))) {
			ui_text_set(P, &gate, action_reason);
		} else if (!video_find_phase_ready(action_reason, sizeof(action_reason))) {
			ui_text_set(P, &gate, action_reason);
		} else {
			ui_text_set(P, &gate,
				"Live values apply now; Save keeps them after reboot.");
		}
		ui_disable(P, align_obj, !video_align_ready(action_reason,
			sizeof(action_reason)));
		ui_disable(P, find_obj, !video_find_phase_ready(action_reason,
			sizeof(action_reason)));
		ui_disable(P, auto_obj, FALSE);
	}
	if (!cfg.supported) {
		ui_disable(P, sample_obj, TRUE);
		ui_disable(P, phase_obj, TRUE);
		ui_disable(P, phase_minus_28_obj, TRUE);
		ui_disable(P, phase_minus_1_obj, TRUE);
		ui_disable(P, phase_plus_1_obj, TRUE);
		ui_disable(P, phase_plus_28_obj, TRUE);
		ui_disable(P, crop_h_obj, TRUE);
		ui_disable(P, crop_v_obj, TRUE);
		ui_disable(P, crop_h_auto, TRUE);
		ui_disable(P, crop_v_auto, TRUE);
		ui_disable(P, width_obj, TRUE);
		ui_disable(P, height_obj, TRUE);
		ui_disable(P, width_auto, TRUE);
		ui_disable(P, height_auto, TRUE);
		ui_disable(P, auto_obj, TRUE);
		ui_disable(P, align_obj, TRUE);
		ui_disable(P, find_obj, TRUE);
		ui_text_set(P, &gate, "Capture settings need firmware 2.3 or newer.");
	}
}

/* Create an integer edit control with a page-local gadget id. */
static Object *integer(Object **out, UWORD id, LONG min, LONG max)
{
	*out = IntegerObject, GA_ID, UI_GID(P, id), GA_RelVerify, TRUE,
		INTEGER_Minimum, min, INTEGER_Maximum, max, End;
	return *out;
}

/* Create a compact Automatic checkbox with a page-local gadget id. */
static Object *automatic(Object **out, UWORD id)
{
	*out = CheckBoxObject, GA_ID, UI_GID(P, id), GA_RelVerify, TRUE,
		GA_Text, "Automatic", End;
	return *out;
}

/* Create one of the phase stepping buttons. */
static Object *phase_button(UWORD id, const char *text)
{
	return NewObject(BUTTON_GetClass(), NULL, GA_ID, UI_GID(P, id),
		GA_RelVerify, TRUE, GA_Text, text, End;
}

/* Create the compact Sampling and Framing groups. */
static Object *create(void)
{
	sample_list = ui_chooser_labels(sample_names);
	sample_obj = ChooserObject, GA_ID, UI_GID(P, G_SAMPLE), GA_RelVerify, TRUE,
		CHOOSER_Labels, sample_list, End;
	phase_label_obj = ui_label(video_phase_domain() == ZZ_VCAP_PHASE_C28 ?
		"Phase (C28 steps)" : video_phase_domain() == ZZ_VCAP_PHASE_E7M ?
		"Phase (E7M steps)" : "Phase (no engine)");
	integer(&phase_obj, G_PHASE, -896, 895);
	phase_minus_28_obj = phase_button(G_PHASE_MINUS_28, "-28");
	phase_minus_1_obj = phase_button(G_PHASE_MINUS_1, "-1");
	phase_plus_1_obj = phase_button(G_PHASE_PLUS_1, "+1");
	phase_plus_28_obj = phase_button(G_PHASE_PLUS_28, "+28");
	integer(&crop_h_obj, G_CROP_H, 0, 4095);
	integer(&crop_v_obj, G_CROP_V, 0, 4095);
	integer(&width_obj, G_WIDTH, 256, 1280);
	/* Width permits typed multiples of 16 only; do not offer invalid one-step arrows. */
	ui_set(P, width_obj, INTEGER_Arrows, FALSE, TAG_END);
	integer(&height_obj, G_HEIGHT, 100, 1024);
	automatic(&crop_h_auto, G_CROP_H_AUTO);
	automatic(&crop_v_auto, G_CROP_V_AUTO);
	automatic(&width_auto, G_WIDTH_AUTO);
	automatic(&height_auto, G_HEIGHT_AUTO);
	auto_obj = NewObject(BUTTON_GetClass(), NULL, GA_ID, UI_GID(P, G_AUTO),
		GA_RelVerify, TRUE, GA_Text, "Reset to _automatic", End;
	align_obj = NewObject(BUTTON_GetClass(), NULL, GA_ID, UI_GID(P, G_ALIGN),
		GA_RelVerify, TRUE, GA_Text, "_Align picture...", End;
	find_obj = NewObject(BUTTON_GetClass(), NULL, GA_ID, UI_GID(P, G_FIND),
		GA_RelVerify, TRUE, GA_Text, "_Find best phase...", End;
	return VGroupObject, LAYOUT_SpaceOuter, TRUE,
		LAYOUT_AddChild, VGroupObject, LAYOUT_BevelStyle, BVS_GROUP,
			LAYOUT_Label, "Sampling",
			LAYOUT_AddChild, sample_obj, Label("Sample mode (boot-time)"),
			LAYOUT_AddChild, HGroupObject,
				LAYOUT_AddChild, phase_obj, CHILD_Label, phase_label_obj,
				LAYOUT_AddChild, phase_minus_28_obj, CHILD_WeightedWidth, 0,
				LAYOUT_AddChild, phase_minus_1_obj, CHILD_WeightedWidth, 0,
				LAYOUT_AddChild, phase_plus_1_obj, CHILD_WeightedWidth, 0,
				LAYOUT_AddChild, phase_plus_28_obj, CHILD_WeightedWidth, 0,
			End,
			LAYOUT_AddChild, ui_text_new(&sweep, "Run Find best phase..."),
				Label("Last phase sweep"),
			LAYOUT_AddChild, ui_note_new(&legend,
				"Legend: = clean, + mixed, . dirty, | selected."),
			LAYOUT_AddChild, ui_note_new(&gate, ""),
		End,
		LAYOUT_AddChild, VGroupObject, LAYOUT_BevelStyle, BVS_GROUP,
			LAYOUT_Label, "Framing",
			LAYOUT_AddChild, HGroupObject,
				LAYOUT_AddChild, crop_h_obj,
				LAYOUT_AddChild, crop_h_auto, CHILD_WeightedWidth, 0,
			End, Label("Crop H (samples)"),
			LAYOUT_AddChild, HGroupObject,
				LAYOUT_AddChild, crop_v_obj,
				LAYOUT_AddChild, crop_v_auto, CHILD_WeightedWidth, 0,
			End, Label("Crop V (lines)"),
			LAYOUT_AddChild, HGroupObject,
				LAYOUT_AddChild, width_obj,
				LAYOUT_AddChild, width_auto, CHILD_WeightedWidth, 0,
			End, Label("Capture width"),
			LAYOUT_AddChild, HGroupObject,
				LAYOUT_AddChild, height_obj,
				LAYOUT_AddChild, height_auto, CHILD_WeightedWidth, 0,
			End, Label("Capture height"),
			LAYOUT_AddChild, ui_text_new(&applied, ""), Label("Applied"),
		End,
		LAYOUT_AddChild, HGroupObject,
			LAYOUT_AddChild, auto_obj,
			LAYOUT_AddChild, align_obj,
			LAYOUT_AddChild, find_obj,
		End,
		LAYOUT_AddChild, SpaceObject, End,
	End;
}

/* Free this page's chooser labels after its gadget tree is gone. */
static void cleanup(void)
{
	ui_chooser_free_labels(sample_list);
	sample_list = NULL;
}

/* Synchronize the page after model changes. */
static void sync(void)
{
	ui_set(P, sample_obj, CHOOSER_Selected, (ULONG)cfg.edit.videocap_sample,
		TAG_END);
	sync_live();
}

/* Apply the two independent crop values and their Automatic selections. */
static BOOL change_crop(char *msg)
{
	return video_set_crop((UWORD)field_value(crop_h_obj),
		!ui_get(crop_h_auto, CHECKBOX_Checked), (UWORD)field_value(crop_v_obj),
		!ui_get(crop_v_auto, CHECKBOX_Checked), msg, UI_TEXT_MAX);
}

/* Apply the two independent capture-window values and Automatic selections. */
static BOOL change_geometry(char *msg)
{
	return video_set_geometry(ui_get(width_auto, CHECKBOX_Checked) ? 0 :
		(UWORD)field_value(width_obj), ui_get(height_auto, CHECKBOX_Checked) ? 0 :
		(UWORD)field_value(height_obj), msg, UI_TEXT_MAX);
}

/* Dispatch a page gadget, preserving real failures for the status requester. */
static void gadget(UWORD id, UWORD code)
{
	char msg[UI_TEXT_MAX];
	int phase;
	int delta = 0;
	BOOL ok = TRUE;

	switch (id) {
	case G_SAMPLE:
		if (code < 3)
			ok = video_set_sample(code, msg, sizeof(msg));
		break;
	case G_PHASE:
		ok = video_set_phase((int)field_value(phase_obj), TRUE, msg, sizeof(msg));
		break;
	case G_PHASE_MINUS_28:
		delta = -28;
		break;
	case G_PHASE_MINUS_1:
		delta = -1;
		break;
	case G_PHASE_PLUS_1:
		delta = 1;
		break;
	case G_PHASE_PLUS_28:
		delta = 28;
		break;
	case G_CROP_H:
	case G_CROP_V:
	case G_CROP_H_AUTO:
	case G_CROP_V_AUTO:
		ok = change_crop(msg);
		break;
	case G_WIDTH:
	case G_HEIGHT:
	case G_WIDTH_AUTO:
	case G_HEIGHT_AUTO:
		ok = change_geometry(msg);
		break;
	case G_AUTO:
		ok = video_reset_automatic(msg, sizeof(msg));
		break;
	case G_ALIGN:
		ok = video_align(msg, sizeof(msg));
		break;
	case G_FIND:
		ok = video_find_phase(msg, sizeof(msg));
		break;
	default:
		return;
	}
	if (delta) {
		if (!video_phase_state(&phase, msg, sizeof(msg))) {
			ok = FALSE;
		} else {
			ok = video_set_phase(zz_vcap_phase_step(phase, delta,
				video_phase_domain()), TRUE, msg, sizeof(msg));
		}
	}
	if (ok)
		ui_status("%s", msg);
	else
		ui_error("%s", msg);
	ui_pending_changed();
	sync();
}

/* Poll live capture state at two hertz while the page is visible. */
static void tick(void)
{
	if (ui_ticks() % 5 == 0) {
		/* Live readouts refresh below; editor values stay untouched until sync. */
		struct zz_vcap_snapshot snap;
		struct zz_vcap_geometry_state state;
		char msg[UI_TEXT_MAX];

		if (video_snapshot(&snap) && video_geometry_state(&state, msg, sizeof(msg)))
			ui_text_printf(P, &applied, "Crop %u x %u; window %u x %u",
				(unsigned)snap.effective_h, (unsigned)snap.effective_v,
				(unsigned)state.applied_width, (unsigned)state.applied_height);
		video_sweep_text(msg, sizeof(msg));
		ui_text_set(P, &sweep, msg);
	}
}

/* Commit integer text that has not yet generated a gadget event. */
static void flush(void)
{
	char msg[UI_TEXT_MAX];
	BOOL changed = FALSE;
	BOOL h_auto;
	BOOL v_auto;
	BOOL w_auto;
	BOOL ht_auto;

	if (!cfg.supported)
		return;
	h_auto = ui_get(crop_h_auto, CHECKBOX_Checked) != 0;
	v_auto = ui_get(crop_v_auto, CHECKBOX_Checked) != 0;
	w_auto = ui_get(width_auto, CHECKBOX_Checked) != 0;
	ht_auto = ui_get(height_auto, CHECKBOX_Checked) != 0;
	if (h_auto == settings_present(&cfg.edit, CFG_CROP_H) ||
		v_auto == settings_present(&cfg.edit, CFG_CROP_V) ||
		(!h_auto && (UWORD)field_value(crop_h_obj) != cfg.edit.videocap_crop_h) ||
		(!v_auto && (UWORD)field_value(crop_v_obj) != cfg.edit.videocap_crop_v)) {
		if (change_crop(msg))
			changed = TRUE;
		else
			ui_error("%s", msg);
	}
	if (w_auto == settings_present(&cfg.edit, CFG_WIDTH) ||
		ht_auto == settings_present(&cfg.edit, CFG_HEIGHT) ||
		(!w_auto && (UWORD)field_value(width_obj) != cfg.edit.videocap_width) ||
		(!ht_auto && (UWORD)field_value(height_obj) != cfg.edit.videocap_height)) {
		if (change_geometry(msg))
			changed = TRUE;
		else
			ui_error("%s", msg);
	}
	if (changed)
		ui_pending_changed();
}

struct ui_page page_capture = {
	"Capture", "Capture calibration", "Sampling phase, crop and capture window values.",
	UI_PAGE_ADVANCED | UI_PAGE_CHILD,
	create,
	NULL,
	flush,      /* leaving the page commits a typed value */
	gadget,
	tick,
	sync,
	flush,
	cleanup,
};
