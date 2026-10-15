/*
 * ZZTop ZZ9000.CFG editor model.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * One model for every card-file setting, whatever page shows it.
 *
 *  saved  the card file as last read (raw SD file, not the firmware's
 *         cold-boot parse: that would revert edits made since boot)
 *  edit   saved plus the user's pending edits
 *
 * A field is dirty while its edit value (including presence) differs from
 * saved, so changing a setting back clears it. Save re-reads the card file
 * immediately before writing and applies only dirty fields on top: keys
 * other writers changed meanwhile (the firmware audio Save, a manual edit)
 * survive, and an untouched absent key stays absent.
 */
#ifndef ZZTOP_SETTINGS_H
#define ZZTOP_SETTINGS_H

#include <exec/types.h>
#include "zzcfg_amiga.h"

enum cfg_field {
	CFG_PROFILE = 0,    /* videocap_profile (output + refresh) */
	CFG_SAMPLE,         /* videocap_sample */
	CFG_SCAN_MODE,      /* scanline_mode */
	CFG_SCAN_PARITY,    /* scanline_parity */
	CFG_CROP_H,         /* videocap_crop_h, absent = Automatic */
	CFG_CROP_V,
	CFG_PHASE_E7M,      /* videocap_phase (legacy E7M steps) */
	CFG_PHASE_C28,      /* videocap_c28_phase */
	CFG_WIDTH,          /* videocap_width, absent = Automatic */
	CFG_HEIGHT,
	CFG_INT2,
	CFG_MAC,
	CFG_HDF,
	CFG_OFFSCREEN,
	CFG_OVERLAY,
	CFG_FAST_RAM,
	CFG_FIELD_COUNT
};

#define CFG_BIT(f) (1UL << (f))

/* Fields that only take effect at the next cold boot. Scanlines and the
 * capture calibration fields also drive live hardware. */
#define CFG_REBOOT_FIELDS (CFG_BIT(CFG_PROFILE) | CFG_BIT(CFG_SAMPLE) | \
	CFG_BIT(CFG_INT2) | CFG_BIT(CFG_MAC) | CFG_BIT(CFG_HDF) | \
	CFG_BIT(CFG_OFFSCREEN) | CFG_BIT(CFG_OVERLAY) | CFG_BIT(CFG_FAST_RAM))

/* ENV: variables the drivers apply over the card file. */
#define CFG_ENV_INT2   0x0001   /* ENV:ZZ9K_INT2 */
#define CFG_ENV_MAC    0x0002   /* ENV:ZZ9K_MAC */
#define CFG_ENV_NATIVE 0x0004   /* ZZ9000-VCAP-800x600, ZZ9000-NS-VSYNC[-NTSC] */

struct settings_state {
	BOOL supported;          /* firmware has the CFG mailbox (2.3+) */
	UWORD read_status;       /* ZZ_CFG_FILE_* of the last read */
	UWORD file_bytes;
	struct zzcfg_values saved;
	struct zzcfg_values edit;
	ULONG dirty;             /* CFG_BIT(field) set */
	UWORD env;               /* CFG_ENV_* currently present */
	char env_mac[ZZCFG_MAC_CHARS + 3];  /* valid when CFG_ENV_MAC */
	UWORD env_profile;       /* profile the native ENV switches select */
	enum zzcfg_fast_ram_state fast_ram_state;
	UWORD fast_ram_outcome;  /* raw ZZ_CFG_FAST_RAM_OUTCOME_* */
};

extern struct settings_state cfg;

/* Read the card file and drop every edit. */
void settings_load(void);

/* Edits. Setting a value makes the key present (an explicit override);
 * settings_clear removes it (Automatic / firmware default). */
void settings_set(enum cfg_field f, LONG value);
void settings_set_text(enum cfg_field f, const char *text);
void settings_clear(enum cfg_field f);
/* Drop the edit of one field (back to the saved value). */
void settings_reset(enum cfg_field f);
BOOL settings_present(const struct zzcfg_values *v, enum cfg_field f);
LONG settings_value(const struct zzcfg_values *v, enum cfg_field f);

UWORD settings_dirty_count(void);
UWORD settings_reboot_count(void);

/* Check the pending text fields with the firmware's own rules. Returns
 * FALSE with *bad and a message for the first invalid one. */
BOOL settings_validate(enum cfg_field *bad, char *msg, UWORD size);

/* Write the dirty fields over a fresh read of the card file.
 * Returns an FWUP status; FWUP_OK reloads the model. */
UWORD settings_save(void);

/* ENV overrides. adopt copies the ENV value into the edit (so Save
 * migrates it into the card file); remove deletes the ENV: and ENVARC:
 * copies. Both re-scan cfg.env. */
void settings_env_scan(void);
void settings_env_adopt(UWORD which);
BOOL settings_env_remove(UWORD which);

/* The firmware audio Save regenerates the non-audio keys from its own
 * cold-boot parse, which reverts any card-file change made since boot.
 * Bracket every audio Save with a guard:
 *  begin  snapshots the card file. FALSE when the file exists but cannot
 *         be read: do not start an unprotected audio save.
 *  end    puts the snapshot's non-audio keys back under the audio keys the
 *         firmware wrote, when it changed them. FWUP_OK clears the guard;
 *         any error keeps the snapshot so end can be retried later.
 * guard.valid tells whether a repair is still owed. */
struct settings_guard {
	BOOL valid;
	struct zzcfg_values before;
};
BOOL settings_guard_begin(struct settings_guard *guard);
UWORD settings_guard_end(struct settings_guard *guard);

/* Helpers shared with pages. */
BOOL settings_mac_valid(const char *text);

#endif /* ZZTOP_SETTINGS_H */
