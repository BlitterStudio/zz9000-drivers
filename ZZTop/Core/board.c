/*
 * ZZTop board access: discovery, register I/O and health readout.
 * Copyright (C) 2016-2026, Lucie L. Hartmann <lucie@mntre.com>
 *                          MNT Research GmbH, Berlin
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <exec/types.h>
#include <libraries/configvars.h>
#include <proto/exec.h>
#include <proto/expansion.h>

#include <stdio.h>

#include "board.h"
#include "fw_release.h"
#include "zz9000_capture_calibration.h"
#include "zzcfg_query.h"

struct board board;

static WORD temp_smooth = -32768;   /* sentinel: no sample yet */
static WORD temp_min = 32767;
static WORD temp_max = -32768;

BOOL board_find(void)
{
	struct Library *ExpansionBase;
	struct ConfigDev *cd;

	ExpansionBase = OpenLibrary((CONST_STRPTR)"expansion.library", 0);
	if (!ExpansionBase) return FALSE;

	board.zorro = 2;
	cd = FindConfigDev(NULL, ZZ9000_MNT_MANUFACTURER, ZZ9000_PRODUCT_Z2);
	if (!cd) {
		board.zorro = 3;
		cd = FindConfigDev(NULL, ZZ9000_MNT_MANUFACTURER,
			ZZ9000_PRODUCT_Z3);
	}
	if (cd) board.regs = (volatile UBYTE *)cd->cd_BoardAddr;
	CloseLibrary(ExpansionBase);
	if (!cd) return FALSE;

	board_refresh_identity();
	return TRUE;
}

UWORD board_read16(ULONG offset)
{
	return *(volatile UWORD *)(board.regs + offset);
}

void board_write16(ULONG offset, UWORD value)
{
	*(volatile UWORD *)(board.regs + offset) = value;
}

void board_refresh_identity(void)
{
	board.hw_version = board_read16(ZZ_REG_HW_VERSION);
	board.fw_version = board_read16(ZZ_REG_FW_VERSION);
	board.fw_caps = board_read16(ZZ_REG_FW_CAPABILITIES);
	board.fw_patch_known = board_has_cap(ZZ_FW_CAP_FW_VERSION_PATCH);
	board.fw_patch = board.fw_patch_known ?
		board_read16(ZZ_REG_FW_VERSION_PATCH) : 0;
}

void board_fw_version_text(char *buf, ULONG size)
{
	if (board.fw_patch_known)
		snprintf(buf, size, "%u.%u.%u", board.fw_version >> 8,
			board.fw_version & 0xff, board.fw_patch);
	else
		snprintf(buf, size, "%u.%u", board.fw_version >> 8,
			board.fw_version & 0xff);
}

/* Two ordered 16-bit reads, as every other bitstream-register client. */
static ULONG board_read32(ULONG offset)
{
	ULONG high = board_read16(offset);

	return high << 16 | board_read16(offset + 2);
}

int board_variant(void)
{
	/* The field-diagnostic bundle arrived with the 2.8 bitstreams. */
	if (!BOARD_FW_AT_LEAST(2, 8))
		return FWREL_VARIANT_UNKNOWN;
	return fwrel_variant_detect(
		board_read32(ZZ_CAPTURE_FIELD_DIAG_CAP_REG),
		board_read32(ZZ_CAPTURE_FIELD_DIAG_VARIANT_REG),
		board_read32(ZZ_CAPTURE_CAP_REG));
}

BOOL board_has_cfg(void)
{
	return BOARD_FW_AT_LEAST(2, 3);
}

BOOL board_has_cap(UWORD bit)
{
	return (board.fw_caps & bit) != 0;
}

BOOL board_ax_present(void)
{
	return (board_read16(ZZ_REG_AUDIO_CONFIG) & 1) != 0;
}

void board_sample_health(struct board_health *out)
{
	WORD t = (WORD)board_read16(ZZ_REG_TEMPERATURE);

	/* Light exponential smoothing for display (the XADC reading jitters
	 * by a few tenths); min/max track raw samples. */
	if (temp_smooth == -32768)
		temp_smooth = t;
	else
		temp_smooth = (WORD)((t + 9 * (LONG)temp_smooth + 5) / 10);
	if (t < temp_min) temp_min = t;
	if (t > temp_max) temp_max = t;

	out->temp_tenths = temp_smooth;
	out->temp_min_tenths = temp_min;
	out->temp_max_tenths = temp_max;
	out->vccaux_centi = board_read16(ZZ_REG_VOLTAGE_AUX);
	out->vccint_centi = board_read16(ZZ_REG_VOLTAGE_INT);
	out->ax_present = board_ax_present();
	out->usb_status = board_read16(ZZ_REG_USB_STATUS);
	out->sd_status = board_read16(ZZ_REG_SD_STATUS);
	out->sd_boot_status = board_read16(ZZ_REG_SD_BOOT_STATUS);
	/* Older firmware leaves stale bus data at 0x4e: only trust it when
	 * the stats capability is advertised. */
	out->vcap_stats_valid = board_has_cap(ZZ_FW_CAP_VIDEOCAP_STATS);
	out->vcap_stats = out->vcap_stats_valid ?
		board_read16(ZZ_REG_VIDEOCAP_STATS) : 0;
}

void board_reset_temp_range(void)
{
	temp_min = 32767;
	temp_max = -32768;
	if (temp_smooth != -32768) {
		temp_min = temp_smooth;
		temp_max = temp_smooth;
	}
}

UWORD board_scanline_mode(void)
{
	return board_read16(ZZ_SCANLINE_MODE_REG) & 0x3;
}

UWORD board_scanline_parity(void)
{
	return board_read16(ZZ_SCANLINE_PARITY_REG) & 0x1;
}

void board_set_scanline_mode(UWORD mode)
{
	board_write16(ZZ_SCANLINE_MODE_REG, mode & 0x3);
}

void board_set_scanline_parity(UWORD parity)
{
	board_write16(ZZ_SCANLINE_PARITY_REG, parity & 0x1);
}

UWORD board_gamma(void)
{
	if (!board_has_cfg())
		return 0;
	return zzcfg_query((ULONG)board.regs, ZZ_CFG_KEY_GAMMA, NULL);
}

void board_set_gamma(UWORD hundredths)
{
	board_write16(ZZ_REG_GAMMA, hundredths);
}

ULONG board_register_probe(UWORD reads)
{
	UWORD fw = board_read16(ZZ_REG_FW_VERSION);
	UWORD hw = board_read16(ZZ_REG_HW_VERSION);
	ULONG errors = 0;
	UWORD i;

	for (i = 0; i < reads; i++) {
		if (board_read16(ZZ_REG_FW_VERSION) != fw) errors++;
		if (board_read16(ZZ_REG_HW_VERSION) != hw) errors++;
	}
	if (fw == 0 || fw == 0xffff || hw == 0xffff) errors++;
	return errors;
}
