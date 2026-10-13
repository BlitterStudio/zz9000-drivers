/*
 * ZZTop Diagnostics page: raw card observations and safe read-only checks.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <exec/types.h>
#include <exec/lists.h>
#include <intuition/classusr.h>
#include <gadgets/button.h>

#include <gadgets/checkbox.h>
#include <gadgets/layout.h>
#include <gadgets/listbrowser.h>
#include <gadgets/space.h>
#include <images/bevel.h>
#include <images/label.h>
#include <reaction/reaction_macros.h>
#include <clib/alib_protos.h>
#include <proto/checkbox.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/label.h>
#include <proto/layout.h>
#include <proto/button.h>
#include <proto/listbrowser.h>
#include <proto/space.h>

#include <stdio.h>
#include <string.h>

#include "audio.h"
#include "board.h"
#include "ui.h"
#include <zz9k/abi.h>

#define P PAGE_DIAG
#define REG_COUNT 14

enum {
	G_PROBE = 1,
	G_RESET_TEMP,
	G_AUDIO_LOG,
	G_REGISTERS
};

struct reg_line {
	const char *name;
	UWORD offset;
	char offset_text[8];
	char value[20];
	struct Node *node;
};

static struct reg_line registers[REG_COUNT] = {
	{ "Hardware version", ZZ_REG_HW_VERSION, { 0 }, { 0 }, NULL },
	{ "Firmware version", ZZ_REG_FW_VERSION, { 0 }, { 0 }, NULL },
	{ "Capabilities", ZZ_REG_FW_CAPABILITIES, { 0 }, { 0 }, NULL },
	{ "Temperature", ZZ_REG_TEMPERATURE, { 0 }, { 0 }, NULL },
	{ "VCCAUX", ZZ_REG_VOLTAGE_AUX, { 0 }, { 0 }, NULL },
	{ "VCCINT", ZZ_REG_VOLTAGE_INT, { 0 }, { 0 }, NULL },
	{ "USB status", ZZ_REG_USB_STATUS, { 0 }, { 0 }, NULL },
	{ "SD status", ZZ_REG_SD_STATUS, { 0 }, { 0 }, NULL },
	{ "SD boot status", ZZ_REG_SD_BOOT_STATUS, { 0 }, { 0 }, NULL },
	{ "Scanline mode", ZZ_SCANLINE_MODE_REG, { 0 }, { 0 }, NULL },
	{ "Scanline parity", ZZ_SCANLINE_PARITY_REG, { 0 }, { 0 }, NULL },
	{ "Video capture stats", ZZ_REG_VIDEOCAP_STATS, { 0 }, { 0 }, NULL },
	{ "Audio config", ZZ_REG_AUDIO_CONFIG, { 0 }, { 0 }, NULL },
	{ "Firmware updater status", ZZ_REG_FWUP_STATUS, { 0 }, { 0 }, NULL }
};

static struct List register_labels;
static struct ColumnInfo register_columns[] = {
	{ 150, (STRPTR)"Register", 0 },
	{ 50, (STRPTR)"Offset", 0 },
	{ 60, (STRPTR)"Value", 0 },
	{ -1, NULL, 0 }
};
static Object *g_registers, *g_probe, *g_reset_temp, *g_audio_log;
static struct ui_text t_board, t_probe, t_caps_a, t_caps_b, t_caps_c, t_caps_d, t_sdk_caps, t_temp;

static Object *button(UWORD id, const char *text)
{
	return NewObject(BUTTON_GetClass(), NULL,
		GA_ID, UI_GID(P, id),
		GA_RelVerify, TRUE,
		GA_Text, text,
	End;
}

static struct Node *register_node(struct reg_line *line)
{
	struct TagItem tags[] = {
		{ LBNA_Column, 0 }, { LBNCA_Text, 0 },
		{ LBNA_Column, 1 }, { LBNCA_Text, 0 },
		{ LBNA_Column, 2 }, { LBNCA_Text, 0 },
		{ TAG_END, 0 }
	};

	tags[1].ti_Data = (ULONG)line->name;
	tags[3].ti_Data = (ULONG)line->offset_text;
	tags[5].ti_Data = (ULONG)line->value;
	return AllocListBrowserNodeA(6, tags);
}
static void free_register_nodes(void)
{
	UWORD i;

	for (i = 0; i < REG_COUNT; i++) {
		if (!registers[i].node) continue;
		FreeListBrowserNode(registers[i].node);
		registers[i].node = NULL;
	}
	NewList(&register_labels);
}

static BOOL replace_value(struct reg_line *line, UWORD value)
{
	char next[sizeof(line->value)];

	snprintf(next, sizeof(next), "%04x", (unsigned)value);
	if (strcmp(next, line->value) == 0) return FALSE;
	snprintf(line->value, sizeof(line->value), "%s", next);
	return TRUE;
}

static void refresh_registers(void)
{
	struct board_health h;
	BOOL changed = FALSE;
	BOOL value_changed[REG_COUNT];
	UWORD i;
	UWORD values[REG_COUNT];

	board_sample_health(&h);
	values[0] = board.hw_version;
	values[1] = board.fw_version;
	values[2] = board.fw_caps;
	values[3] = (UWORD)h.temp_tenths;
	values[4] = h.vccaux_centi;
	values[5] = h.vccint_centi;
	values[6] = h.usb_status;
	values[7] = h.sd_status;
	values[8] = h.sd_boot_status;
	values[9] = board_scanline_mode();
	values[10] = board_scanline_parity();
	values[11] = h.vcap_stats_valid ? h.vcap_stats : 0;
	values[12] = board_read16(ZZ_REG_AUDIO_CONFIG);
	values[13] = board_read16(ZZ_REG_FWUP_STATUS);
	for (i = 0; i < REG_COUNT; i++) {
		value_changed[i] = replace_value(&registers[i], values[i]);
		if (value_changed[i]) changed = TRUE;
	}
	ui_text_printf(P, &t_temp, "%d.%d \260C (%d.%d to %d.%d this session)",
		h.temp_tenths / 10, h.temp_tenths % 10,
		h.temp_min_tenths / 10, h.temp_min_tenths % 10,
		h.temp_max_tenths / 10, h.temp_max_tenths % 10);

	if (!changed) return;
	/* Detach once, update only changed labels, then reattach: ListBrowser
	 * otherwise redraws each row separately and visibly flickers at 2 Hz. */
	ui_set(P, g_registers, LISTBROWSER_Labels, ~0UL, TAG_END);
	for (i = 0; i < REG_COUNT; i++) {
		if (!value_changed[i]) continue;
		struct TagItem tags[] = {
			{ LBNA_Column, 0 }, { LBNCA_Text, 0 },
			{ LBNA_Column, 1 }, { LBNCA_Text, 0 },
			{ LBNA_Column, 2 }, { LBNCA_Text, 0 },
			{ TAG_END, 0 }
		};
		tags[1].ti_Data = (ULONG)registers[i].name;
		tags[3].ti_Data = (ULONG)registers[i].offset_text;
		tags[5].ti_Data = (ULONG)registers[i].value;
		SetListBrowserNodeAttrsA(registers[i].node, tags);
	}
	ui_set(P, g_registers, LISTBROWSER_Labels, (ULONG)&register_labels,
		TAG_END);
}

static void refresh_capabilities(void)
{
	ui_text_printf(P, &t_caps_a,
		"Video: profiles %s; live picture adjustment %s",
		board_has_cap(ZZ_FW_CAP_VIDEOCAP_PROFILE) ? "yes" : "no",
		board_has_cap(ZZ_FW_CAP_VIDEOCAP_LIVE) ? "yes" : "no");
	ui_text_printf(P, &t_caps_b,
		"Video: centred 1080p %s; 50 Hz %s; source sync %s",
		board_has_cap(ZZ_FW_CAP_VIDEOCAP_CENTERED_1080P) ? "yes" : "no",
		board_has_cap(ZZ_FW_CAP_VIDEOCAP_CENTERED_1080P_50) ? "yes" : "no",
		board_has_cap(ZZ_FW_CAP_VIDEOCAP_SOURCE_SYNC) ? "yes" : "no");
	ui_text_printf(P, &t_caps_c, "Video: scanout origin %s; capture geometry %s",
		board_has_cap(ZZ_FW_CAP_VIDEOCAP_SCANOUT_ORIGIN) ? "yes" : "no",
		board_has_cap(ZZ_FW_CAP_VIDEOCAP_GEOMETRY) ? "yes" : "no");
	ui_text_printf(P, &t_caps_d,
		"Video: geometry acknowledgement %s; capture statistics %s",
		board_has_cap(ZZ_FW_CAP_VIDEOCAP_GEOMETRY_ACK) ? "yes" : "no",
		board_has_cap(ZZ_FW_CAP_VIDEOCAP_STATS) ? "yes" : "no");
	ui_text_printf(P, &t_sdk_caps,
		"Audio: control %s; metering %s",
		(board.zz9k_caps & ZZ9K_CAP_AUDIO_CONTROL) ? "yes" : "no",
		(board.zz9k_caps & ZZ9K_CAP_AUDIO_METERING) ? "yes" : "no");
}

static Object *create(void)
{
	UWORD i;

	NewList(&register_labels);
	for (i = 0; i < REG_COUNT; i++) {
		snprintf(registers[i].offset_text, sizeof(registers[i].offset_text),
			"%04lx", (unsigned long)registers[i].offset);
		registers[i].node = register_node(&registers[i]);
		if (!registers[i].node) {
			free_register_nodes();
			return NULL;
		}
		AddTail(&register_labels, registers[i].node);
	}
	return VGroupObject,
		LAYOUT_SpaceOuter, TRUE,
		LAYOUT_AddChild, VGroupObject,
			LAYOUT_BevelStyle, BVS_GROUP, LAYOUT_Label, "Board",
			LAYOUT_AddChild, ui_text_new(&t_board, ""),
			Label("Connection"),
			LAYOUT_AddChild, ui_text_new(&t_temp, ""),
			Label("Temperature"),
		End,
		LAYOUT_AddChild, VGroupObject,
			LAYOUT_BevelStyle, BVS_GROUP, LAYOUT_Label, "Raw registers",
			LAYOUT_AddChild, (g_registers = ListBrowserObject,
				GA_ID, UI_GID(P, G_REGISTERS), GA_RelVerify, TRUE,
				LISTBROWSER_Labels, (ULONG)&register_labels,
				LISTBROWSER_ColumnInfo, (ULONG)register_columns,
				LISTBROWSER_ColumnTitles, TRUE,
				LISTBROWSER_ShowSelected, FALSE,
			End),
			CHILD_MinHeight, 96,
		End,
		LAYOUT_AddChild, VGroupObject,
			LAYOUT_BevelStyle, BVS_GROUP, LAYOUT_Label, "Capabilities",
			LAYOUT_AddChild, ui_note_new(&t_caps_a, ""),
			LAYOUT_AddChild, ui_note_new(&t_caps_b, ""),
			LAYOUT_AddChild, ui_note_new(&t_caps_c, ""),
			LAYOUT_AddChild, ui_note_new(&t_caps_d, ""),
			LAYOUT_AddChild, ui_note_new(&t_sdk_caps, ""),
		End,
		LAYOUT_AddChild, VGroupObject,
			LAYOUT_BevelStyle, BVS_GROUP, LAYOUT_Label, "Checks",
			LAYOUT_AddChild, (g_probe = button(G_PROBE, "_Run bus probe")),
			LAYOUT_AddChild, ui_text_new(&t_probe, "Not run"),
			Label("Result"),
			LAYOUT_AddChild, (g_reset_temp = button(G_RESET_TEMP,
				"Reset temperature _range")),
			LAYOUT_AddChild, (g_audio_log = CheckBoxObject,
				GA_ID, UI_GID(P, G_AUDIO_LOG), GA_RelVerify, TRUE,
				GA_Text, "Audio control-plane debug log (T:zztop-audio.log)",
			End),
		End,
		LAYOUT_AddChild, SpaceObject, End,
	End;
}

static void gadget(UWORD id, UWORD code)
{
	ULONG errors;
	(void)code;
	if (id == G_PROBE) {
		errors = board_register_probe(8);
		if (errors) ui_text_printf(P, &t_probe, "%lu read errors",
			(unsigned long)errors);
		else ui_text_set(P, &t_probe, "No read errors");
		ui_status("Bus probe: %s", errors ? "read errors found" : "no read errors");
	} else if (id == G_RESET_TEMP) {
		board_reset_temp_range();
		refresh_registers();
		ui_status("Temperature range reset.");
	} else if (id == G_AUDIO_LOG) {
		audio_log_enable(ui_get(g_audio_log, CHECKBOX_Checked) != 0);
	}
}

static void sync(void)
{
	ui_text_printf(P, &t_board, "Zorro %s, board address $%08lx",
		board.zorro == 3 ? "III" : "II", (unsigned long)board.regs);
	ui_set(P, g_audio_log, CHECKBOX_Checked, audio_log_enabled(), TAG_END);
	refresh_capabilities();
	refresh_registers();
}

static void tick(void)
{
	if (ui_ticks() % 5 == 0) refresh_registers();
}
static void cleanup(void)
{
	free_register_nodes();
}

struct ui_page page_diag = {
	"Diagnostics",
	"Diagnostics",
	"Raw registers, capability bits and bus checks.",
	UI_PAGE_ADVANCED,
	create,
	NULL,
	NULL,
	gadget,
	tick,
	sync,
	NULL,
	cleanup,
};
