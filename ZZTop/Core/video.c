/*
 * ZZTop live native-video (scandoubler capture) control.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <exec/types.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdio.h>
#include <string.h>

#include "align.h"
#include "board.h"
#include "settings.h"
#include "frontend.h"
#include "vcap_sampling.h"
#include "video.h"

#define VCAP_APPLY_TICKS 100

struct video_live_state {
	BOOL supported;
	BOOL crop_touched;
	BOOL crop_confirmed;
	ULONG crop_target;
	BOOL crop_target_valid;
	BOOL phase_touched;
	BOOL phase_confirmed;
	int phase_target;
	BOOL geometry_touched;
	BOOL geometry_confirmed;
	UWORD geometry_width;
	UWORD geometry_height;
	enum zz_vcap_phase_domain domain;
	struct zz_vcap_snapshot current;
	struct zz_vcap_snapshot crop_anchor;
	BOOL crop_anchor_valid;
	int phase_anchor;
	BOOL phase_anchor_valid;
	struct zz_vcap_geometry_state geometry_anchor;
	BOOL geometry_anchor_valid;
	unsigned char sweep[ZZ_CAPTURE_BINS];
	BOOL sweep_valid;
};

static struct video_live_state live;

/* Read one 16-bit register through the discovered board window. */
static uint16_t io_read16(void *ctx, uint32_t offset)
{
	(void)ctx;
	return board_read16(offset);
}

/* Write one 16-bit register through the discovered board window. */
static void io_write16(void *ctx, uint32_t offset, uint16_t value)
{
	(void)ctx;
	board_write16(offset, value);
}

/* Delay protocol polling without a hardware-specific dependency. */
static void io_delay(void *ctx, unsigned ticks)
{
	(void)ctx;
	Delay(ticks);
}

static const struct zz_vcap_io card_io = {
	NULL, io_read16, io_write16, io_delay, NULL, NULL
};

/* Copy a bounded plain-language message for the caller. */
static void write_message(char *msg, UWORD size, const char *text)
{
	if (!msg || !size) return;
	snprintf(msg, size, "%s", text);
	msg[size - 1] = '\0';
}

/* Prefix the shared protocol result text with caller context. */
static void write_result(char *msg, UWORD size, const char *prefix,
	enum zz_vcap_result result)
{
	if (!msg || !size) return;
	snprintf(msg, size, "%s%s", prefix, zz_vcap_result_text(result));
	msg[size - 1] = '\0';
}

/* Read a coherent live capture snapshot around the status sequence. */
static BOOL capture_snapshot(struct zz_vcap_snapshot *out)
{
	int tries;

	for (tries = 0; tries < 8; tries++) {
		ULONG before = zz_vcap_read32(&card_io, ZZ_VCAP_LIVE_STATUS);
		ULONG raw = zz_vcap_read32(&card_io, ZZ_VCAP_LIVE_APPLIED_RAW);
		ULONG effective = zz_vcap_read32(&card_io, ZZ_VCAP_LIVE_EFFECTIVE_CROP);
		ULONG after = zz_vcap_read32(&card_io, ZZ_VCAP_LIVE_STATUS);

		if (zz_vcap_snapshot_status_valid(before, after)) {
			out->status = after;
			out->raw = raw;
			zz_vcap_effective_unpack(effective, &out->effective_h,
				&out->effective_v);
			return TRUE;
		}
	}
	return FALSE;
}

/* Refresh live state and lazily establish Revert anchors. */
static BOOL refresh_live(void)
{
	struct zz_vcap_phase_state phase;

	live.domain = zz_vcap_phase_domain(zz_vcap_read32(&card_io,
		ZZ_CAPTURE_CAP_REG));
	if (!live.phase_anchor_valid && zz_vcap_phase_read(&card_io, &phase) ==
		ZZ_VCAP_OK && zz_vcap_phase_check(&card_io, phase.applied) == ZZ_VCAP_OK) {
		live.domain = phase.domain;
		live.phase_anchor = phase.applied;
		live.phase_anchor_valid = TRUE;
	}
	if (!live.supported || !capture_snapshot(&live.current))
		return FALSE;
	if (!live.crop_anchor_valid) {
		live.crop_anchor = live.current;
		live.crop_anchor_valid = TRUE;
	}
	if (!live.geometry_anchor_valid && zz_vcap_geometry_read(&card_io,
		&live.geometry_anchor) == ZZ_VCAP_OK &&
		(live.geometry_anchor.status & ZZ_VCAP_GEOMETRY_STATUS_APPLIED_VALID) &&
		!(live.geometry_anchor.status & ZZ_VCAP_GEOMETRY_STATUS_PENDING) &&
		live.geometry_anchor.request_serial == live.geometry_anchor.applied_serial)
		live.geometry_anchor_valid = TRUE;
	return TRUE;
}

/* Map persisted output and sample settings to a capture path. */
static void path_for(struct zz_vcap_path *path, UWORD profile, UWORD sample)
{
	UWORD pal, full, vsync;

	zzcfg_profile_to_legacy(profile, &pal, &full, &vsync);
	path->sample = sample;
	path->full_width = full;
}

/* Check whether staged output and sample settings match live capture. */
BOOL video_current_path_matches(void)
{
	struct zz_vcap_control control;
	struct zz_vcap_path applied, staged;

	if (!refresh_live() || !zz_vcap_control_unpack(live.current.raw, &control))
		return FALSE;
	path_for(&staged, cfg.edit.videocap_profile, cfg.edit.videocap_sample);
	applied.sample = control.sample;
	applied.full_width = control.full_width;
	return zz_vcap_path_equal(&staged, &applied);
}

/* Keep the main window responsive during a bounded protocol wait. */
static int wait_keep_running(void *ctx)
{
	return fe_wait_keep_running(ctx);
}

/* Attach UI wait servicing to a copy of the card I/O contract. */
static struct zz_vcap_io wait_io(struct fe_wait *wait)
{
	struct zz_vcap_io io = card_io;

	io.control_ctx = wait;
	io.keep_running = wait_keep_running;
	return io;
}

/* The crop ABI predates the common client because it needs snapshot readback
 * in addition to the phase/geometry protocol clients. */
/* Commit one crop-control word and wait for its acknowledged snapshot. */
static BOOL apply_raw(ULONG raw, struct zz_vcap_snapshot *applied,
	struct fe_wait *wait, BOOL restoring)
{
	struct zz_vcap_status status;
	ULONG stable;
	UBYTE sequence;
	UWORD tick;

	if (wait && !restoring && !fe_wait_keep_running(wait)) return FALSE;
	if (zz_vcap_read_stable(&card_io, ZZ_VCAP_LIVE_STATUS, &stable) !=
			ZZ_VCAP_OK) return FALSE;
	zz_vcap_status_unpack(stable, &status);
	if (!status.applied_valid || status.busy ||
		status.request_sequence != status.applied_sequence) return FALSE;
	sequence = zz_vcap_next_sequence(status.request_sequence);
	board_write16(ZZ_VCAP_LIVE_STAGED_RAW_HI, (UWORD)(raw >> 16));
	board_write16(ZZ_VCAP_LIVE_STAGED_RAW_LO, (UWORD)raw);
	board_write16(ZZ_VCAP_LIVE_COMMIT, ZZ_VCAP_LIVE_COMMIT_TOKEN);
	for (tick = 0; tick < VCAP_APPLY_TICKS; tick++) {
		if (wait && !restoring && !fe_wait_keep_running(wait)) return FALSE;
		if (zz_vcap_read_stable(&card_io, ZZ_VCAP_LIVE_STATUS, &stable) ==
				ZZ_VCAP_OK && zz_vcap_request_complete(stable, sequence)) {
			if (!capture_snapshot(applied)) return FALSE;
			return zz_vcap_request_result(stable, sequence, applied->raw,
				raw) == ZZ_VCAP_REQUEST_APPLIED;
		}
		Delay(1);
	}
	return FALSE;
}

/* Apply a phase through the cancellable phase protocol. */
static BOOL phase_apply(int phase, BOOL restoring, char *msg, UWORD size)
{
	struct fe_wait wait;
	struct zz_vcap_io io;
	BOOL cancel = FALSE;
	enum zz_vcap_result result;

	fe_wait_begin(&wait, &cancel);
	io = wait_io(&wait);
	fe_busy(TRUE);
	result = zz_vcap_phase_apply(&io, phase, restoring);
	fe_busy(FALSE);
	fe_wait_end(&wait);
	if (result != ZZ_VCAP_OK) {
		write_result(msg, size, "Phase not confirmed: ", result);
		return FALSE;
	}
	return TRUE;
}

/* Apply a capture window through the cancellable geometry protocol. */
static BOOL geometry_apply(UWORD width, UWORD height, BOOL restoring,
	char *msg, UWORD size)
{
	struct fe_wait wait;
	struct zz_vcap_io io;
	BOOL cancel = FALSE;
	enum zz_vcap_result result;

	fe_wait_begin(&wait, &cancel);
	io = wait_io(&wait);
	fe_busy(TRUE);
	result = zz_vcap_geometry_apply(&io, width, height, restoring);
	fe_busy(FALSE);
	fe_wait_end(&wait);
	if (result != ZZ_VCAP_OK) {
		write_result(msg, size, "Capture window not confirmed: ", result);
		return FALSE;
	}
	return TRUE;
}

/* Detect live capture support and seed the live state at startup. */
void video_init(void)
{
	memset(&live, 0, sizeof(live));
	live.supported = zz_vcap_live_supported(board.fw_version, board.fw_caps,
		zz_vcap_read32(&card_io, ZZ_VCAP_LIVE_CAPABILITY));
	live.domain = zz_vcap_phase_domain(zz_vcap_read32(&card_io,
		ZZ_CAPTURE_CAP_REG));
	(void)refresh_live();
}

/* Report whether this firmware exposes live capture controls. */
BOOL video_live_supported(void)
{
	return live.supported;
}

/* Explain the firmware feature missing for live capture controls. */
const char *video_unavailable_reason(void)
{
	return live.supported ? "" :
		"Live controls need firmware 2.8 with matching bitstream";
}

/* Return the currently detected phase-engine domain. */
enum zz_vcap_phase_domain video_phase_domain(void)
{
	return live.domain;
}

/* Return a freshly coherent live snapshot when available. */
BOOL video_snapshot(struct zz_vcap_snapshot *snapshot)
{
	if (!live.supported || !refresh_live()) return FALSE;
	if (snapshot) *snapshot = live.current;
	return TRUE;
}

/* Report whether firmware acknowledges capture-window requests. */
BOOL video_geometry_supported(void)
{
	return zz_vcap_geometry_supported(&card_io) != 0;
}

/* Read and validate the phase currently applied by the capture engine. */
BOOL video_phase_state(int *phase, char *msg, UWORD size)
{
	struct zz_vcap_phase_state state;
	enum zz_vcap_result result = zz_vcap_phase_read(&card_io, &state);

	if (result == ZZ_VCAP_OK) result = zz_vcap_phase_check(&card_io,
		state.applied);
	if (result != ZZ_VCAP_OK) {
		write_result(msg, size, "Phase unavailable: ", result);
		return FALSE;
	}
	live.domain = state.domain;
	if (phase) *phase = state.applied;
	return TRUE;
}

/* Read the capture-window state and explain protocol failures. */
BOOL video_geometry_state(struct zz_vcap_geometry_state *state, char *msg,
	UWORD size)
{
	enum zz_vcap_result result = zz_vcap_geometry_read(&card_io, state);

	if (result != ZZ_VCAP_OK) {
		write_result(msg, size, "Capture window unavailable: ", result);
		return FALSE;
	}
	return TRUE;
}

/* Persist each crop axis independently, preserving Automatic state. */
static void stage_crop(UWORD h, BOOL hp, UWORD v, BOOL vp)
{
	if (hp)
		settings_set(CFG_CROP_H, h);
	else
		settings_clear(CFG_CROP_H);
	if (vp)
		settings_set(CFG_CROP_V, v);
	else
		settings_clear(CFG_CROP_V);
}

/* Apply and stage custom crop values or Automatic axes. */
BOOL video_set_crop(UWORD h, BOOL hp, UWORD v, BOOL vp, char *msg, UWORD size)
{
	struct zz_vcap_control control;
	struct zz_vcap_snapshot applied;
	struct fe_wait wait;
	BOOL cancel = FALSE;

	if (h > ZZ_VCAP_CROP_MAX || v > ZZ_VCAP_CROP_MAX) {
		write_message(msg, size, "Crop values must be between 0 and 4095");
		return FALSE;
	}
	if (!live.supported) {
		stage_crop(h, hp, v, vp);
		write_message(msg, size, "Crop saved for the next power-cycle");
		return TRUE;
	}
	if (!video_current_path_matches()) {
		write_message(msg, size,
			"The running capture path differs; Save and reboot or restore it");
		return FALSE;
	}
	if (!zz_vcap_control_unpack(live.current.raw, &control)) {
		write_message(msg, size, "Live crop state is unavailable");
		return FALSE;
	}
	control.crop_h = h;
	control.crop_v = v;
	control.crop_h_present = hp ? 1 : 0;
	control.crop_v_present = vp ? 1 : 0;
	live.crop_touched = TRUE;
	live.crop_confirmed = FALSE;
	live.crop_target = zz_vcap_control_pack(&control);
	live.crop_target_valid = TRUE;
	fe_wait_begin(&wait, &cancel);
	fe_busy(TRUE);
	if (!apply_raw(live.crop_target, &applied, &wait, FALSE)) {
		fe_busy(FALSE);
		fe_wait_end(&wait);
		write_message(msg, size, cancel ? "Crop change cancelled" :
			"Crop was not acknowledged by native video");
		return FALSE;
	}
	fe_busy(FALSE);
	fe_wait_end(&wait);
	live.current = applied;
	live.crop_confirmed = TRUE;
	stage_crop(h, hp, v, vp);
	live.sweep_valid = FALSE;
	write_message(msg, size, "Framing applied; Save keeps it after reboot");
	return TRUE;
}

/* Apply and stage a capture sampling mode. */
BOOL video_set_sample(UWORD sample, char *msg, UWORD size)
{
	struct zz_vcap_control control;
	struct zz_vcap_snapshot applied;
	struct zz_vcap_path staged, running;
	struct fe_wait wait;
	BOOL cancel = FALSE;

	if (sample > 2) {
		write_message(msg, size, "Unknown capture sample mode");
		return FALSE;
	}
	if (live.supported) {
		if (!refresh_live() || !zz_vcap_control_unpack(live.current.raw, &control)) {
			write_message(msg, size, "Live capture state is unavailable");
			return FALSE;
		}
		/* A staged output profile whose capture width the running path
		 * does not use yet takes effect at the next power-cycle: stage
		 * the sample with it instead of building a live target that
		 * video_save_check() would reject, which would make the pair
		 * impossible to save. */
		path_for(&staged, cfg.edit.videocap_profile, control.sample);
		running.sample = control.sample;
		running.full_width = control.full_width;
		if (!zz_vcap_path_equal(&staged, &running)) {
			settings_set(CFG_SAMPLE, sample);
			live.sweep_valid = FALSE;
			write_message(msg, size,
				"Sample mode staged with the new output; applies after reboot");
			return TRUE;
		}
		control.sample = sample;
		live.crop_touched = TRUE;
		live.crop_confirmed = FALSE;
		live.crop_target = zz_vcap_control_pack(&control);
		live.crop_target_valid = TRUE;
		fe_wait_begin(&wait, &cancel);
		fe_busy(TRUE);
		if (!apply_raw(live.crop_target, &applied, &wait, FALSE)) {
			fe_busy(FALSE);
			fe_wait_end(&wait);
			write_message(msg, size, cancel ? "Sample change cancelled" :
				"Sample change was not acknowledged");
			return FALSE;
		}
		fe_busy(FALSE);
		fe_wait_end(&wait);
		live.current = applied;
		live.crop_confirmed = TRUE;
	}
	settings_set(CFG_SAMPLE, sample);
	live.sweep_valid = FALSE;
	write_message(msg, size, "Sample mode applied; Save keeps it after reboot");
	return TRUE;
}

/* Apply and stage a phase in the active engine domain. */
BOOL video_set_phase(int phase, BOOL present, char *msg, UWORD size)
{
	enum cfg_field field;

	if (live.domain == ZZ_VCAP_PHASE_NONE ||
		!zz_vcap_phase_valid(phase, live.domain)) {
		write_message(msg, size, "Phase is unavailable for this capture engine");
		return FALSE;
	}
	field = live.domain == ZZ_VCAP_PHASE_C28 ? CFG_PHASE_C28 : CFG_PHASE_E7M;
	live.phase_touched = TRUE;
	live.phase_confirmed = FALSE;
	live.phase_target = phase;
	if (!phase_apply(phase, FALSE, msg, size))
		return FALSE;
	live.phase_confirmed = TRUE;
	if (present)
		settings_set(field, phase);
	else
		settings_clear(field);
	live.sweep_valid = FALSE;
	snprintf(msg, size, "Phase %d applied; Save keeps it after reboot", phase);
	return TRUE;
}

/* Apply and stage a capture window or Automatic dimensions. */
BOOL video_set_geometry(UWORD width, UWORD height, char *msg, UWORD size)
{
	if ((width && (width < 256 || width > 1280 || (width & 15))) ||
		(height && (height < 100 || height > 1024))) {
		write_message(msg, size,
			"Width is 256..1280 in steps of 16; height is 100..1024");
		return FALSE;
	}
	if (!video_geometry_supported()) {
		write_message(msg, size,
			"Capture window needs firmware that confirms capture window changes.");
		return FALSE;
	}
	live.geometry_touched = TRUE;
	live.geometry_confirmed = FALSE;
	live.geometry_width = width;
	live.geometry_height = height;
	if (!geometry_apply(width, height, FALSE, msg, size))
		return FALSE;
	live.geometry_confirmed = TRUE;
	if (width)
		settings_set(CFG_WIDTH, width);
	else
		settings_clear(CFG_WIDTH);
	if (height)
		settings_set(CFG_HEIGHT, height);
	else
		settings_clear(CFG_HEIGHT);
	write_message(msg, size, "Capture window applied; Save keeps it after reboot");
	return TRUE;
}

/* Stage an output profile and discard invalid old-grid framing. */
BOOL video_set_profile(UWORD profile, char *msg, UWORD size)
{
	struct zz_vcap_path old_path, new_path;
	struct zz_vcap_snapshot applied;

	if (zzcfg_profile_for_output_refresh(zzcfg_profile_output(profile),
		zzcfg_profile_refresh(profile), board.fw_caps) == ZZCFG_VCAP_PROFILE_COUNT) {
		write_message(msg, size, "That output and refresh pair is unsupported");
		return FALSE;
	}
	path_for(&old_path, cfg.edit.videocap_profile, cfg.edit.videocap_sample);
	path_for(&new_path, profile, cfg.edit.videocap_sample);
	if (live.supported && live.crop_touched && !zz_vcap_path_equal(&old_path, &new_path)) {
		/* A calibration preview belongs to the old capture grid. Restore it
		 * before staging a boot-time path switch, or Revert loses its anchor. */
		if (!live.crop_anchor_valid || !apply_raw(live.crop_anchor.raw, &applied,
			NULL, TRUE)) {
			write_message(msg, size, "Profile change waits for live picture restore; retry");
			return FALSE;
		}
		live.current = applied;
		live.crop_touched = FALSE;
	}
	settings_set(CFG_PROFILE, profile);
	if (live.supported && (settings_present(&cfg.edit, CFG_CROP_H) ||
		settings_present(&cfg.edit, CFG_CROP_V)) &&
		!zz_vcap_path_equal(&old_path, &new_path)) {
		settings_clear(CFG_CROP_H);
		settings_clear(CFG_CROP_V);
		write_message(msg, size,
			"Framing reset to Automatic; Save, reboot, then align picture");
	} else write_message(msg, size, "Output changed; Save, then power-cycle");
	return TRUE;
}

/* Reset crop, phase and capture window in live-safe order. */
BOOL video_reset_automatic(char *msg, UWORD size)
{
	struct zz_vcap_control control;
	struct zz_vcap_snapshot applied;
	struct fe_wait wait;
	BOOL cancel = FALSE;
	enum cfg_field field;

	if (!live.supported) {
		settings_clear(CFG_CROP_H);
		settings_clear(CFG_CROP_V);
		settings_clear(CFG_WIDTH);
		settings_clear(CFG_HEIGHT);
		if (live.domain == ZZ_VCAP_PHASE_C28)
			settings_clear(CFG_PHASE_C28);
		else if (live.domain == ZZ_VCAP_PHASE_E7M)
			settings_clear(CFG_PHASE_E7M);
		write_message(msg, size, "Automatic framing saved for the next power-cycle");
		return TRUE;
	}
	if (!video_geometry_supported() || !refresh_live() ||
		!zz_vcap_control_unpack(live.current.raw, &control)) {
		write_message(msg, size, "Reset to automatic needs live capture window support");
		return FALSE;
	}
	control.crop_h = 0;
	control.crop_v = 0;
	control.crop_h_present = 0;
	control.crop_v_present = 0;
	live.crop_touched = TRUE;
	live.crop_confirmed = FALSE;
	live.crop_target = zz_vcap_control_pack(&control);
	live.crop_target_valid = TRUE;
	fe_wait_begin(&wait, &cancel);
	fe_busy(TRUE);
	if (!apply_raw(live.crop_target, &applied, &wait, FALSE)) {
		fe_busy(FALSE);
		fe_wait_end(&wait);
		write_message(msg, size, cancel ? "Reset to automatic cancelled" :
			"Automatic crop was not acknowledged");
		return FALSE;
	}
	live.current = applied;
	live.crop_confirmed = TRUE;
	fe_busy(FALSE);
	fe_wait_end(&wait);
	if (live.domain != ZZ_VCAP_PHASE_NONE) {
		live.phase_touched = TRUE;
		live.phase_confirmed = FALSE;
		live.phase_target = 0;
		if (!phase_apply(0, FALSE, msg, size))
			return FALSE;
		live.phase_confirmed = TRUE;
	}
	live.geometry_touched = TRUE;
	live.geometry_confirmed = FALSE;
	live.geometry_width = 0;
	live.geometry_height = 0;
	if (!geometry_apply(0, 0, FALSE, msg, size))
		return FALSE;
	live.geometry_confirmed = TRUE;
	field = live.domain == ZZ_VCAP_PHASE_C28 ? CFG_PHASE_C28 : CFG_PHASE_E7M;
	settings_clear(CFG_CROP_H);
	settings_clear(CFG_CROP_V);
	settings_clear(CFG_WIDTH);
	settings_clear(CFG_HEIGHT);
	if (live.domain != ZZ_VCAP_PHASE_NONE)
		settings_clear(field);
	live.sweep_valid = FALSE;
	write_message(msg, size, "Picture reset to Automatic; Save keeps it after reboot");
	return TRUE;
}

/* Check whether native-screen framing alignment can safely run. */
BOOL video_align_ready(char *reason, UWORD size)
{
	if (!live.supported) {
		write_message(reason, size, video_unavailable_reason());
		return FALSE;
	}
	if (!refresh_live()) {
		write_message(reason, size, "Align picture waits for stable native video frames");
		return FALSE;
	}
	if (!video_current_path_matches()) {
		write_message(reason, size, "The running capture path differs; Save and reboot or restore it");
		return FALSE;
	}
	write_message(reason, size, "");
	return TRUE;
}

/* Check every capability required by automatic phase search. */
BOOL video_find_phase_ready(char *reason, UWORD size)
{
	int phase;

	if (!video_align_ready(reason, size)) return FALSE;
	if (live.domain != ZZ_VCAP_PHASE_C28) {
		write_message(reason, size, "Find best phase needs the C28 capture engine");
		return FALSE;
	}
	if (zz_vcap_read32(&card_io, ZZ_CAPTURE_METADATA_CAP_REG) !=
		ZZ_CAPTURE_METADATA_CAP) {
		write_message(reason, size, "Find best phase needs frozen-field metadata");
		return FALSE;
	}
	if (!video_phase_state(&phase, reason, size)) return FALSE;
	write_message(reason, size, "");
	return TRUE;
}

/* Run the native-screen framing tool and stage its accepted crop. */
BOOL video_align(char *msg, UWORD size)
{
	struct zz_vcap_control control;
	struct zz_vcap_snapshot entry;

	if (!video_align_ready(msg, size) || !video_snapshot(&entry))
		return FALSE;
	fe_busy(TRUE);
	if (!align_picture_run(&card_io, &entry, &control, msg, size)) {
		fe_busy(FALSE);
		if (strstr(msg, "state is unknown")) {
			live.crop_touched = TRUE;
			live.crop_confirmed = FALSE;
			live.crop_target_valid = FALSE;
		}
		return FALSE;
	}
	fe_busy(FALSE);
	live.crop_target = zz_vcap_control_pack(&control);
	live.crop_target_valid = TRUE;
	live.crop_touched = TRUE;
	live.crop_confirmed = TRUE;
	(void)video_snapshot(&live.current);
	stage_crop(control.crop_h, control.crop_h_present, control.crop_v,
		control.crop_v_present);
	live.sweep_valid = FALSE;
	write_message(msg, size, "Picture alignment applied; Save keeps it after reboot");
	return TRUE;
}

/* Run a C28 phase sweep and apply its chosen phase. */
BOOL video_find_phase(char *msg, UWORD size)
{
	struct zz_vcap_status status;
	struct zz_vcap_phase_state phase;
	int chosen;
	int result;

	if (!video_find_phase_ready(msg, size) ||
		zz_vcap_phase_read(&card_io, &phase) != ZZ_VCAP_OK || !refresh_live())
		return FALSE;
	zz_vcap_status_unpack(live.current.status, &status);
	fe_busy(TRUE);
	result = vcap_sampling_run(&card_io, status.ntsc, 0, phase.applied,
		&chosen, live.sweep, msg, size);
	fe_busy(FALSE);
	if (result != 1) {
		if (result == -2) {
			live.phase_touched = TRUE;
			live.phase_confirmed = FALSE;
		}
		live.sweep_valid = FALSE;
		return FALSE;
	}
	live.sweep_valid = TRUE;
	if (!video_set_phase(chosen, TRUE, msg, size))
		return FALSE;
	live.sweep_valid = TRUE;
	return TRUE;
}

/* Format the most recent phase sweep for its compact readout. */
void video_sweep_text(char *text, UWORD size)
{
	unsigned i, mark;
	int phase;

	if (!text || !size) return;
	if (live.domain != ZZ_VCAP_PHASE_C28) {
		write_message(text, size, "Phase sweep needs the C28 capture engine");
		return;
	}
	if (!live.sweep_valid || !video_phase_state(&phase, NULL, 0)) {
		write_message(text, size, "Run Find best phase...");
		return;
	}
	if (size < ZZ_CAPTURE_BINS / 4 + 1) {
		text[0] = '\0';
		return;
	}
	mark = (unsigned)((phase - ZZ_CAPTURE_PHASE_MIN) / ZZ_CAPTURE_BIN_STEPS) / 4;
	if (mark >= ZZ_CAPTURE_BINS / 4) mark = ZZ_CAPTURE_BINS / 4 - 1;
	for (i = 0; i < ZZ_CAPTURE_BINS / 4; i++) {
		unsigned n = live.sweep[i * 4] + live.sweep[i * 4 + 1] +
			live.sweep[i * 4 + 2] + live.sweep[i * 4 + 3];
		text[i] = n == 4 ? '=' : n ? '+' : '.';
	}
	text[mark] = '|';
	text[ZZ_CAPTURE_BINS / 4] = '\0';
}

/* Report live state that Revert must restore or Save must confirm. */
BOOL video_live_pending(void)
{
	return live.crop_touched || live.phase_touched || live.geometry_touched;
}

/* Reject Save when touched live state is not confirmed. */
BOOL video_save_check(char *msg, UWORD size)
{
	struct zz_vcap_control control;
	struct zz_vcap_path path;
	struct zz_vcap_path applied;
	enum zz_vcap_result result;

	if (live.crop_touched) {
		if (!live.crop_confirmed || !live.crop_target_valid || !refresh_live() ||
			live.current.raw != live.crop_target ||
			!zz_vcap_control_unpack(live.current.raw, &control)) {
			write_message(msg, size, "Picture framing is not confirmed");
			return FALSE;
		}
		path_for(&path, cfg.edit.videocap_profile, cfg.edit.videocap_sample);
		applied.sample = control.sample;
		applied.full_width = control.full_width;
		if (!zz_vcap_path_equal(&path, &applied)) {
			write_message(msg, size,
				"Custom framing cannot be saved until the running capture path matches it");
			return FALSE;
		}
	}
	if (live.phase_touched) {
		if (!live.phase_confirmed) {
			write_message(msg, size, "The phase is not confirmed");
			return FALSE;
		}
		result = zz_vcap_phase_check(&card_io, live.phase_target);
		if (result != ZZ_VCAP_OK) {
			write_result(msg, size, "The phase is not confirmed: ", result);
			return FALSE;
		}
	}
	if (live.geometry_touched) {
		if (!live.geometry_confirmed) {
			write_message(msg, size, "The capture window is not confirmed");
			return FALSE;
		}
		result = zz_vcap_geometry_check(&card_io, live.geometry_width,
			live.geometry_height);
		if (result != ZZ_VCAP_OK) {
			write_result(msg, size, "The capture window is not confirmed: ", result);
			return FALSE;
		}
	}
	return TRUE;
}

/* Rebase live Revert anchors after a successful global Save. */
void video_saved(void)
{
	struct zz_vcap_geometry_state geometry;

	if (live.crop_touched && live.crop_confirmed && refresh_live() &&
		live.current.raw == live.crop_target) {
		live.crop_anchor = live.current;
		live.crop_anchor_valid = TRUE;
		live.crop_touched = FALSE;
		live.crop_target_valid = FALSE;
	}
	if (live.phase_touched && live.phase_confirmed &&
		zz_vcap_phase_check(&card_io, live.phase_target) == ZZ_VCAP_OK) {
		live.phase_anchor = live.phase_target;
		live.phase_anchor_valid = TRUE;
		live.phase_touched = FALSE;
	}
	if (live.geometry_touched && live.geometry_confirmed &&
		zz_vcap_geometry_read(&card_io, &geometry) == ZZ_VCAP_OK &&
		geometry.requested_width == live.geometry_width &&
		geometry.requested_height == live.geometry_height &&
		geometry.request_serial == geometry.applied_serial) {
		live.geometry_anchor = geometry;
		live.geometry_anchor_valid = TRUE;
		live.geometry_touched = FALSE;
	}
}

/* Restore every touched live setting to its pre-edit anchor. */
BOOL video_revert(char *msg, UWORD size)
{
	struct zz_vcap_snapshot applied;
	BOOL ok = TRUE;

	if (live.crop_touched) {
		if (!live.crop_anchor_valid || !apply_raw(live.crop_anchor.raw, &applied,
			NULL, TRUE)) {
			write_message(msg, size, "Could not restore live picture framing");
			return FALSE;
		}
		live.current = applied;
	}
	if (live.phase_touched && (!live.phase_anchor_valid ||
		!phase_apply(live.phase_anchor, TRUE, msg, size)))
		ok = FALSE;
	if (ok && live.geometry_touched && (!live.geometry_anchor_valid ||
		!geometry_apply(live.geometry_anchor.requested_width,
		live.geometry_anchor.requested_height, TRUE, msg, size)))
		ok = FALSE;
	if (!ok)
		return FALSE;
	live.crop_touched = FALSE;
	live.crop_confirmed = FALSE;
	live.crop_target_valid = FALSE;
	live.phase_touched = FALSE;
	live.phase_confirmed = FALSE;
	live.geometry_touched = FALSE;
	live.geometry_confirmed = FALSE;
	live.sweep_valid = FALSE;
	(void)refresh_live();
	return TRUE;
}
