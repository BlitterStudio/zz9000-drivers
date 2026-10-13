/*
 * ZZTop Firmware page: guarded BOOT.bin upload and backup restore.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <exec/types.h>
#include <dos/dos.h>
#include <intuition/classusr.h>
#include <gadgets/button.h>
#include <gadgets/fuelgauge.h>
#include <gadgets/layout.h>
#include <gadgets/space.h>
#include <images/bevel.h>
#include <images/label.h>
#include <reaction/reaction_macros.h>
#include <proto/dos.h>
#include <proto/fuelgauge.h>
#include <proto/intuition.h>
#include <proto/label.h>
#include <proto/layout.h>
#include <proto/button.h>
#include <proto/space.h>

#include <stdio.h>
#include <string.h>

#include "board.h"
#include "fwup_amiga.h"
#include "fwup_client.h"
#include "ui.h"

#define P PAGE_FIRMWARE

enum {
	G_CHOOSE = 1,
	G_INSTALL,
	G_RESTORE
};

static Object *g_choose, *g_install, *g_restore;
static struct ui_gauge g_progress;
static struct ui_text t_installed, t_support, t_file, t_status, t_restart, t_restore_note;
/* Workbench launches have a small stack: these requester and status buffers
 * must survive nested requesters and the synchronous push. */
static char path[256];
static char message[400];
static BOOL fwup_probed;
static BOOL fwup_supported;
static BOOL transfer_active;
static ULONG progress_max = 1;
static char progress_text[24];

static Object *button(UWORD id, const char *text)
{
	return NewObject(BUTTON_GetClass(), NULL,
		GA_ID, UI_GID(P, id),
		GA_RelVerify, TRUE,
		GA_Text, text,
	End;
}

static void update_gate(void)
{
	ui_disable(P, g_choose, transfer_active || !fwup_supported);
	ui_disable(P, g_install, transfer_active || !fwup_supported || !path[0]);
	ui_disable(P, g_restore, transfer_active || !fwup_supported);
}

static void probe(void)
{
	if (fwup_probed) return;
	fwup_probed = TRUE;
	fwup_supported = fwup_probe_board((ULONG)board.regs) != 0;
	if (fwup_supported)
		ui_text_set(P, &t_support,
			"This firmware accepts updates from AmigaOS.");
	else
		ui_text_set(P, &t_support,
			"This firmware cannot update from AmigaOS; install a FWUP build first.");
	update_gate();
}

static void progress(void *ctx, ULONG done, LONG total)
{
	ULONG done_kb = (done + 1023) / 1024;

	(void)ctx;
	if (total > 0) {
		progress_max = ((ULONG)total + 1023) / 1024;
		if (progress_max == 0) progress_max = 1;
		ui_set(P, g_progress.obj, FUELGAUGE_Max, progress_max, TAG_END);
		snprintf(progress_text, sizeof(progress_text), "%lu of %lu KB",
			(unsigned long)done_kb, (unsigned long)progress_max);
		ui_gauge_set(P, &g_progress, (LONG)done_kb, progress_text);
	} else {
		ui_gauge_set(P, &g_progress, 0, "Installing...");
	}
	ui_service_refresh();
}

static void install(void)
{
	UWORD st;

	if (!path[0] || !fwup_supported || transfer_active) return;
	if (ui_ask("Install|Cancel",
		"Install %s as BOOT.bin?\n\nThe current image is kept as BOOT.bak.\n"
		"Power-cycle the Amiga afterwards to start the new firmware.",
		FilePart((CONST_STRPTR)path)) != 1)
		return;

	transfer_active = TRUE;
	update_gate();
	progress_max = 1;
	ui_set(P, g_progress.obj, FUELGAUGE_Max, progress_max, TAG_END);
	ui_gauge_set(P, &g_progress, 0, "0 KB");
	ui_text_set(P, &t_restart, "");
	ui_text_set(P, &t_status, "Installing...");
	ui_busy(TRUE);
	st = fwup_send_file((ULONG)board.regs, path, "BOOT.bin", progress, NULL);
	ui_busy(FALSE);
	transfer_active = FALSE;
	update_gate();
	if (st == FWUP_OK) {
		ui_gauge_set(P, &g_progress, (LONG)progress_max, "Installed");
		ui_text_set(P, &t_status, "Installed.");
		ui_text_set(P, &t_restart,
			"Power-cycle the Amiga to start the new firmware.");
		ui_status("Firmware installed; power-cycle the Amiga.");
	} else {
		snprintf(message, sizeof(message), "Firmware install failed: %s (0x%04x)",
			fwup_strerror(st), (unsigned)st);
		ui_text_set(P, &t_status, "Install failed.");
		ui_error("%s", message);
		ui_status("Firmware install failed: %s", fwup_strerror(st));
	}
}

static void restore(void)
{
	UWORD st;

	if (!fwup_supported || transfer_active) return;
	if (ui_ask("Restore|Cancel",
		"Restore BOOT.bak as the active BOOT.bin?\n\n"
		"This needs a booting card. The current BOOT.bin is discarded and no "
		"backup remains. Power-cycle the Amiga afterwards.") != 1)
		return;

	transfer_active = TRUE;
	update_gate();
	ui_text_set(P, &t_restart, "");
	ui_text_set(P, &t_status, "Restoring previous firmware...");
	ui_busy(TRUE);
	st = fwup_restore_board((ULONG)board.regs, "BOOT.bin");
	ui_busy(FALSE);
	transfer_active = FALSE;
	update_gate();
	if (st == FWUP_OK) {
		ui_text_set(P, &t_status, "Previous firmware restored.");
		ui_text_set(P, &t_restart,
			"Power-cycle the Amiga to start the restored firmware.");
		ui_status("Previous firmware restored; power-cycle the Amiga.");
	} else {
		snprintf(message, sizeof(message), "Firmware restore failed: %s (0x%04x)",
			fwup_strerror(st), (unsigned)st);
		ui_text_set(P, &t_status, "Restore failed.");
		ui_error("%s", message);
		ui_status("Firmware restore failed: %s", fwup_strerror(st));
	}
}

static Object *create(void)
{
	return VGroupObject,
		LAYOUT_SpaceOuter, TRUE,
		LAYOUT_AddChild, VGroupObject,
			LAYOUT_BevelStyle, BVS_GROUP,
			LAYOUT_Label, "Installed",
			LAYOUT_AddChild, ui_text_new(&t_installed, ""),
			Label("Firmware version"),
			LAYOUT_AddChild, ui_note_new(&t_support, "Checking update support..."),
		End,
		LAYOUT_AddChild, VGroupObject,
			LAYOUT_BevelStyle, BVS_GROUP,
			LAYOUT_Label, "Install new firmware",
			LAYOUT_AddChild, HGroupObject,
				LAYOUT_AddChild, (g_choose = button(G_CHOOSE, "_Choose file...")),
				CHILD_WeightedWidth, 0,
				LAYOUT_AddChild, ui_text_new(&t_file, "No file chosen"),
			End,
			LAYOUT_AddChild, ui_gauge_new(&g_progress, 1),
			Label("Progress"),
			LAYOUT_AddChild, (g_install = button(G_INSTALL, "_Install")),
			LAYOUT_AddChild, ui_note_new(&t_status, ""),
			LAYOUT_AddChild, ui_note_new(&t_restart, ""),
		End,
		LAYOUT_AddChild, VGroupObject,
			LAYOUT_BevelStyle, BVS_GROUP,
			LAYOUT_Label, "Go back",
			LAYOUT_AddChild, (g_restore = button(G_RESTORE,
				"_Restore previous firmware")),
			LAYOUT_AddChild, ui_note_new(&t_restore_note,
				"Promotes BOOT.bak only while the card still boots."),
		End,
		LAYOUT_AddChild, SpaceObject, End,
	End;
}

static void show(void)
{
	probe();
}

static void gadget(UWORD id, UWORD code)
{
	(void)code;
	if (id == G_CHOOSE) {
		if (fwup_supported && ui_pick_file("Select firmware file to install",
			"#?.(bin|rom|img)", path, sizeof(path))) {
			ui_text_set(P, &t_file,
				(const char *)FilePart((CONST_STRPTR)path));
			ui_status("Firmware image selected: %s", FilePart((CONST_STRPTR)path));
			update_gate();
		}
	} else if (id == G_INSTALL) {
		install();
	} else if (id == G_RESTORE) {
		restore();
	}
}

static void sync(void)
{
	ui_text_printf(P, &t_installed, "%u.%u", board.fw_version >> 8,
		board.fw_version & 0xff);
	if (path[0]) ui_text_set(P, &t_file,
		(const char *)FilePart((CONST_STRPTR)path));
	update_gate();
}

struct ui_page page_firmware = {
	"Firmware",
	"Firmware",
	"Install new firmware or go back to the previous one.",
	0,
	create,
	show,
	NULL,
	gadget,
	NULL,
	sync,
	NULL,
	NULL,
};
