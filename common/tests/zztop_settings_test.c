/*
 * Host tests for ZZTop's ZZ9000.CFG editor model (ZZTop/Sources/settings.c):
 * per-field dirty tracking, merge-on-save over a fresh card read, and the
 * guard that undoes the firmware audio Save's rewrite of non-audio keys.
 *
 * The card file is simulated behind the real zzcfg_amiga.c register
 * protocol (ZZCFG_TEST_IO) and the in-memory FWUP transport.
 *
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <string.h>

#include "zztop_settings_test_platform.h"
#include "board.h"
#include "fwup_client.h"
#include "settings.h"

extern char fwup_test_saved_text[];
extern uint16_t fwup_test_saved_len;

static int failures;

static void check(int cond, const char *what)
{
	if (!cond) {
		printf("FAIL: %s\n", what);
		failures++;
	}
}

/* ---- simulated card file ---- */

static char card[ZZCFG_MAX_SIZE + 1];
static UWORD card_status;
static int reset_pending, read_pending, saves_seen;

static void card_set(const char *text)
{
	snprintf(card, sizeof(card), "%s", text);
	card_status = ZZ_CFG_FILE_OK;
}

/* A completed FWUP CLOSE is the card file changing. */
static void card_sync_saves(void)
{
	static uint16_t last_len;
	static char last[sizeof(card)];

	if (fwup_test_saved_len != last_len ||
			memcmp(last, fwup_test_saved_text, fwup_test_saved_len)) {
		last_len = fwup_test_saved_len;
		memcpy(last, fwup_test_saved_text, last_len);
		memcpy(card, fwup_test_saved_text, last_len);
		card[last_len] = '\0';
		saves_seen++;
	}
}

UWORD zzcfg_test_reg_read(ULONG b, ULONG offset)
{
	(void)b;
	if (offset == ZZ_REG_CONFIG_FILE) {
		card_sync_saves();
		if (reset_pending) {
			reset_pending = 0;
			return ZZ_CFG_FILE_IDLE;
		}
		if (read_pending) {
			read_pending = 0;
			return card_status;
		}
		return card_status;
	}
	if (offset == ZZ_REG_CONFIG_FILE_LEN)
		return (UWORD)strlen(card);
	return 0;
}

void zzcfg_test_reg_write(ULONG b, ULONG offset, UWORD value)
{
	(void)b;
	if (offset != ZZ_REG_CONFIG_FILE) return;
	if (value == 0) reset_pending = 1;
	else if (value == 1) read_pending = 1;
}

UBYTE zzcfg_test_buffer_read(ULONG b, UWORD offset)
{
	(void)b;
	return (UBYTE)card[offset];
}

/* ---- board doubles ---- */

struct board board;
static UWORD live_scan_mode = 2, live_scan_parity = 1;

BOOL board_has_cfg(void) { return TRUE; }
BOOL board_has_cap(UWORD bit) { return (board.fw_caps & bit) != 0; }
UWORD board_scanline_mode(void) { return live_scan_mode; }
UWORD board_scanline_parity(void) { return live_scan_parity; }

/* ---- dos doubles: no ENV: variables ---- */

BPTR Open(CONST_STRPTR name, LONG mode) { (void)name; (void)mode; return 0; }
LONG Close(BPTR f) { (void)f; return 1; }
LONG Read(BPTR f, void *b, LONG n) { (void)f; (void)b; (void)n; return -1; }
BOOL DeleteFile(CONST_STRPTR name) { (void)name; return FALSE; }

/* ---- helpers ---- */

static int has_line(const char *text, const char *line)
{
	size_t n = strlen(line);
	const char *p = text;

	while ((p = strstr(p, line))) {
		if ((p == text || p[-1] == '\n') && (p[n] == '\n' || !p[n]))
			return 1;
		p += n;
	}
	return 0;
}

static void reset(const char *text)
{
	memset(&board, 0, sizeof(board));
	board.fw_version = 0x0208;
	board.fw_caps = ZZ_FW_CAP_VIDEOCAP_PROFILE;
	card_set(text);
	card_sync_saves();
	saves_seen = 0;
	settings_load();
}

/* ---- tests ---- */

static void test_dirty_tracks_value_not_touch(void)
{
	reset("int2 = off\n");
	check(settings_dirty_count() == 0, "fresh load is clean");
	settings_set(CFG_INT2, 1);
	check(settings_dirty_count() == 1, "changed field is dirty");
	check(settings_reboot_count() == 1, "INT2 needs a reboot");
	settings_set(CFG_INT2, 0);
	check(settings_dirty_count() == 0,
		"setting the saved value again clears the edit");

	/* Presence is part of the value: an explicit 0 crop is not the
	 * Automatic (absent) crop. */
	settings_set(CFG_CROP_H, 0);
	check(settings_dirty_count() == 1, "explicit crop over absent is dirty");
	settings_clear(CFG_CROP_H);
	check(settings_dirty_count() == 0, "back to Automatic is clean");
}

static void test_save_merges_over_fresh_card(void)
{
	reset("int2 = off\n");
	settings_set(CFG_INT2, 1);

	/* Another writer (the firmware audio Save, a manual edit) changes
	 * the card after ZZTop loaded it. */
	card_set("int2 = off\nhdf = game.hdf\n");

	check(settings_save() == FWUP_OK, "save succeeds");
	check(has_line(card, "int2 = on"), "edited key written");
	check(has_line(card, "hdf = game.hdf"),
		"key changed by another writer survives the save");
	check(settings_dirty_count() == 0, "model reloaded after save");
	check(cfg.saved.int2 == 1, "reloaded model shows the saved value");
}

static void test_untouched_absent_native_key_stays_absent(void)
{
	reset("int2 = off\n");
	settings_set(CFG_INT2, 1);
	check(settings_save() == FWUP_OK, "save succeeds");
	check(strstr(card, "\nvideocap_profile") == NULL &&
		strncmp(card, "videocap_profile", 16) != 0,
		"absent output profile is not activated by an unrelated save");
	check(!has_line(card, "scanline_mode = 2"),
		"live scanline default is not persisted unless edited");
}

static void test_guard_restores_keys_reverted_by_audio_save(void)
{
	int before;

	/* ZZTop saved INT2 earlier in this boot... */
	reset("int2 = on\nhdf = game.hdf\n");
	{
		struct settings_guard guard;

		check(settings_guard_begin(&guard), "snapshot taken");
		/* ...then the firmware audio Save rewrote the file from its
		 * cold-boot parse (INT2 off, no hdf) plus new audio keys. */
		card_set("int2 = off\naudio_baseline = 9248\n");
		before = saves_seen;
		check(settings_guard_end(&guard) == FWUP_OK, "guard repair saved");
		card_sync_saves();
		check(saves_seen == before + 1, "guard wrote the card once");
	}
	check(has_line(card, "int2 = on"), "reverted key restored");
	check(has_line(card, "hdf = game.hdf"), "dropped key restored");
	check(strstr(card, "audio_baseline = 9248") != NULL,
		"new audio keys from the firmware kept");
}

static void test_guard_leaves_untouched_file_alone(void)
{
	int before;
	struct settings_guard guard;

	reset("int2 = on\n");
	check(settings_guard_begin(&guard), "snapshot taken");
	/* Audio save only changed audio keys. Render through the generator
	 * so formatting matches what the firmware would emit. */
	{
		struct zzcfg_values v = cfg.saved;
		static char text[ZZCFG_MAX_SIZE];

		v.audio_baseline = 9248;
		v.audio_baseline_present = 1;
		zzcfg_generate(&v, text, sizeof(text));
		card_set(text);
	}
	before = saves_seen;
	check(settings_guard_end(&guard) == FWUP_OK, "guard succeeds");
	card_sync_saves();
	check(saves_seen == before, "no rewrite when non-audio keys match");
}

static void test_guard_refuses_unreadable_card(void)
{
	struct settings_guard guard;

	reset("int2 = on\n");
	card_status = ZZ_CFG_FILE_IO_ERROR;
	check(!settings_guard_begin(&guard),
		"no unprotected audio save when the card cannot be read");
	card_status = ZZ_CFG_FILE_OK;
}

static void test_guard_survives_failed_post_save_read(void)
{
	struct settings_guard guard;

	reset("int2 = on\n");
	check(settings_guard_begin(&guard), "snapshot taken");
	card_set("int2 = off\naudio_baseline = 9248\n");
	card_status = ZZ_CFG_FILE_IO_ERROR;
	check(settings_guard_end(&guard) != FWUP_OK,
		"unreadable card after the audio save is an error");
	check(guard.valid, "snapshot kept for a retry");
	card_status = ZZ_CFG_FILE_OK;
	check(settings_guard_end(&guard) == FWUP_OK, "retry repairs");
	card_sync_saves();
	check(has_line(card, "int2 = on"), "retry restored the reverted key");
	check(!guard.valid, "guard cleared once repaired");
}

int main(void)
{
	test_dirty_tracks_value_not_touch();
	test_save_merges_over_fresh_card();
	test_untouched_absent_native_key_stays_absent();
	test_guard_restores_keys_reverted_by_audio_save();
	test_guard_leaves_untouched_file_alone();
	test_guard_refuses_unreadable_card();
	test_guard_survives_failed_post_save_read();
	if (failures) {
		printf("zztop_settings_test: %d failure(s)\n", failures);
		return 1;
	}
	printf("zztop_settings_test: PASS\n");
	return 0;
}
