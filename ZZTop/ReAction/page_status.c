/*
 * ZZTop Status page: what the card is, how it is doing, what it can do.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <exec/types.h>
#include <intuition/classusr.h>
#include <gadgets/layout.h>
#include <gadgets/space.h>
#include <images/bevel.h>
#include <images/label.h>
#include <reaction/reaction_macros.h>
#include <proto/intuition.h>
#include <proto/layout.h>
#include <proto/label.h>
#include <proto/space.h>
#include <proto/label.h>

#include <stdio.h>
#include <string.h>

#include "zztop.h"
#include "audio.h"
#include "board.h"
#include "settings.h"
#include "ui.h"

#define P PAGE_STATUS

static struct ui_text t_card, t_firmware, t_features;
static struct ui_text t_temp, t_vccint, t_vccaux;
static struct ui_text t_audio, t_video, t_sdboot, t_cfgfile;
static struct ui_text t_pending;

static Object *row(const char *label, struct ui_text *t)
{
	(void)label;
	return ui_text_new(t, "");
}

static Object *create(void)
{
	return VGroupObject,
		LAYOUT_SpaceOuter, TRUE,
		LAYOUT_AddChild, VGroupObject,
			LAYOUT_BevelStyle, BVS_GROUP,
			LAYOUT_Label, "Card",
			LAYOUT_AddChild, row("Model", &t_card),
			Label("Model"),
			LAYOUT_AddChild, row("Firmware", &t_firmware),
			Label("Firmware"),
			LAYOUT_AddChild, row("Features", &t_features),
			Label("Features"),
		End,
		LAYOUT_AddChild, VGroupObject,
			LAYOUT_BevelStyle, BVS_GROUP,
			LAYOUT_Label, "Health",
			LAYOUT_AddChild, row("Temperature", &t_temp),
			Label("FPGA temperature"),
			LAYOUT_AddChild, row("VCCINT", &t_vccint),
			Label("Core voltage"),
			LAYOUT_AddChild, row("VCCAUX", &t_vccaux),
			Label("Aux voltage"),
		End,
		LAYOUT_AddChild, VGroupObject,
			LAYOUT_BevelStyle, BVS_GROUP,
			LAYOUT_Label, "Devices",
			LAYOUT_AddChild, row("Audio", &t_audio),
			Label("Audio"),
			LAYOUT_AddChild, row("Native video", &t_video),
			Label("Native video in"),
			LAYOUT_AddChild, row("SD boot", &t_sdboot),
			Label("SD card boot"),
			LAYOUT_AddChild, row("Config", &t_cfgfile),
			Label("ZZ9000.CFG"),
		End,
		LAYOUT_AddChild, ui_note_new(&t_pending, ""),
		CHILD_WeightedHeight, 0,
		LAYOUT_AddChild, SpaceObject, End,
	End;
}

static void features_text(char *buf, size_t size)
{
	size_t n = 0;

	buf[0] = '\0';
#define FEATURE(cond, name) \
	if (cond) n += snprintf(buf + n, n < size ? size - n : 0, "%s%s", \
		n ? ", " : "", name)
	FEATURE(board_has_cap(ZZ_FW_CAP_VIDEOCAP_LIVE), "live calibration");
	FEATURE(board_has_cap(ZZ_FW_CAP_VIDEOCAP_CENTERED_1080P), "1080p");
	FEATURE(board_has_cap(ZZ_FW_CAP_VIDEOCAP_SOURCE_SYNC), "Amiga sync");
	FEATURE(board_has_cfg(), "card settings");
	FEATURE(board.zz9k_library, "SDK services");
#undef FEATURE
	if (!buf[0]) snprintf(buf, size, "basic RTG");
}

static void refresh_health(void)
{
	struct board_health h;

	board_sample_health(&h);
	ui_text_printf(P, &t_temp, "%d.%d \260C   (%d.%d to %d.%d this session)",
		h.temp_tenths / 10, h.temp_tenths % 10,
		h.temp_min_tenths / 10, h.temp_min_tenths % 10,
		h.temp_max_tenths / 10, h.temp_max_tenths % 10);
	ui_text_printf(P, &t_vccint, "%u.%02u V", h.vccint_centi / 100,
		h.vccint_centi % 100);
	ui_text_printf(P, &t_vccaux, "%u.%02u V", h.vccaux_centi / 100,
		h.vccaux_centi % 100);

	if (!h.ax_present)
		ui_text_set(P, &t_audio, "No ZZ9000AX daughterboard");
	else if (audio_available())
		ui_text_set(P, &t_audio, "ZZ9000AX, mixer available");
	else
		ui_text_printf(P, &t_audio, "ZZ9000AX (%s)",
			audio_unavailable_reason());

	if (!h.vcap_stats_valid)
		ui_text_set(P, &t_video, "Not reported by this firmware");
	else if ((h.vcap_stats & ZZ_VCAP_LINES_MASK) == 0)
		ui_text_set(P, &t_video, "No signal");
	else
		ui_text_printf(P, &t_video, "%u lines per field",
			(unsigned)(h.vcap_stats & ZZ_VCAP_LINES_MASK));
}

static void sync(void)
{
	char features[UI_TEXT_MAX];
	struct app_pending p;

	board_refresh_identity();
	ui_text_printf(P, &t_card, "ZZ9000 on Zorro %s",
		board.zorro == 3 ? "III" : "II");
	ui_text_printf(P, &t_firmware, "%u.%u", board.fw_version >> 8,
		board.fw_version & 0xff);
	features_text(features, sizeof(features));
	ui_text_set(P, &t_features, features);

	if (!cfg.supported) {
		ui_text_set(P, &t_cfgfile, "Needs firmware 2.3 or newer");
		ui_text_set(P, &t_sdboot, "-");
	} else {
		if (cfg.read_status == ZZ_CFG_FILE_OK)
			ui_text_printf(P, &t_cfgfile, "%u bytes on the SD card",
				(unsigned)cfg.file_bytes);
		else if (cfg.read_status == ZZ_CFG_FILE_NO_FILE)
			ui_text_set(P, &t_cfgfile, "Not created yet (defaults)");
		else
			ui_text_set(P, &t_cfgfile, "Could not be read");
		if (!cfg.saved.hdf[0])
			ui_text_printf(P, &t_sdboot, "%s (default)",
				ZZCFG_HDF_DEFAULT);
		else if (zzcfg_hdf_gadget_text(cfg.saved.hdf)[0] == '\0')
			ui_text_set(P, &t_sdboot, "Off");
		else
			ui_text_set(P, &t_sdboot, cfg.saved.hdf);
	}

	app_pending(&p);
	if (p.card || p.audio || p.live)
		ui_text_set(P, &t_pending,
			"There are unsaved changes: Save keeps them, Revert "
			"undoes them.");
	else
		ui_text_set(P, &t_pending, "");
	refresh_health();
}

static void tick(void)
{
	/* Registers are cheap; twice a second keeps it readable. */
	if (ui_ticks() % 5 == 0) refresh_health();
}

struct ui_page page_status = {
	"Status",
	"Card status",
	"Identity, health and attached hardware of this ZZ9000.",
	0,
	create,
	sync,
	NULL,
	NULL,
	tick,
	sync,
	NULL,
	NULL,
};
