/*
 * ZZTop ZZ9000.CFG editor model.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifdef ZZTOP_SETTINGS_TEST
/* Host tests: AmigaOS types plus in-memory ENV:/ENVARC: files. */
#include "zztop_settings_test_platform.h"
#else
#include <exec/types.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#endif

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "board.h"
#include "fwup_client.h"
#include "settings.h"
#include "zz9000_gamma.h"

struct settings_state cfg;

/* One raw-text buffer for every read; reads and saves never overlap. */
static char raw_text[ZZCFG_MAX_SIZE + 1];

/* ------------------------------------------------------------------ */
/* Reading                                                             */

/* Editor defaults under the card file: what the hardware does when a
 * key is absent. Scanlines and gamma follow the live hardware (the config
 * applied them at boot; ZZScanlines, ZZGamma or a previous session may
 * have changed them). */
static void values_defaults(struct zzcfg_values *v)
{
	memset(v, 0, sizeof(*v));
	v->videocap_profile = ZZCFG_VCAP_FULL_60;
	v->use_videocap_profile_key =
		board_has_cap(ZZ_FW_CAP_VIDEOCAP_PROFILE) ? 1 : 0;
	v->firmware_capabilities = board.fw_caps;
	v->videocap_crop_h = ZZCFG_VIDEOCAP_CROP_H_COMPAT;
	v->videocap_crop_v = ZZCFG_VIDEOCAP_CROP_V_COMPAT;
	v->scanline_mode = board_scanline_mode();
	v->scanline_parity = board_scanline_parity();
	v->gamma = board_gamma();
	if (!v->gamma)
		v->gamma = ZZGAMMA_HUNDREDTHS_IDENTITY;
	/* ZZ9000.card enables both when the key is absent, and the
	 * generator writes every supported key. */
	v->offscreen_bitmaps = 1;
	v->video_overlay = 1;
}

/* Fresh read of the card file into v. Returns the ZZ_CFG_FILE_* status;
 * v holds defaults for anything the file does not set. */
static UWORD read_card(struct zzcfg_values *v, UWORD *bytes,
	BOOL apply_boot_outcome)
{
	UWORD st = ZZ_CFG_FILE_IDLE, len = 0;

	values_defaults(v);
	if (cfg.supported) {
		st = zzcfg_read_raw((ULONG)board.regs, raw_text,
			sizeof(raw_text), &len);
		if (st == ZZ_CFG_FILE_OK)
			zzcfg_parse_text(raw_text, len, v);
		/* A configured-on Fast RAM that the boot gate rejected as
		 * malformed must not be rewritten as a clean preference by an
		 * unrelated save. */
		if (apply_boot_outcome)
			zzcfg_fast_ram_invalidate_withheld(v, cfg.fast_ram_outcome);
	}
	if (bytes) *bytes = len;
	return st;
}

static void fast_ram_scan(void)
{
	cfg.fast_ram_outcome = ZZ_CFG_FAST_RAM_OUTCOME_PENDING;
	cfg.fast_ram_state = ZZCFG_FAST_RAM_UNAVAILABLE;
	if (cfg.supported)
		cfg.fast_ram_state = zzcfg_fast_ram_effective_state(
			(ULONG)board.regs, &cfg.fast_ram_outcome);
}

void settings_load(void)
{
	cfg.supported = board_has_cfg();
	fast_ram_scan();
	cfg.read_status = read_card(&cfg.saved, &cfg.file_bytes, TRUE);
	cfg.edit = cfg.saved;
	cfg.dirty = 0;
	settings_env_scan();
}

/* ------------------------------------------------------------------ */
/* Field access                                                        */

BOOL settings_present(const struct zzcfg_values *v, enum cfg_field f)
{
	switch (f) {
	case CFG_PROFILE:     return v->videocap_profile_present != 0;
	case CFG_SAMPLE:      return v->videocap_sample_present != 0;
	case CFG_SCAN_MODE:   return v->scanline_mode_present != 0;
	case CFG_SCAN_PARITY: return v->scanline_parity_present != 0;
	case CFG_GAMMA:       return v->gamma_present != 0;
	case CFG_CROP_H:      return v->videocap_crop_h_present != 0;
	case CFG_CROP_V:      return v->videocap_crop_v_present != 0;
	case CFG_PHASE_E7M:   return v->videocap_phase_present != 0;
	case CFG_PHASE_C28:   return v->videocap_c28_phase_present != 0;
	case CFG_WIDTH:       return v->videocap_width_present != 0;
	case CFG_HEIGHT:      return v->videocap_height_present != 0;
	case CFG_MAC:         return v->mac[0] != '\0';
	case CFG_HDF:         return v->hdf[0] != '\0';
	case CFG_FAST_RAM:    return v->fast_ram_present && !v->fast_ram_invalid;
	default:              return TRUE;    /* always written */
	}
}

LONG settings_value(const struct zzcfg_values *v, enum cfg_field f)
{
	switch (f) {
	case CFG_PROFILE:     return v->videocap_profile;
	case CFG_SAMPLE:      return v->videocap_sample;
	case CFG_SCAN_MODE:   return v->scanline_mode;
	case CFG_SCAN_PARITY: return v->scanline_parity;
	case CFG_GAMMA:       return v->gamma;
	case CFG_CROP_H:      return v->videocap_crop_h;
	case CFG_CROP_V:      return v->videocap_crop_v;
	case CFG_PHASE_E7M:   return v->videocap_phase;
	case CFG_PHASE_C28:   return v->videocap_c28_phase;
	case CFG_WIDTH:       return v->videocap_width;
	case CFG_HEIGHT:      return v->videocap_height;
	case CFG_INT2:        return v->int2;
	case CFG_OFFSCREEN:   return v->offscreen_bitmaps;
	case CFG_OVERLAY:     return v->video_overlay;
	case CFG_FAST_RAM:    return v->fast_ram;
	default:              return 0;
	}
}

static BOOL field_equal(const struct zzcfg_values *a,
	const struct zzcfg_values *b, enum cfg_field f)
{
	switch (f) {
	case CFG_MAC:
		return strcmp(a->mac, b->mac) == 0;
	case CFG_HDF:
		return strcmp(a->hdf, b->hdf) == 0;
	case CFG_FAST_RAM:
		return a->fast_ram_present == b->fast_ram_present &&
			a->fast_ram_invalid == b->fast_ram_invalid &&
			(!a->fast_ram_present || a->fast_ram == b->fast_ram);
	default:
		if (settings_present(a, f) != settings_present(b, f))
			return FALSE;
		/* Two absent keys are the same setting whatever value the
		 * editor holds behind them. */
		if (!settings_present(a, f)) return TRUE;
		return settings_value(a, f) == settings_value(b, f);
	}
}

/* Copy one field (value and presence) from src to dst. */
static void field_copy(struct zzcfg_values *dst,
	const struct zzcfg_values *src, enum cfg_field f)
{
	switch (f) {
	case CFG_PROFILE:
		dst->videocap_profile = src->videocap_profile;
		dst->videocap_profile_present = src->videocap_profile_present;
		break;
	case CFG_SAMPLE:
		dst->videocap_sample = src->videocap_sample;
		dst->videocap_sample_present = src->videocap_sample_present;
		break;
	case CFG_SCAN_MODE:
		dst->scanline_mode = src->scanline_mode;
		dst->scanline_mode_present = src->scanline_mode_present;
		break;
	case CFG_SCAN_PARITY:
		dst->scanline_parity = src->scanline_parity;
		dst->scanline_parity_present = src->scanline_parity_present;
		break;
	case CFG_GAMMA:
		dst->gamma = src->gamma;
		dst->gamma_present = src->gamma_present;
		break;
	case CFG_CROP_H:
		dst->videocap_crop_h = src->videocap_crop_h;
		dst->videocap_crop_h_present = src->videocap_crop_h_present;
		break;
	case CFG_CROP_V:
		dst->videocap_crop_v = src->videocap_crop_v;
		dst->videocap_crop_v_present = src->videocap_crop_v_present;
		break;
	case CFG_PHASE_E7M:
		dst->videocap_phase = src->videocap_phase;
		dst->videocap_phase_present = src->videocap_phase_present;
		break;
	case CFG_PHASE_C28:
		dst->videocap_c28_phase = src->videocap_c28_phase;
		dst->videocap_c28_phase_present =
			src->videocap_c28_phase_present;
		break;
	case CFG_WIDTH:
		dst->videocap_width = src->videocap_width;
		dst->videocap_width_present = src->videocap_width_present;
		break;
	case CFG_HEIGHT:
		dst->videocap_height = src->videocap_height;
		dst->videocap_height_present = src->videocap_height_present;
		break;
	case CFG_INT2:
		dst->int2 = src->int2;
		break;
	case CFG_MAC:
		memcpy(dst->mac, src->mac, sizeof(dst->mac));
		break;
	case CFG_HDF:
		memcpy(dst->hdf, src->hdf, sizeof(dst->hdf));
		break;
	case CFG_OFFSCREEN:
		dst->offscreen_bitmaps = src->offscreen_bitmaps;
		break;
	case CFG_OVERLAY:
		dst->video_overlay = src->video_overlay;
		break;
	case CFG_FAST_RAM:
		dst->fast_ram = src->fast_ram;
		dst->fast_ram_present = src->fast_ram_present;
		dst->fast_ram_invalid = src->fast_ram_invalid;
		break;
	default:
		break;
	}
}

static void update_dirty(enum cfg_field f)
{
	if (field_equal(&cfg.edit, &cfg.saved, f))
		cfg.dirty &= ~CFG_BIT(f);
	else
		cfg.dirty |= CFG_BIT(f);
}

void settings_set(enum cfg_field f, LONG value)
{
	struct zzcfg_values *v = &cfg.edit;

	switch (f) {
	case CFG_PROFILE:
		v->videocap_profile = (UWORD)value;
		v->videocap_profile_present = 1;
		break;
	case CFG_SAMPLE:
		v->videocap_sample = (UWORD)value;
		v->videocap_sample_present = 1;
		break;
	case CFG_SCAN_MODE:
		v->scanline_mode = (UWORD)value;
		v->scanline_mode_present = 1;
		break;
	case CFG_SCAN_PARITY:
		v->scanline_parity = (UWORD)value;
		v->scanline_parity_present = 1;
		break;
	case CFG_GAMMA:
		v->gamma = (UWORD)value;
		v->gamma_present = 1;
		break;
	case CFG_CROP_H:
		v->videocap_crop_h = (UWORD)value;
		v->videocap_crop_h_present = 1;
		break;
	case CFG_CROP_V:
		v->videocap_crop_v = (UWORD)value;
		v->videocap_crop_v_present = 1;
		break;
	case CFG_PHASE_E7M:
		v->videocap_phase = (WORD)value;
		v->videocap_phase_present = 1;
		break;
	case CFG_PHASE_C28:
		v->videocap_c28_phase = (WORD)value;
		v->videocap_c28_phase_present = 1;
		break;
	case CFG_WIDTH:
		v->videocap_width = (UWORD)value;
		v->videocap_width_present = 1;
		break;
	case CFG_HEIGHT:
		v->videocap_height = (UWORD)value;
		v->videocap_height_present = 1;
		break;
	case CFG_INT2:
		v->int2 = value ? 1 : 0;
		break;
	case CFG_OFFSCREEN:
		v->offscreen_bitmaps = value ? 1 : 0;
		break;
	case CFG_OVERLAY:
		v->video_overlay = value ? 1 : 0;
		break;
	case CFG_FAST_RAM:
		v->fast_ram = value ? 1 : 0;
		v->fast_ram_present = 1;
		v->fast_ram_invalid = 0;
		break;
	default:
		return;
	}
	update_dirty(f);
}

void settings_set_text(enum cfg_field f, const char *text)
{
	switch (f) {
	case CFG_MAC:
		snprintf(cfg.edit.mac, sizeof(cfg.edit.mac), "%s", text);
		break;
	case CFG_HDF:
		snprintf(cfg.edit.hdf, sizeof(cfg.edit.hdf), "%s", text);
		break;
	default:
		return;
	}
	update_dirty(f);
}

void settings_clear(enum cfg_field f)
{
	struct zzcfg_values *v = &cfg.edit;

	switch (f) {
	case CFG_PROFILE:     v->videocap_profile_present = 0; break;
	case CFG_SAMPLE:      v->videocap_sample_present = 0; break;
	case CFG_SCAN_MODE:   v->scanline_mode_present = 0; break;
	case CFG_SCAN_PARITY: v->scanline_parity_present = 0; break;
	case CFG_GAMMA:       v->gamma_present = 0; break;
	case CFG_CROP_H:      v->videocap_crop_h_present = 0; break;
	case CFG_CROP_V:      v->videocap_crop_v_present = 0; break;
	case CFG_PHASE_E7M:   v->videocap_phase_present = 0; break;
	case CFG_PHASE_C28:   v->videocap_c28_phase_present = 0; break;
	case CFG_WIDTH:       v->videocap_width_present = 0; break;
	case CFG_HEIGHT:      v->videocap_height_present = 0; break;
	case CFG_MAC:         v->mac[0] = '\0'; break;
	case CFG_HDF:         v->hdf[0] = '\0'; break;
	case CFG_FAST_RAM:
		v->fast_ram_present = 0;
		v->fast_ram_invalid = 0;
		break;
	default:
		return;
	}
	update_dirty(f);
}

void settings_reset(enum cfg_field f)
{
	field_copy(&cfg.edit, &cfg.saved, f);
	cfg.dirty &= ~CFG_BIT(f);
}

static UWORD count_bits(ULONG bits)
{
	UWORD n = 0;

	while (bits) {
		n += (UWORD)(bits & 1);
		bits >>= 1;
	}
	return n;
}

UWORD settings_dirty_count(void)
{
	return count_bits(cfg.dirty);
}

UWORD settings_reboot_count(void)
{
	return count_bits(cfg.dirty & CFG_REBOOT_FIELDS);
}

UWORD settings_power_cycle_count(void)
{
	return count_bits(cfg.dirty & CFG_POWER_CYCLE_FIELDS);
}

const char *settings_system_note(void)
{
	if (!cfg.supported)
		return "These settings need firmware 2.3+.";
	return "Other settings: power-cycle. Fast RAM: reboot.";
}

static const char *fast_ram_outcome_text(UWORD outcome)
{
	switch (outcome) {
	case ZZ_CFG_FAST_RAM_OUTCOME_OFF:       return "off";
	case ZZ_CFG_FAST_RAM_OUTCOME_ABSENT:    return "not configured";
	case ZZ_CFG_FAST_RAM_OUTCOME_INVALID:   return "invalid config";
	case ZZ_CFG_FAST_RAM_OUTCOME_TRUNCATED: return "truncated config";
	case ZZ_CFG_FAST_RAM_OUTCOME_MEDIA_ERR: return "SD error";
	case ZZ_CFG_FAST_RAM_OUTCOME_TIMEOUT:   return "boot timeout";
	default:                                return "boot gate";
	}
}

static int fast_ram_saved_on(void)
{
	return cfg.saved.fast_ram_present && !cfg.saved.fast_ram_invalid &&
		cfg.saved.fast_ram;
}

/* A read failure or a poisoned line. OFF and ABSENT are clean decisions. */
static int fast_ram_outcome_failed(UWORD outcome)
{
	return outcome == ZZ_CFG_FAST_RAM_OUTCOME_INVALID ||
		outcome == ZZ_CFG_FAST_RAM_OUTCOME_TRUNCATED ||
		outcome == ZZ_CFG_FAST_RAM_OUTCOME_MEDIA_ERR ||
		outcome == ZZ_CFG_FAST_RAM_OUTCOME_TIMEOUT;
}

void settings_fast_ram_status(char *status, UWORD status_size,
	char *note, UWORD note_size)
{
	if (note && note_size)
		note[0] = '\0';
	if (!status || status_size == 0)
		return;
	status[0] = '\0';

	if (!cfg.supported) {
		snprintf(status, status_size, "%s",
			"Unavailable (no config support)");
		return;
	}
	if (cfg.fast_ram_state == ZZCFG_FAST_RAM_UNAVAILABLE) {
		snprintf(status, status_size, "%s",
			"Unavailable on this firmware");
		if (note && note_size)
			snprintf(note, note_size, "%s",
				"Fast RAM needs FW boot gate support.");
		return;
	}
	/* The outcome register is the last boot. Fail-closed results stay
	 * visible even if the file has since been rewritten. */
	if (cfg.fast_ram_state == ZZCFG_FAST_RAM_WITHHELD ||
			fast_ram_outcome_failed(cfg.fast_ram_outcome)) {
		if (cfg.fast_ram_state == ZZCFG_FAST_RAM_WITHHELD) {
			snprintf(status, status_size, "Withheld (%s)",
				fast_ram_outcome_text(cfg.fast_ram_outcome));
			if (note && note_size)
				snprintf(note, note_size, "%s",
					"Boot gate withheld memory for safety.");
		} else {
			snprintf(status, status_size, "Disabled (%s)",
				fast_ram_outcome_text(cfg.fast_ram_outcome));
		}
		return;
	}
	if (note && note_size)
		snprintf(note, note_size, "%s", "Takes effect after reboot.");
	if (cfg.fast_ram_state == ZZCFG_FAST_RAM_ENABLED) {
		if (fast_ram_saved_on())
			snprintf(status, status_size, "On%s",
				cfg.fast_ram_outcome == ZZ_CFG_FAST_RAM_OUTCOME_BAK_ON ?
				" (backup)" : "");
		else
			snprintf(status, status_size, "%s",
				"On now; off after reboot");
		return;
	}
	if (cfg.fast_ram_outcome != ZZ_CFG_FAST_RAM_OUTCOME_OFF &&
			cfg.fast_ram_outcome != ZZ_CFG_FAST_RAM_OUTCOME_ABSENT) {
		snprintf(status, status_size, "Disabled (%s)",
			fast_ram_outcome_text(cfg.fast_ram_outcome));
		return;
	}
	if (fast_ram_saved_on()) {
		snprintf(status, status_size, "%s", "Off now; on after reboot");
		return;
	}
	if (cfg.fast_ram_outcome == ZZ_CFG_FAST_RAM_OUTCOME_OFF) {
		snprintf(status, status_size, "%s", "Off");
		return;
	}
	snprintf(status, status_size, "Disabled (%s)",
		fast_ram_outcome_text(cfg.fast_ram_outcome));
}

void settings_saved_message(char *msg, UWORD size)
{
	UWORD reboot = settings_reboot_count();
	UWORD power = settings_power_cycle_count();

	if (!msg || size == 0)
		return;
	if (power)
		snprintf(msg, size,
			"Saved. Power-cycle the Amiga to apply %u "
			"boot-time setting%s.",
			(unsigned)reboot, reboot == 1 ? "" : "s");
	else if (reboot)
		snprintf(msg, size, "%s",
			"Saved. Reboot the Amiga to apply Fast RAM.");
	else
		snprintf(msg, size, "%s", "Saved.");
}

/* ------------------------------------------------------------------ */
/* Validation and save                                                 */

BOOL settings_mac_valid(const char *s)
{
	int i, j;

	for (i = 0; i < 6; i++) {
		for (j = 0; j < 2; j++) {
			char c = s[i * 3 + j];
			if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
					(c >= 'A' && c <= 'F')))
				return FALSE;
		}
		if (i < 5 && s[i * 3 + 2] != ':' && s[i * 3 + 2] != '-')
			return FALSE;
	}
	return s[ZZCFG_MAC_CHARS] == '\0';
}

BOOL settings_validate(enum cfg_field *bad, char *msg, UWORD size)
{
	if ((cfg.dirty & CFG_BIT(CFG_MAC)) && cfg.edit.mac[0] &&
			!settings_mac_valid(cfg.edit.mac)) {
		*bad = CFG_MAC;
		snprintf(msg, size, "MAC address must look like 00:11:22:33:44:55");
		return FALSE;
	}
	/* Firmware hdf rules (not the FWUP name rules): a name the firmware
	 * rejects is silently ignored at the next cold boot. */
	if ((cfg.dirty & CFG_BIT(CFG_HDF)) && cfg.edit.hdf[0] &&
			!zzcfg_hdf_name_valid(cfg.edit.hdf)) {
		*bad = CFG_HDF;
		snprintf(msg, size, "SD image name must be a plain file name "
			"in the card's root");
		return FALSE;
	}
	return TRUE;
}

static struct zzcfg_values save_values;

UWORD settings_save(void)
{
	UWORD st, f;

	if (!cfg.supported) return FWUP_ERR_UNKNOWN;
	st = read_card(&save_values, NULL, TRUE);
	if (st != ZZ_CFG_FILE_OK && st != ZZ_CFG_FILE_NO_FILE)
		return FWUP_ERR_OPEN;
	for (f = 0; f < CFG_FIELD_COUNT; f++)
		if (cfg.dirty & CFG_BIT(f))
			field_copy(&save_values, &cfg.edit, (enum cfg_field)f);
	st = zzcfg_save((ULONG)board.regs, &save_values);
	if (st == FWUP_OK) settings_load();
	return st;
}

/* ------------------------------------------------------------------ */
/* ENV overrides                                                       */

static BOOL env_exists(const char *path)
{
	BPTR f = Open((CONST_STRPTR)path, MODE_OLDFILE);

	if (!f) return FALSE;
	Close(f);
	return TRUE;
}

static BOOL env_read_mac(char *out, UWORD size)
{
	BPTR f = Open((CONST_STRPTR)"ENV:ZZ9K_MAC", MODE_OLDFILE);
	LONG n;

	if (!f) return FALSE;
	n = Read(f, out, size - 1);
	Close(f);
	if (n < ZZCFG_MAC_CHARS) return FALSE;
	out[ZZCFG_MAC_CHARS] = '\0';
	return settings_mac_valid(out);
}

void settings_env_scan(void)
{
	UWORD pal_mode, full, vsync;
	BOOL native = FALSE;

	cfg.env = 0;
	if (env_exists("ENV:ZZ9K_INT2")) cfg.env |= CFG_ENV_INT2;
	if (env_read_mac(cfg.env_mac, sizeof(cfg.env_mac)))
		cfg.env |= CFG_ENV_MAC;

	/* Legacy native-video switches select a capture path through the
	 * lossy legacy tuple. */
	zzcfg_profile_to_legacy(cfg.saved.videocap_profile, &pal_mode, &full,
		&vsync);
	if (env_exists("ENV:ZZ9000-VCAP-800x600")) {
		pal_mode = 0;
		full = 0;
		native = TRUE;
	}
	if (env_exists("ENV:ZZ9000-NS-VSYNC")) {
		vsync = 1;
		native = TRUE;
	} else if (env_exists("ENV:ZZ9000-NS-VSYNC-NTSC")) {
		vsync = 2;
		native = TRUE;
	}
	if (native) {
		cfg.env |= CFG_ENV_NATIVE;
		cfg.env_profile = zzcfg_profile_from_legacy(pal_mode, full, vsync);
	}
}

/* Native-video overrides are adopted through video_set_profile(), which
 * also clears crop measured on the old capture grid. */
void settings_env_adopt(UWORD which)
{
	settings_env_scan();
	if ((which & CFG_ENV_INT2) && (cfg.env & CFG_ENV_INT2))
		settings_set(CFG_INT2, 1);
	if ((which & CFG_ENV_MAC) && (cfg.env & CFG_ENV_MAC))
		settings_set_text(CFG_MAC, cfg.env_mac);
}

static const char *const env_names_int2[] = { "ZZ9K_INT2", NULL };
static const char *const env_names_mac[] = { "ZZ9K_MAC", NULL };
static const char *const env_names_native[] = {
	"ZZ9000-VCAP-800x600", "ZZ9000-NS-VSYNC", "ZZ9000-NS-VSYNC-NTSC", NULL
};

static BOOL env_delete(const char *const *names)
{
	char path[40];
	BOOL ok = TRUE;
	int i;

	for (i = 0; names[i]; i++) {
		snprintf(path, sizeof(path), "ENVARC:%s", names[i]);
		if (env_exists(path) && !DeleteFile((STRPTR)path)) ok = FALSE;
		snprintf(path, sizeof(path), "ENV:%s", names[i]);
		if (env_exists(path) && !DeleteFile((STRPTR)path)) ok = FALSE;
	}
	return ok;
}

BOOL settings_env_remove(UWORD which)
{
	BOOL ok = TRUE;

	if (which & CFG_ENV_INT2) ok &= env_delete(env_names_int2);
	if (which & CFG_ENV_MAC) ok &= env_delete(env_names_mac);
	if (which & CFG_ENV_NATIVE) ok &= env_delete(env_names_native);
	settings_env_scan();
	return ok;
}

/* ------------------------------------------------------------------ */
/* Firmware audio Save guard                                           */

static char guard_text_a[ZZCFG_MAX_SIZE];
static char guard_text_b[ZZCFG_MAX_SIZE];
static struct zzcfg_values guard_after;

BOOL settings_guard_begin(struct settings_guard *guard)
{
	UWORD st;

	guard->valid = FALSE;
	if (!cfg.supported) return TRUE;   /* no card file to protect */
	/* The boot-outcome Fast RAM rule applies here as in every other
	 * save: a repair must never write a withheld preference back clean. */
	st = read_card(&guard->before, NULL, TRUE);
	if (st == ZZ_CFG_FILE_NO_FILE) return TRUE;
	if (st != ZZ_CFG_FILE_OK) return FALSE;
	guard->valid = TRUE;
	return TRUE;
}

UWORD settings_guard_end(struct settings_guard *guard)
{
	const size_t audio = offsetof(struct zzcfg_values, audio_active);
	UWORD len_a, len_b, st;

	if (!guard->valid) return FWUP_OK;
	/* A failed read keeps the snapshot: the caller retries later. */
	if (read_card(&guard_after, NULL, TRUE) != ZZ_CFG_FILE_OK)
		return FWUP_ERR_OPEN;
	/* Keep everything before the audio block from the snapshot, take
	 * the audio block the firmware just wrote. */
	memcpy((UBYTE *)&guard->before + audio, (UBYTE *)&guard_after + audio,
		sizeof(struct zzcfg_values) - audio);
	len_a = zzcfg_generate(&guard->before, guard_text_a,
		sizeof(guard_text_a));
	len_b = zzcfg_generate(&guard_after, guard_text_b,
		sizeof(guard_text_b));
	st = FWUP_OK;
	if (len_a != len_b || memcmp(guard_text_a, guard_text_b, len_a) != 0)
		st = zzcfg_save((ULONG)board.regs, &guard->before);
	if (st == FWUP_OK) guard->valid = FALSE;
	return st;
}
