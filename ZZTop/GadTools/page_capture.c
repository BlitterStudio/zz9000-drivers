/*
 * ZZTop Capture page: advanced live native-video calibration controls.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <exec/types.h>
#include <gadgets/string.h>
#include <intuition/gadgetclass.h>
#include <intuition/intuition.h>
#include <libraries/gadtools.h>
#include <proto/gadtools.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>

#include "gt.h"
#include "settings.h"
#include "video.h"

#define P GT_PAGE_CAPTURE
#define GT_TEXT_MAX 128

enum {
	G_SAMPLE = 1,
	G_PHASE,
	G_PHASE_MINUS_28,
	G_PHASE_MINUS_1,
	G_PHASE_PLUS_1,
	G_PHASE_PLUS_28,
	G_CROP_H,
	G_CROP_H_AUTO,
	G_CROP_V,
	G_CROP_V_AUTO,
	G_WIDTH,
	G_WIDTH_AUTO,
	G_HEIGHT,
	G_HEIGHT_AUTO,
	G_AUTO,
	G_ALIGN,
	G_FIND,
	G_SWEEP,
	G_LEGEND,
	G_GATE,
	G_APPLIED
};

static STRPTR sample_names[] = {
	(STRPTR)"Average (recommended)",
	(STRPTR)"Even fields (diagnostic)",
	(STRPTR)"Odd fields (diagnostic)",
	NULL
};

static struct Gadget *sample_gadget;
static struct Gadget *phase_gadget;
static struct Gadget *phase_minus_28_gadget;
static struct Gadget *phase_minus_1_gadget;
static struct Gadget *phase_plus_1_gadget;
static struct Gadget *phase_plus_28_gadget;
static struct Gadget *crop_h_gadget;
static struct Gadget *crop_h_auto_gadget;
static struct Gadget *crop_v_gadget;
static struct Gadget *crop_v_auto_gadget;
static struct Gadget *width_gadget;
static struct Gadget *width_auto_gadget;
static struct Gadget *height_gadget;
static struct Gadget *height_auto_gadget;
static struct Gadget *auto_gadget;
static struct Gadget *align_gadget;
static struct Gadget *find_gadget;
static struct Gadget *sweep_gadget;
static struct Gadget *legend_gadget;
static struct Gadget *gate_gadget;
static struct Gadget *applied_gadget;

static char phase_buffer[6];
static char crop_h_buffer[6];
static char crop_v_buffer[6];
static char width_buffer[6];
static char height_buffer[6];
static char sweep_buffer[GT_TEXT_MAX];
static char gate_buffer[GT_TEXT_MAX];
static char applied_buffer[GT_TEXT_MAX];
static char message_buffer[GT_TEXT_MAX];
static char phase_reason[GT_TEXT_MAX];
static char geometry_reason[GT_TEXT_MAX];
static char action_reason[GT_TEXT_MAX];
static struct zz_vcap_snapshot capture_snapshot;
static struct zz_vcap_geometry_state geometry_state;
static struct TagItem phase_tags[] = {
	{ GTST_MaxChars, 6 },
	{ GTST_String, (ULONG)phase_buffer },
	{ TAG_DONE, 0 }
};
static struct TagItem crop_h_tags[] = {
	{ GTST_MaxChars, 6 },
	{ GTST_String, (ULONG)crop_h_buffer },
	{ TAG_DONE, 0 }
};
static struct TagItem crop_v_tags[] = {
	{ GTST_MaxChars, 6 },
	{ GTST_String, (ULONG)crop_v_buffer },
	{ TAG_DONE, 0 }
};
static struct TagItem width_tags[] = {
	{ GTST_MaxChars, 6 },
	{ GTST_String, (ULONG)width_buffer },
	{ TAG_DONE, 0 }
};
static struct TagItem height_tags[] = {
	{ GTST_MaxChars, 6 },
	{ GTST_String, (ULONG)height_buffer },
	{ TAG_DONE, 0 }
};
static struct TagItem set_tags[] = {
	{ TAG_IGNORE, 0 },
	{ TAG_DONE, 0 }
};

/* Initialize a GadTools descriptor in the page's window-relative space. */
static void new_gadget(struct NewGadget *ng, UWORD local, WORD left, UWORD row,
	WORD width, const char *text, UWORD flags)
{
	ng->ng_LeftEdge = left;
	ng->ng_TopEdge = gt_row(row);
	ng->ng_Width = width;
	ng->ng_Height = gt_layout.row_height;
	ng->ng_GadgetText = (STRPTR)text;
	ng->ng_TextAttr = (struct TextAttr *)gt_layout.font;
	ng->ng_GadgetID = GT_GID(P, local);
	ng->ng_Flags = flags;
	ng->ng_VisualInfo = NULL;
	ng->ng_UserData = NULL;
}

/* Add one gadget with a temporary descriptor that GadTools copies. */
static struct Gadget *add_gadget(struct gt_page *page, ULONG kind, UWORD local,
	WORD left, UWORD row, WORD width, const char *text, UWORD flags,
	struct TagItem *tags)
{
	struct NewGadget ng;

	new_gadget(&ng, local, left, row, width, text, flags);
	return gt_page_add(page, kind, &ng, tags);
}

/* Set one GadTools attribute through a real two-item TagItem array. */
static void set_attr(struct Gadget *gadget, ULONG tag, ULONG value)
{
	if (!gadget)
		return;
	set_tags[0].ti_Tag = tag;
	set_tags[0].ti_Data = value;
	GT_SetGadgetAttrsA(gadget, gt_render_window(gadget), NULL, set_tags);
}

/* Set an integer field's text without retaining a caller-owned pointer. */
static void set_field(struct Gadget *gadget, char *buffer, UWORD size, LONG value)
{
	snprintf(buffer, size, "%d", (int)value);
	if (!gadget)
		return;
	GT_SetGadgetAttrs(gadget, gt_render_window(gadget), NULL, GTST_String, (ULONG)buffer,
		TAG_END);
}

/* Return a GadTools string gadget's editable buffer. */
static const char *field_text(struct Gadget *gadget)
{
	struct StringInfo *info;

	if (!gadget)
		return "";
	info = (struct StringInfo *)gadget->SpecialInfo;
	if (!info || !info->Buffer)
		return "";
	return (const char *)info->Buffer;
}

/* Parse a complete signed decimal entry from one GadTools string field. */
static BOOL field_value(struct Gadget *gadget, LONG *value)
{
	const char *text;
	char *end;
	long parsed;

	text = field_text(gadget);
	if (!text || !text[0])
		return FALSE;
	parsed = strtol(text, &end, 10);
	if (end == text)
		return FALSE;
	while (*end && isspace((unsigned char)*end))
		end++;
	if (*end)
		return FALSE;
	*value = (LONG)parsed;
	return TRUE;
}

/* Set the paired Automatic checkbox state. */
static void set_checked(struct Gadget *gadget, BOOL checked)
{
	set_attr(gadget, GTCB_Checked, checked);
}

/* Update the applied crop/window description from the live capture engine. */
static void refresh_applied(void)
{
	BOOL snapshot_ready;
	BOOL geometry_ready;

	snapshot_ready = video_snapshot(&capture_snapshot);
	geometry_ready = video_geometry_state(&geometry_state, geometry_reason,
		sizeof(geometry_reason));
	if (snapshot_ready && geometry_ready) {
		snprintf(applied_buffer, sizeof(applied_buffer),
			"Crop %ux%u; window %ux%u%s",
			(unsigned)capture_snapshot.effective_h,
			(unsigned)capture_snapshot.effective_v,
			(unsigned)geometry_state.applied_width,
			(unsigned)geometry_state.applied_height,
			(geometry_state.status & ZZ_VCAP_GEOMETRY_STATUS_PENDING) ?
			" (pending)" : "");
	} else if (snapshot_ready) {
		snprintf(applied_buffer, sizeof(applied_buffer), "Crop %ux%u; %s",
			(unsigned)capture_snapshot.effective_h,
			(unsigned)capture_snapshot.effective_v,
			strstr(geometry_reason, "unsupported") ? "window unsupported" : geometry_reason);
	} else if (geometry_ready) {
		snprintf(applied_buffer, sizeof(applied_buffer),
			"Window %ux%u%s",
			(unsigned)geometry_state.applied_width,
			(unsigned)geometry_state.applied_height,
			(geometry_state.status & ZZ_VCAP_GEOMETRY_STATUS_PENDING) ?
			" (pending)" : "");
	} else {
		snprintf(applied_buffer, sizeof(applied_buffer), "%s",
			strstr(geometry_reason, "unsupported") ? "Window unsupported" : geometry_reason);
	}
	gt_page_set_text(applied_gadget, applied_buffer);
}

/* Refresh controls, readouts and the explanation for any disabled action. */
static void sync_live(void)
{
	int phase;
	BOOL phase_ready;
	BOOL h_auto;
	BOOL v_auto;
	BOOL w_auto;
	BOOL ht_auto;

	h_auto = !settings_present(&cfg.edit, CFG_CROP_H);
	v_auto = !settings_present(&cfg.edit, CFG_CROP_V);
	w_auto = !settings_present(&cfg.edit, CFG_WIDTH);
	ht_auto = !settings_present(&cfg.edit, CFG_HEIGHT);
	phase_ready = video_phase_state(&phase, phase_reason, sizeof(phase_reason));
	if (phase_ready)
		set_field(phase_gadget, phase_buffer, sizeof(phase_buffer), (LONG)phase);
	gt_page_set_disabled(phase_gadget, !phase_ready);
	gt_page_set_disabled(phase_minus_28_gadget, !phase_ready);
	gt_page_set_disabled(phase_minus_1_gadget, !phase_ready);
	gt_page_set_disabled(phase_plus_1_gadget, !phase_ready);
	gt_page_set_disabled(phase_plus_28_gadget, !phase_ready);

	set_checked(crop_h_auto_gadget, h_auto);
	set_checked(crop_v_auto_gadget, v_auto);
	set_checked(width_auto_gadget, w_auto);
	set_checked(height_auto_gadget, ht_auto);
	gt_page_set_disabled(crop_h_gadget, h_auto);
	gt_page_set_disabled(crop_v_gadget, v_auto);
	gt_page_set_disabled(width_gadget, w_auto);
	gt_page_set_disabled(height_gadget, ht_auto);
	if (video_live_supported() && !video_current_path_matches()) {
		gt_page_set_disabled(crop_h_gadget, TRUE);
		gt_page_set_disabled(crop_v_gadget, TRUE);
		gt_page_set_disabled(crop_h_auto_gadget, TRUE);
		gt_page_set_disabled(crop_v_auto_gadget, TRUE);
	} else {
		gt_page_set_disabled(crop_h_auto_gadget, FALSE);
		gt_page_set_disabled(crop_v_auto_gadget, FALSE);
	}
	set_field(crop_h_gadget, crop_h_buffer, sizeof(crop_h_buffer),
		(LONG)cfg.edit.videocap_crop_h);
	set_field(crop_v_gadget, crop_v_buffer, sizeof(crop_v_buffer),
		(LONG)cfg.edit.videocap_crop_v);
	set_field(width_gadget, width_buffer, sizeof(width_buffer),
		(LONG)cfg.edit.videocap_width);
	set_field(height_gadget, height_buffer, sizeof(height_buffer),
		(LONG)cfg.edit.videocap_height);
	refresh_applied();
	video_sweep_text(sweep_buffer, sizeof(sweep_buffer));
	gt_page_set_text(sweep_gadget, sweep_buffer);

	if (!video_live_supported()) {
		snprintf(gate_buffer, sizeof(gate_buffer), "%s", video_unavailable_reason());
		gt_page_set_disabled(align_gadget, TRUE);
		gt_page_set_disabled(find_gadget, TRUE);
		gt_page_set_disabled(auto_gadget, TRUE);
	} else if (!phase_ready) {
		if (!video_geometry_supported()) {
			snprintf(gate_buffer, sizeof(gate_buffer),
				"%s Window and reset need firmware confirmation.", phase_reason);
		} else {
			snprintf(gate_buffer, sizeof(gate_buffer), "%s", phase_reason);
		}
		gt_page_set_disabled(align_gadget,
			!video_align_ready(action_reason, sizeof(action_reason)));
		gt_page_set_disabled(find_gadget, TRUE);
		gt_page_set_disabled(auto_gadget, !video_geometry_supported());
	} else if (!video_geometry_supported()) {
		snprintf(gate_buffer, sizeof(gate_buffer),
			"Window and Reset need FW confirmation.");
		gt_page_set_disabled(align_gadget,
			!video_align_ready(action_reason, sizeof(action_reason)));
		gt_page_set_disabled(find_gadget,
			!video_find_phase_ready(action_reason, sizeof(action_reason)));
	} else {
		if (!video_align_ready(action_reason, sizeof(action_reason)))
			snprintf(gate_buffer, sizeof(gate_buffer), "%s", action_reason);
		else if (!video_find_phase_ready(action_reason, sizeof(action_reason)))
			snprintf(gate_buffer, sizeof(gate_buffer), "%s", action_reason);
		else
			snprintf(gate_buffer, sizeof(gate_buffer),
				"Live values apply now; Save keeps after reboot.");
		gt_page_set_disabled(align_gadget,
			!video_align_ready(action_reason, sizeof(action_reason)));
		gt_page_set_disabled(find_gadget,
			!video_find_phase_ready(action_reason, sizeof(action_reason)));
		gt_page_set_disabled(auto_gadget, FALSE);
	}
	if (!cfg.supported) {
		gt_page_set_disabled(sample_gadget, TRUE);
		gt_page_set_disabled(phase_gadget, TRUE);
		gt_page_set_disabled(phase_minus_28_gadget, TRUE);
		gt_page_set_disabled(phase_minus_1_gadget, TRUE);
		gt_page_set_disabled(phase_plus_1_gadget, TRUE);
		gt_page_set_disabled(phase_plus_28_gadget, TRUE);
		gt_page_set_disabled(crop_h_gadget, TRUE);
		gt_page_set_disabled(crop_v_gadget, TRUE);
		gt_page_set_disabled(crop_h_auto_gadget, TRUE);
		gt_page_set_disabled(crop_v_auto_gadget, TRUE);
		gt_page_set_disabled(width_gadget, TRUE);
		gt_page_set_disabled(height_gadget, TRUE);
		gt_page_set_disabled(width_auto_gadget, TRUE);
		gt_page_set_disabled(height_auto_gadget, TRUE);
		gt_page_set_disabled(auto_gadget, TRUE);
		gt_page_set_disabled(align_gadget, TRUE);
		gt_page_set_disabled(find_gadget, TRUE);
		snprintf(gate_buffer, sizeof(gate_buffer),
			"Capture settings need firmware 2.3 or newer.");
	} else {
		gt_page_set_disabled(sample_gadget, FALSE);
		gt_page_set_disabled(width_auto_gadget, FALSE);
		gt_page_set_disabled(height_auto_gadget, FALSE);
	}
	gt_page_set_text(gate_gadget, gate_buffer);
}

/* Apply both crop entries and their independent Automatic selections. */
static BOOL change_crop(char *message)
{
	LONG h;
	LONG v;

	if (!field_value(crop_h_gadget, &h) || !field_value(crop_v_gadget, &v)) {
		snprintf(message, GT_TEXT_MAX, "Crop values must be whole numbers");
		return FALSE;
	}
	return video_set_crop((UWORD)h,
		gt_page_get(crop_h_auto_gadget, GTCB_Checked) == 0, (UWORD)v,
		gt_page_get(crop_v_auto_gadget, GTCB_Checked) == 0, message, GT_TEXT_MAX);
}

/* Apply both capture-window entries and their Automatic selections. */
static BOOL change_geometry(char *message)
{
	LONG width;
	LONG height;

	if (!field_value(width_gadget, &width) || !field_value(height_gadget, &height)) {
		snprintf(message, GT_TEXT_MAX, "Capture window values must be whole numbers");
		return FALSE;
	}
	return video_set_geometry(gt_page_get(width_auto_gadget, GTCB_Checked) ? 0 :
		(UWORD)width, gt_page_get(height_auto_gadget, GTCB_Checked) ? 0 :
		(UWORD)height, message, GT_TEXT_MAX);
}

/* Create this page's compact ten-row GadTools gadget chain. */
static BOOL create(struct gt_page *page)
{
	struct gt_form f;
	static const char *const capture_labels[] = {
		"Sample mode",
		"Phase",
		"Phase sweep",
		"Applied",
		NULL
	};
	WORD left, width, half;
	WORD btn_w, phase_w;
	WORD crop_lbl_w, chk_w, str_w, w_lbl_w, h_lbl_w;
	WORD btn3_w;

	page->rows = 10;
	snprintf(phase_buffer, sizeof(phase_buffer), "0");
	snprintf(crop_h_buffer, sizeof(crop_h_buffer), "0");
	snprintf(crop_v_buffer, sizeof(crop_v_buffer), "0");
	snprintf(width_buffer, sizeof(width_buffer), "0");
	snprintf(height_buffer, sizeof(height_buffer), "0");
	snprintf(sweep_buffer, sizeof(sweep_buffer), "Run 'Find phase...' to test.");
	gate_buffer[0] = '\0';
	applied_buffer[0] = '\0';

	gt_page_begin(page);
	gt_form_init(&f, page, capture_labels);
	left = f.left;
	width = f.width;
	half = (WORD)((width - 8) / 2);

	/* Row 0: Sample mode */
	sample_gadget = gt_form_cycle(&f, G_SAMPLE, 0, "Sample mode", (const char *const *)sample_names);

	/* Row 1: Phase String + 4 buttons */
	btn_w = gt_button_width("+28");
	phase_w = (WORD)(f.ctrl_width - btn_w * 4 - 16);
	if (phase_w > 70) phase_w = 70;
	phase_gadget = add_gadget(page, STRING_KIND, G_PHASE, f.ctrl_left, 1,
		phase_w, "Phase", PLACETEXT_LEFT, phase_tags);
	phase_minus_28_gadget = gt_add_button(page, G_PHASE_MINUS_28,
		(WORD)(f.ctrl_left + phase_w + 4), 1, btn_w, "-28");
	phase_minus_1_gadget = gt_add_button(page, G_PHASE_MINUS_1,
		(WORD)(f.ctrl_left + phase_w + btn_w + 8), 1, btn_w, "-1");
	phase_plus_1_gadget = gt_add_button(page, G_PHASE_PLUS_1,
		(WORD)(f.ctrl_left + phase_w + btn_w * 2 + 12), 1, btn_w, "+1");
	phase_plus_28_gadget = gt_add_button(page, G_PHASE_PLUS_28,
		(WORD)(f.ctrl_left + phase_w + btn_w * 3 + 16), 1, btn_w, "+28");

	/* Row 2: Sweep readout */
	sweep_gadget = gt_form_field(&f, G_SWEEP, 2, "Phase sweep", sweep_buffer);

	/* Row 3: Legend */
	legend_gadget = gt_add_note(page, G_LEGEND, left, 3, width,
		"Legend: = clean, + mixed, . dirty, | selected.");

	/* Row 4: Gate note */
	gate_gadget = gt_add_note(page, G_GATE, left, 4, width, gate_buffer);

	/* Row 5: Crop H & Crop V */
	chk_w = gt_checkbox_total_width("Auto");
	crop_lbl_w = (WORD)(gt_text_width("Crop H") + MAX_WORD(6, gt_layout.font_x));
	str_w = (WORD)(half - crop_lbl_w - chk_w - 4);
	if (str_w < 45) str_w = 45;

	crop_h_gadget = add_gadget(page, STRING_KIND, G_CROP_H,
		(WORD)(left + crop_lbl_w), 5, str_w, "Crop H", PLACETEXT_LEFT, crop_h_tags);
	crop_h_auto_gadget = gt_add_checkbox(page, G_CROP_H_AUTO,
		(WORD)(left + crop_lbl_w + str_w + 4), 5, "Auto", TRUE);

	crop_v_gadget = add_gadget(page, STRING_KIND, G_CROP_V,
		(WORD)(left + half + 8 + crop_lbl_w), 5, str_w, "Crop V", PLACETEXT_LEFT, crop_v_tags);
	crop_v_auto_gadget = gt_add_checkbox(page, G_CROP_V_AUTO,
		(WORD)(left + half + 8 + crop_lbl_w + str_w + 4), 5, "Auto", TRUE);

	/* Row 6: Capture width */
	w_lbl_w = (WORD)(gt_text_width("Width") + MAX_WORD(6, gt_layout.font_x));
	str_w = (WORD)(half - w_lbl_w - chk_w - 4);
	if (str_w < 55) str_w = 55;
	width_gadget = add_gadget(page, STRING_KIND, G_WIDTH,
		(WORD)(left + w_lbl_w), 6, str_w, "Width", PLACETEXT_LEFT, width_tags);
	width_auto_gadget = gt_add_checkbox(page, G_WIDTH_AUTO,
		(WORD)(left + w_lbl_w + str_w + 4), 6, "Auto", TRUE);

	/* Row 7: Capture height */
	h_lbl_w = (WORD)(gt_text_width("Height") + MAX_WORD(6, gt_layout.font_x));
	height_gadget = add_gadget(page, STRING_KIND, G_HEIGHT,
		(WORD)(left + h_lbl_w), 7, str_w, "Height", PLACETEXT_LEFT, height_tags);
	height_auto_gadget = gt_add_checkbox(page, G_HEIGHT_AUTO,
		(WORD)(left + h_lbl_w + str_w + 4), 7, "Auto", TRUE);

	/* Row 8: Applied */
	applied_gadget = gt_form_field(&f, G_APPLIED, 8, "Applied", applied_buffer);

	/* Row 9: Buttons */
	btn3_w = (WORD)((width - 8) / 3);
	auto_gadget = gt_add_button(page, G_AUTO, left, 9, btn3_w, "Reset auto");
	align_gadget = gt_add_button(page, G_ALIGN, (WORD)(left + btn3_w + 4),
		9, btn3_w, "Align picture...");
	find_gadget = gt_add_button(page, G_FIND, (WORD)(left + btn3_w * 2 + 8),
		9, (WORD)(width - btn3_w * 2 - 8), "Find phase...");

	return sample_gadget && phase_gadget && phase_minus_28_gadget &&
		phase_minus_1_gadget && phase_plus_1_gadget && phase_plus_28_gadget &&
		crop_h_gadget && crop_h_auto_gadget && crop_v_gadget &&
		crop_v_auto_gadget && width_gadget && width_auto_gadget &&
		height_gadget && height_auto_gadget && auto_gadget && align_gadget &&
		find_gadget && sweep_gadget && legend_gadget && gate_gadget &&
		applied_gadget;
}

/* Synchronize all page controls after a model or page visibility change. */
static void sync(void)
{
	set_attr(sample_gadget, GTCY_Active, (ULONG)cfg.edit.videocap_sample);
	sync_live();
}

/* Dispatch one page-local gadget action through the shared capture model. */
static void gadget(UWORD id, UWORD code)
{
	LONG entered_phase;
	int phase;
	int delta;
	BOOL ok;

	delta = 0;
	ok = TRUE;
	message_buffer[0] = '\0';
	switch (id) {
	case G_SAMPLE:
		if (code >= 3)
			return;
		ok = video_set_sample(code, message_buffer, sizeof(message_buffer));
		break;
	case G_PHASE:
		if (!field_value(phase_gadget, &entered_phase) || entered_phase < -1000 ||
			entered_phase > 1000) {
			snprintf(message_buffer, sizeof(message_buffer),
				"Phase must be signed steps in this clock's range");
			ok = FALSE;
		} else {
			ok = video_set_phase((int)entered_phase, TRUE, message_buffer,
				sizeof(message_buffer));
		}
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
	case G_CROP_H_AUTO:
	case G_CROP_V:
	case G_CROP_V_AUTO:
		ok = change_crop(message_buffer);
		break;
	case G_WIDTH:
	case G_WIDTH_AUTO:
	case G_HEIGHT:
	case G_HEIGHT_AUTO:
		ok = change_geometry(message_buffer);
		break;
	case G_AUTO:
		ok = video_reset_automatic(message_buffer, sizeof(message_buffer));
		break;
	case G_ALIGN:
		ok = video_align(message_buffer, sizeof(message_buffer));
		break;
	case G_FIND:
		ok = video_find_phase(message_buffer, sizeof(message_buffer));
		break;
	default:
		return;
	}
	if (delta) {
		if (!video_phase_state(&phase, message_buffer, sizeof(message_buffer))) {
			ok = FALSE;
		} else {
			ok = video_set_phase(zz_vcap_phase_step(phase, delta,
				video_phase_domain()), TRUE, message_buffer, sizeof(message_buffer));
		}
	}
	gt_status("%s", message_buffer);
	gt_pending_changed();
	sync();
	(void)ok;
}

/* Refresh the volatile capture readouts at two hertz while this page is shown. */
static void tick(void)
{
	if (gt_ticks() % 5 != 0)
		return;
	refresh_applied();
	video_sweep_text(sweep_buffer, sizeof(sweep_buffer));
	gt_page_set_text(sweep_gadget, sweep_buffer);
}

/* Commit typed framing fields before save, revert, or a page switch decides state. */
static void flush(void)
{
	BOOL h_auto;
	BOOL v_auto;
	BOOL w_auto;
	BOOL ht_auto;
	LONG h;
	LONG v;
	LONG width;
	LONG height;
	BOOL changed;

	if (!cfg.supported)
		return;
	h_auto = gt_page_get(crop_h_auto_gadget, GTCB_Checked) != 0;
	v_auto = gt_page_get(crop_v_auto_gadget, GTCB_Checked) != 0;
	w_auto = gt_page_get(width_auto_gadget, GTCB_Checked) != 0;
	ht_auto = gt_page_get(height_auto_gadget, GTCB_Checked) != 0;
	changed = FALSE;
	if (!field_value(crop_h_gadget, &h) || !field_value(crop_v_gadget, &v)) {
		gt_status("Crop values must be whole numbers");
		return;
	}
	if (h_auto == settings_present(&cfg.edit, CFG_CROP_H) ||
		v_auto == settings_present(&cfg.edit, CFG_CROP_V) ||
		(!h_auto && (UWORD)h != cfg.edit.videocap_crop_h) ||
		(!v_auto && (UWORD)v != cfg.edit.videocap_crop_v)) {
		if (change_crop(message_buffer))
			changed = TRUE;
		else
			gt_status("%s", message_buffer);
	}
	if (!field_value(width_gadget, &width) || !field_value(height_gadget, &height)) {
		gt_status("Capture window values must be whole numbers");
		return;
	}
	if (w_auto == settings_present(&cfg.edit, CFG_WIDTH) ||
		ht_auto == settings_present(&cfg.edit, CFG_HEIGHT) ||
		(!w_auto && (UWORD)width != cfg.edit.videocap_width) ||
		(!ht_auto && (UWORD)height != cfg.edit.videocap_height)) {
		if (change_geometry(message_buffer))
			changed = TRUE;
		else
			gt_status("%s", message_buffer);
	}
	if (changed)
		gt_pending_changed();
}

struct gt_page gt_page_capture = {
	"Capture",
	"Capture calibration",
	"Sampling phase, crop and capture window values.",
	GT_PAGE_ADVANCED | GT_PAGE_CHILD,
	NULL,
	NULL,
	10,
	create,
	NULL,
	NULL,
	gadget,
	tick,
	sync,
	flush,
	NULL
};
