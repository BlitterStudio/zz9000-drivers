/*
 * ZZTop GadTools Firmware page: online update check and download, guarded
 * BOOT.bin upload and backup restore.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <exec/types.h>
#include <libraries/gadtools.h>
#include <proto/dos.h>
#include <proto/gadtools.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "board.h"
#include "fw_release.h"
#include "fwonline.h"
#include "fwup_amiga.h"
#include "fwup_client.h"
#include "gt.h"

#define P GT_PAGE_FIRMWARE

enum {
	G_INSTALLED = 1,
	G_SUPPORT,
	G_LATEST,
	G_VARIANT,
	G_CHECK,
	G_UPDATE,
	G_CHOOSE,
	G_FILE,
	G_INSTALL,
	G_PROGRESS,
	G_STATUS,
	G_RESTORE,
	G_RESTORE_NOTE
};

static struct Gadget *g_installed;
static struct Gadget *g_support;
static struct Gadget *g_latest;
static struct Gadget *g_variant;
static struct Gadget *g_check;
static struct Gadget *g_update;
static struct Gadget *g_choose;
static struct Gadget *g_file;
static struct Gadget *g_install;
static struct Gadget *g_progress;
static struct Gadget *g_status;
static struct Gadget *g_restore;
static struct Gadget *g_restore_note;

static char installed[32];
static char path[256];
static char message[400];
static char support_text[96] = "Checking update support...";
static char latest_text[96] = "Not checked";
static char file_text[96] = "No file chosen";
static char progress_text[40] = "Progress: 0 KB";
static char status_note[96] = "";
static char restore_note[96] = "Promotes BOOT.bak while card still boots.";

static const char *variant_choices[FWREL_VARIANT_COUNT + 1];
static int variant = FWREL_VARIANT_ZORRO3;   /* chosen release image */
static int detected = FWREL_VARIANT_UNKNOWN; /* running bitstream */
static BOOL variant_chosen;                  /* variant initialised */
static int variant_shown = -1;               /* cycle gadget's value */

static struct fwonline_release release;
static BOOL release_known;

static BOOL fwup_probed;
static BOOL fwup_supported;
static BOOL transfer_active;
static ULONG progress_max = 1;

static struct TagItem set_tags[] = {
	{ TAG_IGNORE, 0 },
	{ TAG_DONE, 0 }
};

/* Set one GadTools attribute through a real two-item TagItem array. */
static void set_attr(struct Gadget *gadget, ULONG tag, ULONG value)
{
	if (!gadget)
		return;
	set_tags[0].ti_Tag = tag;
	set_tags[0].ti_Data = value;
	GT_SetGadgetAttrsA(gadget, gt_render_window(gadget), NULL, set_tags);
}

/* Enable firmware actions only when their prerequisites are met. */
static void update_gate(void)
{
	gt_page_set_disabled(g_variant, transfer_active);
	gt_page_set_disabled(g_check, transfer_active);
	gt_page_set_disabled(g_update, transfer_active || !fwup_supported ||
		!release_known);
	gt_page_set_disabled(g_choose, transfer_active || !fwup_supported);
	gt_page_set_disabled(g_install, transfer_active || !fwup_supported || !path[0]);
	gt_page_set_disabled(g_restore, transfer_active || !fwup_supported);
}

static void set_status(const char *text)
{
	snprintf(status_note, sizeof(status_note), "%s", text);
	gt_page_set_text(g_status, status_note);
}

static void set_progress(const char *text)
{
	snprintf(progress_text, sizeof(progress_text), "%s", text);
	gt_page_set_text(g_progress, progress_text);
}

/* Append an install-confirmation line without ever stepping past message. */
static void message_append(ULONG *used, const char *fmt, ...)
{
	ULONG remaining;
	va_list ap;
	int written;

	if (*used >= sizeof(message))
		return;
	remaining = sizeof(message) - *used;
	va_start(ap, fmt);
	written = vsnprintf(message + *used, remaining, fmt, ap);
	va_end(ap);
	if (written < 0 || (ULONG)written >= remaining)
		*used = sizeof(message) - 1;
	else
		*used += (ULONG)written;
}


/* Describe the last check result for the chosen image. */
static void render_latest(void)
{
	const char *state;

	if (!release_known) {
		gt_page_set_text(g_latest, latest_text);
		return;
	}
	switch (release.compare) {
	case FWREL_NEWER:           state = "update available"; break;
	case FWREL_UP_TO_DATE:      state = "installed"; break;
	case FWREL_INSTALLED_NEWER: state = "older than installed"; break;
	default:                    state = "may be newer"; break;
	}
	snprintf(latest_text, sizeof(latest_text), "%s - %s", release.tag, state);
	gt_page_set_text(g_latest, latest_text);
}

/* Probe update support once and describe the available firmware actions. */
static void probe(void)
{
	if (fwup_probed)
		return;
	fwup_probed = TRUE;
	fwup_supported = fwup_probe_board((ULONG)board.regs) != 0;
	snprintf(support_text, sizeof(support_text), "%s", fwup_supported ?
		"Firmware accepts updates from AmigaOS." :
		"Firmware cannot update from AmigaOS.");
	gt_page_set_text(g_support, support_text);
	update_gate();
}

/* Render transfer progress as "<verb>N of M KB" (or "<verb>N KB"). */
static void show_progress(const char *verb, ULONG done, LONG total)
{
	ULONG done_kb = (done + 1023) / 1024;

	if (total > 0) {
		progress_max = ((ULONG)total + 1023) / 1024;
		if (progress_max == 0)
			progress_max = 1;
		snprintf(progress_text, sizeof(progress_text), "%s%lu of %lu KB",
			verb, (unsigned long)done_kb, (unsigned long)progress_max);
	} else {
		snprintf(progress_text, sizeof(progress_text), "%s%lu KB", verb,
			(unsigned long)done_kb);
	}
	gt_page_set_text(g_progress, progress_text);
}

/* FWUP transfer progress (fwup_send_file callback). */
static void progress(void *ctx, ULONG done, LONG total)
{
	(void)ctx;
	show_progress("", done, total);
}

/* Release download progress (fwonline hook; ctx is the window wait). */
static void download_progress(void *ctx, ULONG done, LONG total)
{
	(void)ctx;
	show_progress("Downloaded ", done, total);
}

/* Transfer a previously inspected release bundle. Modules go first; a module
 * failure leaves BOOT.bin untouched. */
static BOOL install_bundle(const struct fwup_bundle *bundle, const char *tag)
{
	char failed_name[FWUP_MODULE_FILENAME_MAX + 1];
	int modules_skipped = FALSE;
	UWORD st;

	transfer_active = TRUE;
	update_gate();
	progress_max = 1;
	set_progress("0 KB");
	set_status("Installing...");
	gt_busy(TRUE);
	st = fwup_bundle_install((ULONG)board.regs, bundle,
		fwup_board_capabilities((ULONG)board.regs), progress, NULL,
		failed_name, sizeof(failed_name), &modules_skipped);
	gt_busy(FALSE);
	transfer_active = FALSE;
	update_gate();
	if (st == FWUP_OK) {
		fwonline_record_install(tag);
		set_progress("Installed");
		if (modules_skipped) {
			set_status("BOOT.bin installed. Reboot, then run install again for modules.");
			gt_status("Firmware installed; reboot, then run install again for modules.");
		} else {
			set_status("Installed. Power-cycle the Amiga to start it.");
			gt_status("Firmware installed; power-cycle the Amiga.");
		}
		return TRUE;
	}
	if (failed_name[0]) {
		snprintf(message, sizeof(message), "Module %s failed: %s (0x%04x). "
			"BOOT.bin was not installed.", failed_name, fwup_strerror(st),
			(unsigned)st);
	} else {
		snprintf(message, sizeof(message), "Firmware install failed: %s (0x%04x)",
			fwup_strerror(st), (unsigned)st);
	}
	set_status("Install failed.");
	gt_ask("OK", "%s", message);
	gt_status("%s", message);
	return FALSE;
}

static void describe_bundle_discovery_failure(const char *prefix,
	const struct fwup_bundle *bundle, UWORD status)
{
	char fault[128];

	if (!bundle->directory_error) {
		snprintf(message, sizeof(message), "%s: %s", prefix,
			fwup_strerror(status));
		return;
	}
	Fault(bundle->directory_error, (STRPTR)"", (STRPTR)fault, sizeof(fault));
	snprintf(message, sizeof(message),
		"%s: cannot scan module drawer %s (DOS error %ld): %s", prefix,
		bundle->module_dir, (long)bundle->directory_error, fault);
}


/* Describe and confirm every file a manual bundle install can change. */
static void install(void)
{
	struct fwup_bundle bundle;
	ULONG used;
	UWORD i;
	UWORD st;
	UWORD caps;

	if (!path[0] || !fwup_supported || transfer_active)
		return;
	st = fwup_bundle_discover(path, &bundle);
	if (st != FWUP_OK) {
		describe_bundle_discovery_failure("Cannot prepare firmware bundle",
			&bundle, st);
		set_status("Bundle not installed.");
		gt_ask("OK", "%s", message);
		return;
	}
	message[0] = '\0';
	used = 0;
	message_append(&used, "Install BOOT.bin from %s?\n\n",
		FilePart((CONST_STRPTR)path));
	if (bundle.module_count) {
		message_append(&used, "Modules installed first:\n");
		for (i = 0; i < bundle.module_count; i++)
			message_append(&used, "%s\n", bundle.module_names[i]);
	} else {
		message_append(&used, "No optional modules found.\n");
	}
	caps = fwup_board_capabilities((ULONG)board.regs);
	if (bundle.module_count && (caps & ZZ_FW_CAP_FWUP_MODULE_DIR) == 0)
		message_append(&used, "\nThis firmware will install BOOT.bin only. "
			"Reboot, then run the install again for modules.\n");
	if (gt_ask("Install|Cancel", "%s\nCurrent image kept as BOOT.bak.\n"
		"Power-cycle Amiga afterwards.", message) != 1)
		return;
	(void)install_bundle(&bundle, NULL);
}

static int wait_keep_running(void *ctx)
{
	return fe_wait_keep_running(ctx);
}

/* Ask GitHub for the latest release and compare it with this card. */
static void check_online(void)
{
	struct fe_wait wait;
	struct fwonline_hooks hooks;
	BOOL cancel = FALSE, cancelled;
	BOOL ok;

	if (transfer_active)
		return;
	transfer_active = TRUE;
	update_gate();
	set_status("Checking GitHub for firmware...");
	hooks.keep_running = wait_keep_running;
	hooks.progress = NULL;
	hooks.ctx = &wait;
	fe_wait_begin(&wait, &cancel);
	gt_busy(TRUE);
	ok = fwonline_check(variant, &hooks, &release, &cancelled, message,
		sizeof(message));
	gt_busy(FALSE);
	fe_wait_end(&wait);
	transfer_active = FALSE;
	release_known = ok;
	render_latest();
	update_gate();
	if (!ok) {
		set_status(cancelled ? "Check cancelled." : "Check failed.");
		gt_status("%s", message);
		return;
	}
	switch (release.compare) {
	case FWREL_NEWER:
		set_status(release.asset_listed ? "A newer firmware is available." :
			"Newer firmware has no image for this board.");
		break;
	case FWREL_PATCH_UNKNOWN:
		set_status("Card cannot report its patch level.");
		break;
	case FWREL_UP_TO_DATE:
		set_status("Latest firmware is installed.");
		break;
	default:
		set_status("Installed firmware is newer than the release.");
		break;
	}
	gt_status("Latest release: %s", release.tag);
}

/* Refuse an image for the other bus; confirm any other build change. When
 * the firmware cannot identify its build (before 2.8), the plain Zorro II
 * or Zorro III build is the reference, so A500 or AGA images still ask. */
static BOOL confirm_variant(void)
{
	BOOL card_z3 = board.zorro == 3;
	int expected = detected;

	if (fwrel_variant_is_zorro3(variant) != card_z3) {
		gt_ask("OK", "The %s image is for Zorro %s, but this card is in "
			"Zorro %s mode.\n\nChoose the matching board.",
			fwrel_variant_name(variant), card_z3 ? "II" : "III",
			card_z3 ? "III" : "II");
		return FALSE;
	}
	if (expected == FWREL_VARIANT_UNKNOWN)
		expected = card_z3 ? FWREL_VARIANT_ZORRO3 : FWREL_VARIANT_ZORRO2;
	if (fwrel_variant_matches(expected, variant))
		return TRUE;
	return gt_ask("Use it|Cancel", "%s %s firmware build.\n"
		"Install the %s build instead?\n\nA build for the wrong machine may "
		"not start; recovery then needs the\nmicroSD card in another "
		"computer.", detected == FWREL_VARIANT_UNKNOWN ?
		"This firmware cannot report its build; the default is the" :
		"This card runs the", fwrel_variant_name(expected),
		fwrel_variant_name(variant)) == 1;
}

/* Download the checked release bundle for the chosen board, then install it. */
static void update_online(void)
{
	struct fe_wait wait;
	struct fwonline_hooks hooks;
	struct fwup_bundle bundle;
	BOOL cancel = FALSE, cancelled;
	BOOL ok;
	UWORD st;
	static char image[256];
	const char *question;

	if (!release_known || !fwup_supported || transfer_active)
		return;
	if (!confirm_variant())
		return;
	switch (release.compare) {
	case FWREL_UP_TO_DATE:
		question = "%s is already installed. Reinstall it?";
		break;
	case FWREL_INSTALLED_NEWER:
		question = "The card runs newer firmware than %s. Install %s anyway?";
		break;
	default:
		question = "Download and install %s?";
		break;
	}
	snprintf(message, sizeof(message), question, release.tag, release.tag);
	if (gt_ask("Install|Cancel", "%s\n\nBoard: %s\nCurrent image kept as "
		"BOOT.bak.\nPower-cycle Amiga afterwards.", message,
		fwrel_variant_label(variant)) != 1)
		return;

	image[0] = '\0';
	transfer_active = TRUE;
	update_gate();
	set_progress("Connecting...");
	set_status("Downloading firmware (Esc cancels)...");
	hooks.keep_running = wait_keep_running;
	hooks.progress = download_progress;
	hooks.ctx = &wait;
	fe_wait_begin(&wait, &cancel);
	gt_busy(TRUE);
	ok = fwonline_download(release.tag, variant, image, sizeof(image), &hooks,
		&cancelled, message, sizeof(message));
	/* A key queued after the last read (or while the final chunk was
	 * inflated) has not been polled yet; poll once more before deciding
	 * to flash. */
	if (ok)
		fe_wait_keep_running(&wait);
	gt_busy(FALSE);
	fe_wait_end(&wait);
	transfer_active = FALSE;
	/* Escape in the last poll of a complete download still cancels: the
	 * user asked not to flash. */
	if (ok && cancel) {
		fwonline_remove_bundle(image);
		image[0] = '\0';
		ok = FALSE;
		cancelled = TRUE;
		snprintf(message, sizeof(message), "%s", "Download cancelled");
	}
	update_gate();
	if (!ok) {
		set_progress("Not downloaded");
		set_status(cancelled ? "Download cancelled." : "Download failed.");
		if (!cancelled)
			gt_ask("OK", "%s", message);
		gt_status("%s", message);
		return;
	}
	st = fwup_bundle_discover(image, &bundle);
	if (st != FWUP_OK) {
		describe_bundle_discovery_failure(
			"Downloaded bundle cannot be installed", &bundle, st);
		set_status("Install failed.");
		gt_ask("OK", "%s", message);
		gt_status("%s", message);
	} else {
		(void)install_bundle(&bundle, release.tag);
	}
	fwonline_remove_bundle(image);
	image[0] = '\0';
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
	set_status("Restoring previous firmware...");
	gt_busy(TRUE);
	st = fwup_restore_board((ULONG)board.regs, "BOOT.bin");
	gt_busy(FALSE);
	transfer_active = FALSE;
	update_gate();
	if (st == FWUP_OK) {
		fwonline_record_install(NULL);
		set_status("Restored. Power-cycle the Amiga to start it.");
		gt_status("Previous firmware restored; power-cycle the Amiga.");
	} else {
		snprintf(message, sizeof(message), "Firmware restore failed: %s (0x%04x)",
			fwup_strerror(st), (unsigned)st);
		set_status("Restore failed.");
		gt_ask("OK", "%s", message);
		gt_status("Firmware restore failed: %s", fwup_strerror(st));
	}
}

/* Create the firmware status, online update, file update and restore
 * controls. */
static BOOL create(struct gt_page *page)
{
	struct gt_form f;
	static const char *const fw_labels[] = {
		"Installed", "Latest", "Board", NULL
	};
	WORD left, width;
	WORD check_w, update_w, choose_w, inst_w, rest_w;
	int i;

	for (i = 0; i < FWREL_VARIANT_COUNT; i++)
		variant_choices[i] = fwrel_variant_label(i);
	variant_choices[FWREL_VARIANT_COUNT] = NULL;
	/* Layout changes recreate the page; keep the user's choice. */
	if (!variant_chosen) {
		variant_chosen = TRUE;
		detected = board_variant();
		if (detected != FWREL_VARIANT_UNKNOWN)
			variant = detected;
		else
			variant = board.zorro == 3 ? FWREL_VARIANT_ZORRO3 :
				FWREL_VARIANT_ZORRO2;
	}
	variant_shown = -1;

	gt_page_begin(page);
	gt_form_init(&f, page, fw_labels);
	left = f.left;
	width = f.width;

	g_installed = gt_form_field(&f, G_INSTALLED, 0, "Installed", installed);
	g_support = gt_add_note(page, G_SUPPORT, left, 1, width, support_text);
	g_latest = gt_form_field(&f, G_LATEST, 2, "Latest", latest_text);
	g_variant = gt_form_cycle(&f, G_VARIANT, 3, "Board", variant_choices);

	check_w = gt_button_width("Check online");
	update_w = gt_button_width("Download and install");
	g_check = gt_add_button(page, G_CHECK, left, 4, check_w, "Check online");
	g_update = gt_add_button(page, G_UPDATE, (WORD)(left + check_w + 8), 4,
		update_w, "Download and install");

	choose_w = gt_button_width("Choose file...");
	g_choose = gt_add_button(page, G_CHOOSE, left, 5, choose_w, "Choose file...");
	g_file = gt_add_note(page, G_FILE, (WORD)(left + choose_w + 8), 5,
		(WORD)(width - choose_w - 8), file_text);

	inst_w = gt_button_width("Install");
	g_install = gt_add_button(page, G_INSTALL, left, 6, inst_w, "Install");
	g_progress = gt_add_note(page, G_PROGRESS, (WORD)(left + inst_w + 8), 6,
		(WORD)(width - inst_w - 8), progress_text);

	g_status = gt_add_note(page, G_STATUS, left, 7, width, status_note);

	rest_w = gt_button_width("Restore previous firmware");
	g_restore = gt_add_button(page, G_RESTORE, left, 8, rest_w, "Restore previous firmware");
	g_restore_note = gt_add_note(page, G_RESTORE_NOTE, left, 9, width, restore_note);

	return g_installed && g_support && g_latest && g_variant && g_check &&
		g_update && g_choose && g_file && g_install && g_progress &&
		g_status && g_restore && g_restore_note;
}

/* Probe firmware update support when this page is shown. */
static void show(void)
{
	probe();
}

/* Dispatch a firmware page action selected by the user. */
static void gadget(UWORD id, UWORD code)
{
	if (id == G_VARIANT) {
		if (code < FWREL_VARIANT_COUNT)
			variant = variant_shown = code;
	} else if (id == G_CHECK) {
		check_online();
	} else if (id == G_UPDATE) {
		update_online();
	} else if (id == G_CHOOSE) {
		/* A cancelled or too-long pick must not disturb the current image. */
		static char picked[sizeof(path)];

		if (fwup_supported && gt_pick_file("Select firmware BOOT.bin",
			"BOOT.bin", picked, sizeof(picked))) {
			memcpy(path, picked, sizeof(path));
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
	board_fw_version_text(installed, sizeof(installed));
	gt_page_set_text(g_installed, installed);
	render_latest();
	if (variant_shown != variant) {
		variant_shown = variant;
		set_attr(g_variant, GTCY_Active, (ULONG)variant);
	}
	if (path[0]) {
		snprintf(file_text, sizeof(file_text), "%s", (const char *)FilePart((CONST_STRPTR)path));
		gt_page_set_text(g_file, file_text);
	}
	update_gate();
}

struct gt_page gt_page_firmware = {
	"Firmware",
	"Firmware",
	"Update online, install a file, or go back to the previous image.",
	0,
	NULL,
	NULL,
	10,
	create,
	show,
	NULL,
	gadget,
	NULL,
	sync,
	NULL,
	NULL
};
