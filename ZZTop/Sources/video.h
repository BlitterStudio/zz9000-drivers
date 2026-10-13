/*
 * ZZTop live native-video (scandoubler capture) control.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ZZTOP_VIDEO_H
#define ZZTOP_VIDEO_H

#include <exec/types.h>
#include "zz_vcap_client.h"
#include "zz_vcap_geometry.h"

/* ---- application contract (app.c) ---- */
void video_init(void);
BOOL video_live_pending(void);
BOOL video_save_check(char *msg, UWORD size);
void video_saved(void);
BOOL video_revert(char *msg, UWORD size);

/* ---- page-facing live capture API ---- */
BOOL video_live_supported(void);
const char *video_unavailable_reason(void);
enum zz_vcap_phase_domain video_phase_domain(void);
BOOL video_phase_state(int *phase, char *msg, UWORD size);
BOOL video_geometry_state(struct zz_vcap_geometry_state *state, char *msg,
	UWORD size);
BOOL video_snapshot(struct zz_vcap_snapshot *snapshot);
BOOL video_current_path_matches(void);
BOOL video_geometry_supported(void);
BOOL video_align_ready(char *reason, UWORD size);
BOOL video_find_phase_ready(char *reason, UWORD size);

/* Each successful operation applies live, stages the corresponding CFG key,
 * and leaves it for global Save. Values of zero for width/height mean
 * Automatic. Crop presence selects Custom versus Automatic independently. */
BOOL video_set_profile(UWORD profile, char *msg, UWORD size);
BOOL video_set_crop(UWORD h, BOOL h_present, UWORD v, BOOL v_present,
	char *msg, UWORD size);
BOOL video_set_phase(int phase, BOOL present, char *msg, UWORD size);
BOOL video_set_geometry(UWORD width, UWORD height, char *msg, UWORD size);
BOOL video_set_sample(UWORD sample, char *msg, UWORD size);
BOOL video_reset_automatic(char *msg, UWORD size);

/* The native-screen tools. On successful alignment/sweep their accepted live
 * result is immediately staged like an ordinary page edit. */
BOOL video_align(char *msg, UWORD size);
BOOL video_find_phase(char *msg, UWORD size);
void video_sweep_text(char *text, UWORD size);

#endif /* ZZTOP_VIDEO_H */
