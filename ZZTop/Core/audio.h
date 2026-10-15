/*
 * ZZTop client of the firmware audio control plane (zz9k.library).
 * Copyright (C) 2016-2026, Lucie L. Hartmann <lucie@mntre.com>
 *                          MNT Research GmbH, Berlin
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ZZTOP_AUDIO_H
#define ZZTOP_AUDIO_H

#include <exec/types.h>

#define AUDIO_SCENE_COUNT 8
#define AUDIO_NAME_CHARS 16
#define AUDIO_EQ_BANDS 10

struct audio_scene {
	UWORD lpf;
	UWORD eq[AUDIO_EQ_BANDS];
	UWORD prefactor;
	UWORD volume;
	UWORD pan;
	char name[AUDIO_NAME_CHARS + 1];
};

struct audio_meter {
	ULONG peak_left;
	ULONG peak_right;
	ULONG clip;
	ULONG underrun;
	ULONG overrun;
	ULONG limiter;
	ULONG identity;
	BOOL valid;
};

void audio_init(void);
void audio_shutdown(void);
BOOL audio_available(void);
const char *audio_unavailable_reason(void);
BOOL audio_pending(void);
BOOL audio_save_start(char *msg, UWORD size);
enum audio_save_state { AUDIO_SAVE_RUNNING, AUDIO_SAVE_DONE, AUDIO_SAVE_FAILED };
enum audio_save_state audio_save_poll(char *msg, UWORD size);
BOOL audio_revert(char *msg, UWORD size);
void audio_tick(void);
void audio_log_enable(BOOL on);
BOOL audio_log_enabled(void);

/* Page-facing editor model. Every successful mutation is live immediately. */
BOOL audio_retry(void);
BOOL audio_retry_available(void);
UWORD audio_active_scene(void);
const struct audio_scene *audio_scene(UWORD index);
UWORD audio_baseline_paula(void);
UWORD audio_baseline_ax(void);
UWORD audio_baseline_max_paula(void);
UWORD audio_baseline_max_ax(void);
UWORD audio_ceiling_paula(void);
UWORD audio_ceiling_ax(void);
ULONG audio_boundary(void);
ULONG audio_weighted_level(void);
BOOL audio_calibration_pending(void);
BOOL audio_metering_available(void);
BOOL audio_get_meter(BOOL input, struct audio_meter *out);
BOOL audio_select_scene(UWORD scene);
BOOL audio_set_baseline(UWORD paula, UWORD ax);
BOOL audio_balance(void);
BOOL audio_set_calibration(UWORD paula, UWORD ax);
BOOL audio_set_scene_value(UWORD scene, UWORD param, UWORD value);
BOOL audio_rename_scene(UWORD scene, const char *name);
const char *audio_last_error(void);

#endif /* ZZTOP_AUDIO_H */
