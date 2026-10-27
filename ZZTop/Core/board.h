/*
 * ZZTop board access: discovery, register I/O and health readout.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ZZTOP_BOARD_H
#define ZZTOP_BOARD_H

#include <exec/types.h>
#include "zz9000_hw.h"

/* The ZZ9000 found at startup. `regs` is the Zorro register window
 * (ConfigDev cd_BoardAddr); every other module goes through it. */
struct board {
	volatile UBYTE *regs;
	UBYTE zorro;            /* 2 or 3 */
	UWORD hw_version;
	UWORD fw_version;       /* major << 8 | minor */
	UWORD fw_patch;         /* release patch, valid when fw_patch_known */
	BOOL fw_patch_known;    /* ZZ_FW_CAP_FW_VERSION_PATCH advertised */
	UWORD fw_caps;          /* ZZ_REG_FW_CAPABILITIES */
	ULONG zz9k_caps;        /* zz9k.library ZZ9KQueryCaps bits, 0 without */
	BOOL zz9k_library;      /* zz9k.library opened and answered */
};

extern struct board board;

/* Find the card; FALSE when no ZZ9000 is configured. */
BOOL board_find(void);

/* Re-read the firmware version and capability registers (they are
 * fixed per boot, but cheap and the source of truth). */
void board_refresh_identity(void);

/* "2.8.1", or "2.8" when the firmware cannot report its patch level. */
void board_fw_version_text(char *buf, ULONG size);

/* The running bitstream's release variant (enum fwrel_variant), or
 * FWREL_VARIANT_UNKNOWN before firmware 2.8 or when unidentified. */
int board_variant(void);

UWORD board_read16(ULONG offset);
void board_write16(ULONG offset, UWORD value);

/* Firmware generations that gate whole features. */
#define BOARD_FW_AT_LEAST(maj, min) \
	(board.fw_version >= (UWORD)(((maj) << 8) | (min)))
BOOL board_has_cfg(void);          /* ZZ9000.CFG mailbox: firmware 2.3+ */
BOOL board_has_cap(UWORD bit);     /* ZZ_FW_CAP_* */

/* Health and peripherals. Temperatures are tenths of a degree C and
 * voltages hundredths of a volt, both straight from the XADC registers. */
struct board_health {
	WORD temp_tenths;           /* current, lightly smoothed */
	WORD temp_min_tenths;       /* since start or last reset (raw) */
	WORD temp_max_tenths;
	UWORD vccaux_centi;
	UWORD vccint_centi;
	BOOL ax_present;            /* ZZ9000AX audio daughterboard */
	UWORD usb_status;           /* raw ZZ_REG_USB_STATUS */
	UWORD sd_status;            /* raw ZZ_REG_SD_STATUS */
	UWORD sd_boot_status;       /* raw ZZ_REG_SD_BOOT_STATUS */
	BOOL vcap_stats_valid;      /* ZZ_FW_CAP_VIDEOCAP_STATS advertised */
	UWORD vcap_stats;           /* raw ZZ_REG_VIDEOCAP_STATS when valid */
};

/* Sample every health register once; the smoothing and min/max state
 * persist across calls. */
void board_sample_health(struct board_health *out);
void board_reset_temp_range(void);

BOOL board_ax_present(void);

/* Scanline generator (live registers, V2 layout). */
#define BOARD_SCANLINE_MODES 4
UWORD board_scanline_mode(void);
UWORD board_scanline_parity(void);
void board_set_scanline_mode(UWORD mode);
void board_set_scanline_parity(UWORD parity);

/* Output gamma LUT (live, firmware issue #130). board_gamma reads config
 * key 35: the applied gamma in hundredths, 0 when the firmware or the
 * loaded bitstream has no LUT (or before firmware 2.3). board_set_gamma
 * writes the dedicated ZZ_REG_GAMMA register like the ZZGamma CLI; call
 * it only when board_gamma() is nonzero. */
UWORD board_gamma(void);
void board_set_gamma(UWORD hundredths);

/* Re-read the identity registers `reads` times and count mismatches
 * plus impossible values: a read-only Zorro bus integrity probe. */
ULONG board_register_probe(UWORD reads);

#endif /* ZZTOP_BOARD_H */
