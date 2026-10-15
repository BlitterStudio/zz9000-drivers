/*
 * ZZTop GadTools Firmware page: guarded BOOT.bin upload and backup restore.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <exec/types.h>
#include <libraries/gadtools.h>
#include <proto/dos.h>
#include <proto/gadtools.h>
#include <stdio.h>
#include <string.h>

#include "board.h"
#include "fwup_amiga.h"
#include "fwup_client.h"
#include "gt.h"

#define P GT_PAGE_FIRMWARE

enum {
	G_INSTALLED = 1,
	G_SUPPORT_ONE,
	G_SUPPORT_TWO,
	G_CHOOSE,
	G_FILE,
	G_INSTALL,
	G_PROGRESS,
	G_STATUS,
	G_RESTART,
	G_RESTORE,
	G_RESTORE_NOTE
};

static struct Gadget *g_installed;
static struct Gadget *g_support_one;
static struct Gadget *g_support_two;
static struct Gadget *g_choose;
static struct Gadget *g_file;
static struct Gadget *g_install;
static struct Gadget *g_progress;
static struct Gadget *g_status;
static struct Gadget *g_restart;
static struct Gadget *g_restore;
static struct Gadget *g_restore_note;

static char installed[32];
static char path[256];
static char message[400];
static char support_one[96] = "Checking update support...";
static char support_two[96] = "";
static char file_text[96] = "No file chosen";
static char progress_text[32] = "Progress: 0 KB";
static char status_note[96] = "";
static char restart_note[96] = "";
static char restore_note[96] = "Promotes BOOT.bak while card still boots.";

static BOOL fwup_probed;
static BOOL fwup_supported;
static BOOL transfer_active;
static ULONG progress_max = 1;

/* Enable firmware actions only when their prerequisites are met. */
static void update_gate(void)
{
	gt_page_set_disabled(g_choose, transfer_active || !fwup_supported);
	gt_page_set_disabled(g_install, transfer_active || !fwup_supported || !path[0]);
	gt_page_set_disabled(g_restore, transfer_active || !fwup_supported);
}

/* Probe update support once and describe the available firmware actions. */
static void probe(void)
{
	if (fwup_probed)
		return;
	fwup_probed = TRUE;
	fwup_supported = fwup_probe_board((ULONG)board.regs) != 0;
	if (fwup_supported) {
		snprintf(support_one, sizeof(support_one), "%s", "Firmware accepts updates from AmigaOS.");
		snprintf(support_two, sizeof(support_two), "%s", "Choose a BOOT.bin image to install.");
	} else {
		snprintf(support_one, sizeof(support_one), "%s", "This firmware cannot update from AmigaOS.");
		snprintf(support_two, sizeof(support_two), "%s", "Install a FWUP build first.");
	}
	gt_page_set_text(g_support_one, support_one);
	gt_page_set_text(g_support_two, support_two);
	update_gate();
}

/* Render firmware transfer progress supplied by the updater callback. */
static void progress(void *ctx, ULONG done, LONG total)
{
	ULONG done_kb = (done + 1023) / 1024;
	(void)ctx;

	if (total > 0) {
		progress_max = ((ULONG)total + 1023) / 1024;
		if (progress_max == 0)
			progress_max = 1;
		snprintf(progress_text, sizeof(progress_text), "%lu of %lu KB",
			(unsigned long)done_kb, (unsigned long)progress_max);
	} else {
		snprintf(progress_text, sizeof(progress_text), "%s", "Installing...");
	}
	gt_page_set_text(g_progress, progress_text);
}

/* Install the chosen image while reporting updater progress and outcome. */
static void install(void)
{
	UWORD st;

	if (!path[0] || !fwup_supported || transfer_active)
		return;
	if (gt_ask("Install|Cancel",
		"Install %s as BOOT.bin?\n\nCurrent image kept as BOOT.bak.\nPower-cycle Amiga afterwards.",
		FilePart((CONST_STRPTR)path)) != 1)
		return;

	transfer_active = TRUE;
	update_gate();
	progress_max = 1;
	snprintf(progress_text, sizeof(progress_text), "%s", "0 KB");
	gt_page_set_text(g_progress, progress_text);
	restart_note[0] = '\0';
	gt_page_set_text(g_restart, restart_note);
	snprintf(status_note, sizeof(status_note), "%s", "Installing...");
	gt_page_set_text(g_status, status_note);
	gt_busy(TRUE);
	st = fwup_send_file((ULONG)board.regs, path, "BOOT.bin", progress, NULL);
	gt_busy(FALSE);
	transfer_active = FALSE;
	update_gate();
	if (st == FWUP_OK) {
		snprintf(progress_text, sizeof(progress_text), "%s", "Installed");
		gt_page_set_text(g_progress, progress_text);
		snprintf(status_note, sizeof(status_note), "%s", "Installed.");
		gt_page_set_text(g_status, status_note);
		snprintf(restart_note, sizeof(restart_note), "%s", "Power-cycle Amiga to start new firmware.");
		gt_page_set_text(g_restart, restart_note);
		gt_status("Firmware installed; power-cycle the Amiga.");
	} else {
		snprintf(message, sizeof(message), "Firmware install failed: %s (0x%04x)",
			fwup_strerror(st), (unsigned)st);
		snprintf(status_note, sizeof(status_note), "%s", "Install failed.");
		gt_page_set_text(g_status, status_note);
		gt_ask("OK", "%s", message);
		gt_status("Firmware install failed: %s", fwup_strerror(st));
	}
}

/* Restore the card's backup firmware after user confirmation. */
static void restore(void)
{
	UWORD st;

	if (!fwup_supported || transfer_active)
		return;
	if (gt_ask("Restore|Cancel",
		"Restore BOOT.bak as active BOOT.bin?\n\nPower-cycle Amiga afterwards.") != 1)
		return;

	transfer_active = TRUE;
	update_gate();
	restart_note[0] = '\0';
	gt_page_set_text(g_restart, restart_note);
	snprintf(status_note, sizeof(status_note), "%s", "Restoring previous firmware...");
	gt_page_set_text(g_status, status_note);
	gt_busy(TRUE);
	st = fwup_restore_board((ULONG)board.regs, "BOOT.bin");
	gt_busy(FALSE);
	transfer_active = FALSE;
	update_gate();
	if (st == FWUP_OK) {
		snprintf(status_note, sizeof(status_note), "%s", "Previous firmware restored.");
		gt_page_set_text(g_status, status_note);
		snprintf(restart_note, sizeof(restart_note), "%s",
			"Power-cycle Amiga to start restored firmware.");
		gt_page_set_text(g_restart, restart_note);
		gt_status("Previous firmware restored; power-cycle the Amiga.");
	} else {
		snprintf(message, sizeof(message), "Firmware restore failed: %s (0x%04x)",
			fwup_strerror(st), (unsigned)st);
		snprintf(status_note, sizeof(status_note), "%s", "Restore failed.");
		gt_page_set_text(g_status, status_note);
		gt_ask("OK", "%s", message);
		gt_status("Firmware restore failed: %s", fwup_strerror(st));
	}
}

/* Create the firmware status, update, and restore controls. */
static BOOL create(struct gt_page *page)
{
	struct gt_form f;
	static const char *const fw_labels[] = { "Installed", NULL };
	WORD left, width;
	WORD choose_w, inst_w, rest_w;

	gt_page_begin(page);
	gt_form_init(&f, page, fw_labels);
	left = f.left;
	width = f.width;

	/* Row 0: Installed */
	g_installed = gt_form_field(&f, G_INSTALLED, 0, "Installed", installed);

	/* Rows 1 & 2: Support notes */
	g_support_one = gt_add_note(page, G_SUPPORT_ONE, left, 1, width, support_one);
	g_support_two = gt_add_note(page, G_SUPPORT_TWO, left, 2, width, support_two);

	/* Row 3: Choose button + file readout */
	choose_w = gt_button_width("Choose file...");
	g_choose = gt_add_button(page, G_CHOOSE, left, 3, choose_w, "Choose file...");
	g_file = gt_add_note(page, G_FILE, (WORD)(left + choose_w + 8), 3,
		(WORD)(width - choose_w - 8), file_text);

	/* Row 4: Install button + progress */
	inst_w = gt_button_width("Install");
	g_install = gt_add_button(page, G_INSTALL, left, 4, inst_w, "Install");
	g_progress = gt_add_note(page, G_PROGRESS, (WORD)(left + inst_w + 8), 4,
		(WORD)(width - inst_w - 8), progress_text);

	/* Row 5: Status note */
	g_status = gt_add_note(page, G_STATUS, left, 5, width, status_note);

	/* Row 6: Restart note */
	g_restart = gt_add_note(page, G_RESTART, left, 6, width, restart_note);

	/* Row 7: Restore button */
	rest_w = gt_button_width("Restore previous firmware");
	g_restore = gt_add_button(page, G_RESTORE, left, 7, rest_w, "Restore previous firmware");

	/* Row 8: Restore note */
	g_restore_note = gt_add_note(page, G_RESTORE_NOTE, left, 8, width, restore_note);

	page->rows = 9;
	return g_installed && g_support_one && g_support_two && g_choose &&
		g_file && g_install && g_progress && g_status && g_restart &&
		g_restore && g_restore_note;
}

/* Probe firmware update support when this page is shown. */
static void show(void)
{
	probe();
}

/* Dispatch a firmware page action selected by the user. */
static void gadget(UWORD id, UWORD code)
{
	(void)code;
	if (id == G_CHOOSE) {
		if (fwup_supported && gt_pick_file("Select firmware file to install",
			"#?.(bin|rom|img)", path, sizeof(path))) {
			snprintf(file_text, sizeof(file_text), "%s", (const char *)FilePart((CONST_STRPTR)path));
			gt_page_set_text(g_file, file_text);
			gt_status("Firmware image selected: %s", FilePart((CONST_STRPTR)path));
			update_gate();
		}
	} else if (id == G_INSTALL) {
		install();
	} else if (id == G_RESTORE) {
		restore();
	}
}

/* Synchronise installed-version and firmware action readouts. */
static void sync(void)
{
	snprintf(installed, sizeof(installed), "%u.%u", board.fw_version >> 8,
		board.fw_version & 0xff);
	gt_page_set_text(g_installed, installed);
	if (path[0]) {
		snprintf(file_text, sizeof(file_text), "%s", (const char *)FilePart((CONST_STRPTR)path));
		gt_page_set_text(g_file, file_text);
	}
	update_gate();
}

struct gt_page gt_page_firmware = {
	"Firmware",
	"Firmware",
	"Install new firmware or go back to the previous one.",
	0,
	NULL,
	NULL,
	9,
	create,
	show,
	NULL,
	gadget,
	NULL,
	sync,
	NULL,
	NULL
};
