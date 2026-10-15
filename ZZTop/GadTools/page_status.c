/*
 * ZZTop GadTools Status page: card identity, health and peripheral status.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <exec/types.h>
#include <intuition/intuition.h>
#include <libraries/gadtools.h>
#include <proto/gadtools.h>
#include <stdio.h>
#include <string.h>

#include "board.h"
#include "settings.h"
#include "audio.h"
#include "zztop.h"
#include "gt.h"

#define P GT_PAGE_STATUS

enum {
	G_CARD = 1,
	G_FIRMWARE,
	G_FEATURES,
	G_TEMP,
	G_VOLTAGES,
	G_AUDIO,
	G_VIDEO,
	G_SDBOOT,
	G_CFGFILE,
	G_PENDING
};

#define STATUS_BUF_SIZE 128

static char buf_card[STATUS_BUF_SIZE];
static char buf_firmware[STATUS_BUF_SIZE];
static char buf_features[STATUS_BUF_SIZE];
static char buf_temp[STATUS_BUF_SIZE];
static char buf_voltages[64];
static char buf_audio[STATUS_BUF_SIZE];
static char buf_video[STATUS_BUF_SIZE];
static char buf_sdboot[STATUS_BUF_SIZE];
static char buf_cfgfile[STATUS_BUF_SIZE];
static char buf_pending[STATUS_BUF_SIZE];

static struct Gadget *g_card;
static struct Gadget *g_firmware;
static struct Gadget *g_features;
static struct Gadget *g_temp;
static struct Gadget *g_voltages;
static struct Gadget *g_audio;
static struct Gadget *g_video;
static struct Gadget *g_sdboot;
static struct Gadget *g_cfgfile;
static struct Gadget *g_pending;

static const char *const status_labels[] = {
	"Model",
	"Firmware",
	"Features",
	"Temperature",
	"Voltages",
	"Audio",
	"Video capture",
	"SD card boot",
	"ZZ9000.CFG",
	NULL
};

/* Format the advertised hardware and firmware features. */
static void features_text(char *buf, size_t size)
{
	size_t n = 0;

	buf[0] = '\0';
	if (board_has_cap(ZZ_FW_CAP_VIDEOCAP_LIVE)) {
		n += (size_t)snprintf(buf + n, n < size ? size - n : 0, "%s%s",
			n ? ", " : "", "live pic");
	}
	if (board_has_cap(ZZ_FW_CAP_VIDEOCAP_CENTERED_1080P)) {
		n += (size_t)snprintf(buf + n, n < size ? size - n : 0, "%s%s",
			n ? ", " : "", "1080p");
	}
	if (board_has_cap(ZZ_FW_CAP_VIDEOCAP_SOURCE_SYNC)) {
		n += (size_t)snprintf(buf + n, n < size ? size - n : 0, "%s%s",
			n ? ", " : "", "sync");
	}
	if (board_has_cfg()) {
		n += (size_t)snprintf(buf + n, n < size ? size - n : 0, "%s%s",
			n ? ", " : "", "config");
	}
	if (board.zz9k_library) {
		n += (size_t)snprintf(buf + n, n < size ? size - n : 0, "%s%s",
			n ? ", " : "", "SDK");
	}
	if (!buf[0]) {
		snprintf(buf, size, "%s", "basic RTG");
	}
}

/* Sample temperature, voltages, and attached hardware status. */
static void refresh_health(void)
{
	struct board_health h;

	board_sample_health(&h);
	snprintf(buf_temp, sizeof(buf_temp),
		"%d.%d C  (%d.%d to %d.%d range)",
		h.temp_tenths / 10, h.temp_tenths % 10,
		h.temp_min_tenths / 10, h.temp_min_tenths % 10,
		h.temp_max_tenths / 10, h.temp_max_tenths % 10);
	gt_page_set_text(g_temp, buf_temp);

	snprintf(buf_voltages, sizeof(buf_voltages), "Core %u.%02u V,  Aux %u.%02u V",
		h.vccint_centi / 100, h.vccint_centi % 100,
		h.vccaux_centi / 100, h.vccaux_centi % 100);
	gt_page_set_text(g_voltages, buf_voltages);

	if (!h.ax_present) {
		snprintf(buf_audio, sizeof(buf_audio), "%s", "ZZ9000AX not present");
	} else if (audio_available()) {
		snprintf(buf_audio, sizeof(buf_audio), "%s", "ZZ9000AX present, mixer OK");
	} else {
		snprintf(buf_audio, sizeof(buf_audio), "ZZ9000AX (%s)", audio_unavailable_reason());
	}
	gt_page_set_text(g_audio, buf_audio);

	if (!h.vcap_stats_valid) {
		snprintf(buf_video, sizeof(buf_video), "%s", "Not reported by firmware");
	} else if ((h.vcap_stats & ZZ_VCAP_LINES_MASK) == 0) {
		snprintf(buf_video, sizeof(buf_video), "%s", "No signal");
	} else {
		snprintf(buf_video, sizeof(buf_video), "%u lines per field",
			(unsigned)(h.vcap_stats & ZZ_VCAP_LINES_MASK));
	}
	gt_page_set_text(g_video, buf_video);
}

/* Synchronize identity, configuration status and health readouts. */
static void sync(void)
{
	struct app_pending p;

	board_refresh_identity();
	snprintf(buf_card, sizeof(buf_card), "ZZ9000 on Zorro %s",
		board.zorro == 3 ? "III" : "II");
	gt_page_set_text(g_card, buf_card);

	board_fw_version_text(buf_firmware, sizeof(buf_firmware));
	gt_page_set_text(g_firmware, buf_firmware);

	features_text(buf_features, sizeof(buf_features));
	gt_page_set_text(g_features, buf_features);

	if (!cfg.supported) {
		snprintf(buf_cfgfile, sizeof(buf_cfgfile), "%s", "Needs firmware 2.3+");
		snprintf(buf_sdboot, sizeof(buf_sdboot), "%s", "-");
	} else {
		if (cfg.read_status == ZZ_CFG_FILE_OK) {
			snprintf(buf_cfgfile, sizeof(buf_cfgfile), "%u bytes on SD card",
				(unsigned)cfg.file_bytes);
		} else if (cfg.read_status == ZZ_CFG_FILE_NO_FILE) {
			snprintf(buf_cfgfile, sizeof(buf_cfgfile), "%s", "No file (defaults)");
		} else {
			snprintf(buf_cfgfile, sizeof(buf_cfgfile), "%s", "Read error");
		}
		if (!cfg.saved.hdf[0]) {
			snprintf(buf_sdboot, sizeof(buf_sdboot), "%s (default)", ZZCFG_HDF_DEFAULT);
		} else if (zzcfg_hdf_gadget_text(cfg.saved.hdf)[0] == '\0') {
			snprintf(buf_sdboot, sizeof(buf_sdboot), "%s", "Off");
		} else {
			snprintf(buf_sdboot, sizeof(buf_sdboot), "%s", cfg.saved.hdf);
		}
	}
	gt_page_set_text(g_cfgfile, buf_cfgfile);
	gt_page_set_text(g_sdboot, buf_sdboot);

	app_pending(&p);
	if (p.card || p.audio || p.live) {
		snprintf(buf_pending, sizeof(buf_pending), "%s",
			"Save keeps pending changes, Revert undoes them.");
	} else {
		buf_pending[0] = '\0';
	}
	gt_page_set_text(g_pending, buf_pending);

	refresh_health();
}

/* Heartbeat timer poll for live health registers. */
static void tick(void)
{
	if (gt_ticks() % 5 == 0) {
		refresh_health();
	}
}

/* Create the Status page gadget list. */
static BOOL create(struct gt_page *page)
{
	struct gt_form f;

	sync();
	gt_page_begin(page);
	gt_form_init(&f, page, status_labels);

	g_card = gt_form_field(&f, G_CARD, 0, "Model", buf_card);
	g_firmware = gt_form_field(&f, G_FIRMWARE, 1, "Firmware", buf_firmware);
	g_features = gt_form_field(&f, G_FEATURES, 2, "Features", buf_features);
	g_temp = gt_form_field(&f, G_TEMP, 3, "Temperature", buf_temp);
	g_voltages = gt_form_field(&f, G_VOLTAGES, 4, "Voltages", buf_voltages);
	g_audio = gt_form_field(&f, G_AUDIO, 5, "Audio", buf_audio);
	g_video = gt_form_field(&f, G_VIDEO, 6, "Video capture", buf_video);
	g_sdboot = gt_form_field(&f, G_SDBOOT, 7, "SD card boot", buf_sdboot);
	g_cfgfile = gt_form_field(&f, G_CFGFILE, 8, "ZZ9000.CFG", buf_cfgfile);
	g_pending = gt_add_note(page, G_PENDING, f.left, 9, f.width, buf_pending);

	if (!g_card || !g_firmware || !g_features || !g_temp || !g_voltages ||
		!g_audio || !g_video || !g_sdboot || !g_cfgfile || !g_pending) {
		return FALSE;
	}

	page->rows = 10;
	return TRUE;
}

/* Reset gadget pointers upon destruction. */
static void cleanup(void)
{
	g_card = NULL;
	g_firmware = NULL;
	g_features = NULL;
	g_temp = NULL;
	g_voltages = NULL;
	g_audio = NULL;
	g_video = NULL;
	g_sdboot = NULL;
	g_cfgfile = NULL;
	g_pending = NULL;
}

struct gt_page gt_page_status = {
	"Status",
	"Card status",
	"Identity, health and attached hardware of this ZZ9000.",
	0,
	NULL,
	NULL,
	10,
	create,
	NULL,
	NULL,
	NULL,
	tick,
	sync,
	NULL,
	cleanup
};
