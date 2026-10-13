/*
 * ZZTop GadTools Diagnostics page: raw card observations and bus checks.
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

#include "audio.h"
#include "board.h"
#include "gt.h"
#include "zz9k/abi.h"

#define P GT_PAGE_DIAG
#define DIAG_REG_COUNT 14

enum {
	DIAG_GID_CONNECTION = 1,
	DIAG_GID_TEMPERATURE,
	DIAG_GID_REGISTERS,
	DIAG_GID_CAPS_A,
	DIAG_GID_CAPS_B,
	DIAG_GID_CAPS_AUDIO,
	DIAG_GID_PROBE,
	DIAG_GID_PROBE_RESULT,
	DIAG_GID_RESET_TEMP,
	DIAG_GID_AUDIO_LOG
};

struct diag_reg_desc {
	const char *name;
	UWORD offset;
	struct Node node;
	char text[48];
};

static struct diag_reg_desc diag_registers[DIAG_REG_COUNT] = {
	{ "HW version", ZZ_REG_HW_VERSION, { 0 }, { 0 } },
	{ "FW version", ZZ_REG_FW_VERSION, { 0 }, { 0 } },
	{ "Capabilities", ZZ_REG_FW_CAPABILITIES, { 0 }, { 0 } },
	{ "Temperature", ZZ_REG_TEMPERATURE, { 0 }, { 0 } },
	{ "VCCAUX", ZZ_REG_VOLTAGE_AUX, { 0 }, { 0 } },
	{ "VCCINT", ZZ_REG_VOLTAGE_INT, { 0 }, { 0 } },
	{ "USB status", ZZ_REG_USB_STATUS, { 0 }, { 0 } },
	{ "SD status", ZZ_REG_SD_STATUS, { 0 }, { 0 } },
	{ "SD boot status", ZZ_REG_SD_BOOT_STATUS, { 0 }, { 0 } },
	{ "Scanline mode", ZZ_SCANLINE_MODE_REG, { 0 }, { 0 } },
	{ "Scanline parity", ZZ_SCANLINE_PARITY_REG, { 0 }, { 0 } },
	{ "Video stats", ZZ_REG_VIDEOCAP_STATS, { 0 }, { 0 } },
	{ "Audio config", ZZ_REG_AUDIO_CONFIG, { 0 }, { 0 } },
	{ "FWUP status", ZZ_REG_FWUP_STATUS, { 0 }, { 0 } }
};

static struct List diag_register_labels;
static struct Gadget *diag_connection;
static struct Gadget *diag_temperature;
static struct Gadget *diag_register_list;
static struct Gadget *diag_caps_a;
static struct Gadget *diag_caps_b;
static struct Gadget *diag_caps_audio;
static struct Gadget *diag_probe;
static struct Gadget *diag_probe_result;
static struct Gadget *diag_reset_temp;
static struct Gadget *diag_audio_log;

static char diag_connection_text[64];
static char diag_temperature_text[64];
static char diag_caps_a_text[96];
static char diag_caps_b_text[96];
static char diag_caps_audio_text[96];
static char diag_probe_text[48] = "Not run";
static char diag_next_text[48];

/* Format one register's address, label, and current value for the list. */
static void diag_format_register(struct diag_reg_desc *desc, UWORD value)
{
	snprintf(desc->text, sizeof(desc->text), "$%04lx: %-14s = $%04x",
		(unsigned long)desc->offset, desc->name, (unsigned)value);
}

/* Read live registers and update their diagnostics list entries. */
static void diag_refresh_registers(void)
{
	struct board_health health;
	UWORD values[DIAG_REG_COUNT];
	UWORD i;

	board_sample_health(&health);
	values[0] = board.hw_version;
	values[1] = board.fw_version;
	values[2] = board.fw_caps;
	values[3] = (UWORD)health.temp_tenths;
	values[4] = health.vccaux_centi;
	values[5] = health.vccint_centi;
	values[6] = health.usb_status;
	values[7] = health.sd_status;
	values[8] = health.sd_boot_status;
	values[9] = board_scanline_mode();
	values[10] = board_scanline_parity();
	values[11] = health.vcap_stats_valid ? health.vcap_stats : 0;
	values[12] = board_read16(ZZ_REG_AUDIO_CONFIG);
	values[13] = board_read16(ZZ_REG_FWUP_STATUS);

	for (i = 0; i < DIAG_REG_COUNT; i++)
		diag_format_register(&diag_registers[i], values[i]);

	if (diag_register_list) {
		GT_SetGadgetAttrs(diag_register_list, gt_render_window(diag_register_list), NULL,
			GTLV_Labels, ~0UL, TAG_END);
		GT_SetGadgetAttrs(diag_register_list, gt_render_window(diag_register_list), NULL,
			GTLV_Labels, (ULONG)&diag_register_labels, TAG_END);
	}
	snprintf(diag_temperature_text, sizeof(diag_temperature_text),
		"Temp: %d.%d C (%d.%d-%d.%d)",
		health.temp_tenths / 10, health.temp_tenths % 10,
		health.temp_min_tenths / 10, health.temp_min_tenths % 10,
		health.temp_max_tenths / 10, health.temp_max_tenths % 10);
	gt_page_set_text(diag_temperature, diag_temperature_text);
}

/* Describe advertised video and audio capabilities in page readouts. */
static void diag_refresh_capabilities(void)
{
	snprintf(diag_caps_a_text, sizeof(diag_caps_a_text),
		"Video: profiles %s; live %s; 1080p %s; 50Hz %s",
		board_has_cap(ZZ_FW_CAP_VIDEOCAP_PROFILE) ? "yes" : "no",
		board_has_cap(ZZ_FW_CAP_VIDEOCAP_LIVE) ? "yes" : "no",
		board_has_cap(ZZ_FW_CAP_VIDEOCAP_CENTERED_1080P) ? "yes" : "no",
		board_has_cap(ZZ_FW_CAP_VIDEOCAP_CENTERED_1080P_50) ? "yes" : "no");
	snprintf(diag_caps_b_text, sizeof(diag_caps_b_text),
		"Video: sync %s; origin %s; geom %s; stats %s",
		board_has_cap(ZZ_FW_CAP_VIDEOCAP_SOURCE_SYNC) ? "yes" : "no",
		board_has_cap(ZZ_FW_CAP_VIDEOCAP_SCANOUT_ORIGIN) ? "yes" : "no",
		board_has_cap(ZZ_FW_CAP_VIDEOCAP_GEOMETRY) ? "yes" : "no",
		board_has_cap(ZZ_FW_CAP_VIDEOCAP_STATS) ? "yes" : "no");
	snprintf(diag_caps_audio_text, sizeof(diag_caps_audio_text),
		"Audio: control %s; metering %s",
		(board.zz9k_caps & ZZ9K_CAP_AUDIO_CONTROL) ? "yes" : "no",
		(board.zz9k_caps & ZZ9K_CAP_AUDIO_METERING) ? "yes" : "no");
	gt_page_set_text(diag_caps_a, diag_caps_a_text);
	gt_page_set_text(diag_caps_b, diag_caps_b_text);
	gt_page_set_text(diag_caps_audio, diag_caps_audio_text);
}

/* Initialise the Exec list that owns register-label nodes. */
static void diag_list_init(struct List *list)
{
	list->lh_Head = (struct Node *)&list->lh_Tail;
	list->lh_Tail = NULL;
	list->lh_TailPred = (struct Node *)&list->lh_Head;
}

/* Format and link static register descriptors into the GadTools list. */
static void diag_build_register_labels(void)
{
	UWORD i;

	diag_list_init(&diag_register_labels);
	for (i = 0; i < DIAG_REG_COUNT; i++) {
		diag_registers[i].node.ln_Name = diag_registers[i].text;
		AddTail(&diag_register_labels, &diag_registers[i].node);
	}
}

/* Create the diagnostics readouts, register list, and action buttons. */
static BOOL diag_create(struct gt_page *page)
{
	struct NewGadget ng;
	WORD left, width, half;
	WORD probe_w, reset_w, result_w;
	struct TagItem list_tags[] = {
		{ GTLV_Labels, 0 },
		{ GTLV_ShowSelected, 0 },
		{ TAG_DONE, 0 }
	};

	left = gt_layout.page_left;
	width = gt_layout.page_width;
	half = (WORD)((width - 8) / 2);

	gt_page_begin(page);
	diag_build_register_labels();

	/* Row 0: Connection & Temperature */
	diag_connection_text[0] = '\0';
	diag_temperature_text[0] = '\0';
	diag_connection = gt_add_note(page, DIAG_GID_CONNECTION, left, 0, half, diag_connection_text);
	diag_temperature = gt_add_note(page, DIAG_GID_TEMPERATURE,
		(WORD)(left + half + 8), 0, half, diag_temperature_text);

	/* Rows 1-4: Raw registers LISTVIEW (4 rows tall!) */
	memset(&ng, 0, sizeof(ng));
	ng.ng_LeftEdge = left;
	ng.ng_TopEdge = gt_row(1);
	ng.ng_Width = width;
	ng.ng_Height = (WORD)(gt_row(4) + gt_layout.row_height - gt_row(1));
	ng.ng_GadgetID = GT_GID(P, DIAG_GID_REGISTERS);
	list_tags[0].ti_Data = (ULONG)&diag_register_labels;
	diag_register_list = gt_page_add(page, LISTVIEW_KIND, &ng, list_tags);

	/* Rows 5, 6, 7: Capabilities */
	diag_caps_a_text[0] = '\0';
	diag_caps_b_text[0] = '\0';
	diag_caps_audio_text[0] = '\0';
	diag_caps_a = gt_add_note(page, DIAG_GID_CAPS_A, left, 5, width, diag_caps_a_text);
	diag_caps_b = gt_add_note(page, DIAG_GID_CAPS_B, left, 6, width, diag_caps_b_text);
	diag_caps_audio = gt_add_note(page, DIAG_GID_CAPS_AUDIO, left, 7, width, diag_caps_audio_text);

	/* Row 8: Bus probe button + result + reset temp button */
	probe_w = gt_button_width("Run bus probe");
	reset_w = gt_button_width("Reset temp");
	result_w = (WORD)(width - probe_w - reset_w - 12);

	diag_probe = gt_add_button(page, DIAG_GID_PROBE, left, 8, probe_w, "Run bus probe");
	diag_probe_result = gt_add_note(page, DIAG_GID_PROBE_RESULT,
		(WORD)(left + probe_w + 4), 8, result_w, diag_probe_text);
	diag_reset_temp = gt_add_button(page, DIAG_GID_RESET_TEMP,
		(WORD)(left + probe_w + result_w + 8), 8, reset_w, "Reset temp");

	/* Row 9: Audio debug log checkbox */
	diag_audio_log = gt_add_checkbox(page, DIAG_GID_AUDIO_LOG, left, 9,
		"Audio control-plane debug log (T:zztop-audio.log)",
		audio_log_enabled());

	page->rows = 10;
	return diag_connection && diag_temperature && diag_register_list &&
		diag_caps_a && diag_caps_b && diag_caps_audio && diag_probe &&
		diag_probe_result && diag_reset_temp && diag_audio_log;
}

/* Run the diagnostic action selected by a page gadget. */
static void diag_gadget(UWORD id, UWORD code)
{
	ULONG errors;
	(void)code;

	if (id == DIAG_GID_PROBE) {
		errors = board_register_probe(8);
		snprintf(diag_next_text, sizeof(diag_next_text),
			errors ? "%lu read errors" : "No read errors",
			(unsigned long)errors);
		gt_page_set_text(diag_probe_result, diag_next_text);
		gt_status("Bus probe: %s", errors ? "read errors found" : "no read errors");
	} else if (id == DIAG_GID_RESET_TEMP) {
		board_reset_temp_range();
		diag_refresh_registers();
		gt_status("Temperature range reset.");
	} else if (id == DIAG_GID_AUDIO_LOG) {
		audio_log_enable(gt_page_get(diag_audio_log, GTCB_Checked) != 0);
	}
}

/* Synchronise diagnostic text from the current board state. */
static void diag_sync(void)
{
	snprintf(diag_connection_text, sizeof(diag_connection_text),
		"Zorro %s, board $%08lx",
		board.zorro == 3 ? "III" : "II", (unsigned long)board.regs);
	gt_page_set_text(diag_connection, diag_connection_text);
	GT_SetGadgetAttrs(diag_audio_log, gt_render_window(diag_audio_log), NULL,
		GTCB_Checked, audio_log_enabled(), TAG_END);
	diag_refresh_capabilities();
	diag_refresh_registers();
}

/* Refresh volatile diagnostics on the page timer cadence. */
static void diag_tick(void)
{
	if (gt_ticks() % 5 == 0)
		diag_refresh_registers();
}

/* Clear references after diagnostics gadgets are disposed. */
static void diag_cleanup(void)
{
	diag_connection = NULL;
	diag_temperature = NULL;
	diag_register_list = NULL;
	diag_caps_a = NULL;
	diag_caps_b = NULL;
	diag_caps_audio = NULL;
	diag_probe = NULL;
	diag_probe_result = NULL;
	diag_reset_temp = NULL;
	diag_audio_log = NULL;
}

struct gt_page gt_page_diag = {
	"Diagnostics",
	"Diagnostics",
	"Raw registers, capability bits and bus checks.",
	GT_PAGE_ADVANCED,
	NULL,
	NULL,
	10,
	diag_create,
	NULL,
	NULL,
	diag_gadget,
	diag_tick,
	diag_sync,
	NULL,
	diag_cleanup
};
