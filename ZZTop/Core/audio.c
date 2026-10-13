/*
 * Firmware audio-control-plane client and live editor model.
 * Copyright (C) 2016-2026, Lucie L. Hartmann <lucie@mntre.com>
 *                          MNT Research GmbH, Berlin
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <exec/types.h>
#include <clib/debug_protos.h>
#include <proto/dos.h>
#include <proto/zz9k.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "audio.h"
#include "zztop.h"
#include "board.h"
#include "settings.h"
#include "frontend.h"
#include "zz9k/abi.h"
#include "zz9k/request.h"

#define AUDIO_LEVEL_MAX 255U
#define AUDIO_LPF_MIN 1U
#define AUDIO_LPF_MAX 23900U
#define AUDIO_RANGE_MAX 100U
#define AUDIO_NAME_CHUNKS 8U
#define AUDIO_CALIBRATION_WAIT_TICKS 100UL
#define AUDIO_TICKS_PER_MINUTE (60UL * 50UL)
#define AUDIO_MINUTES_PER_DAY (24UL * 60UL)

struct audio_model {
	struct audio_scene scenes[AUDIO_SCENE_COUNT];
	UWORD active;
	UWORD base_paula;
	UWORD base_ax;
	UWORD ceiling_paula;
	UWORD ceiling_ax;
};

static struct audio_model model;
/* Scenes have no firmware readback, so their anchors remain CFG-seeded values. */
static struct audio_model revert_anchor;
static ULONG boundary;
static ULONG save_status;
static ULONG edit_generation;
static ULONG save_generation;
static ULONG changed_params[AUDIO_SCENE_COUNT];
static BOOL changed_names[AUDIO_SCENE_COUNT];
static BOOL changed_active;
static BOOL changed_baseline;
static BOOL changed_calibration;
static BOOL available;
static BOOL retryable;
static BOOL dirty;
static BOOL save_pending;
static BOOL calibration_pending;
static BOOL log_on;
static UWORD calibration_paula;
static UWORD calibration_ax;
static struct audio_meter meters[2];
static char unavailable[80];
static char last_error[96];
static BPTR log_file;

/* Copies a short outcome into a caller-provided or model-owned buffer. */
static void audio_text(char *dst, UWORD size, const char *text)
{
	if (size != 0)
		snprintf(dst, size, "%s", text);
}

/* Writes the same diagnostics to a fresh T: file and the serial debug port. */
static void audio_log(const char *fmt, ...)
{
	static char text[200];
	va_list ap;

	if (!log_on)
		return;
	va_start(ap, fmt);
	vsnprintf(text, sizeof(text), fmt, ap);
	va_end(ap);
	if (log_file)
		Write(log_file, text, strlen(text));
	KPrintF((CONST_STRPTR)"ZZTop %s", text);
}

/* Opens a new session log on enable and closes it on disable. */
void audio_log_enable(BOOL on)
{
	if (on == log_on)
		return;
	if (log_file) {
		Close(log_file);
		log_file = 0;
	}
	log_on = on;
	if (on) {
		log_file = Open((CONST_STRPTR)"T:zztop-audio.log", MODE_NEWFILE);
		audio_log("log opened (fh=%ld)\n", (LONG)(log_file != 0));
	} else {
		KPrintF((CONST_STRPTR)"ZZTop audio log OFF\n");
	}
}

/* Reports whether the session debug log is enabled. */
BOOL audio_log_enabled(void)
{
	return log_on;
}

/* Makes one typed inline mailbox request and records both payload ends. */
static int audio_mb_call(UWORD opcode, const void *payload, UWORD length,
	ZZ9KMailboxEntry *reply)
{
	ZZ9KRequest request;
	int status;

	if (!board.zz9k_library || !ZZ9KBase || !reply ||
		length > sizeof(request.entry.payload.inline_data))
		return ZZ9K_STATUS_UNSUPPORTED;
	zz9k_request_init(&request, opcode);
	request.entry.payload_len = length;
	memcpy(request.entry.payload.inline_data, payload, length);
	status = ZZ9KCall(&request, reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
	audio_log("op=%04x st=%ld req=%08lx,%08lx rsp=%08lx,%08lx\n",
		(unsigned)opcode, (LONG)status,
		(ULONG)zz9k_get_be32(request.entry.payload.inline_data),
		(ULONG)zz9k_get_be32(request.entry.payload.inline_data + 4),
		(ULONG)zz9k_get_be32(reply->payload.inline_data),
		(ULONG)zz9k_get_be32(reply->payload.inline_data + 4));
	return status;
}

/* Stages one control-plane parameter, optionally asking firmware to commit it. */
static int audio_scene_write_stage(UWORD scene, ULONG param, ULONG value,
	ULONG flags)
{
	ZZ9KAudioSceneWritePayload payload;
	ZZ9KMailboxEntry reply;

	memset(&payload, 0, sizeof(payload));
	zz9k_put_be32(payload.scene, scene);
	zz9k_put_be32(payload.param, param);
	zz9k_put_be32(payload.value, value);
	zz9k_put_be32(payload.flags, flags);
	audio_log("write scene=%u param=%lu value=%lu flags=%08lx\n",
		(unsigned)scene + 1U, param, value, flags);
	return audio_mb_call(ZZ9K_OP_AUDIO_SCENE_WRITE, &payload,
		sizeof(payload), &reply);
}

/* Commits a one-parameter atomic firmware update. */
static int audio_scene_write_commit(UWORD scene, ULONG param, ULONG value)
{
	return audio_scene_write_stage(scene, param, value,
		ZZ9K_AUDIO_SCENE_WRITE_FLAG_COMMIT);
}

/* Obtains one coherent firmware state snapshot without partially installing it. */
static BOOL audio_control_state_get(struct audio_model *out, ULONG *out_save)
{
	ZZ9KAudioControlStateGetPayload payload;
	ZZ9KAudioControlStateResultPayload result;
	ZZ9KMailboxEntry reply;

	memset(&payload, 0, sizeof(payload));
	if (audio_mb_call(ZZ9K_OP_AUDIO_CONTROL_STATE_GET, &payload,
		sizeof(payload), &reply) != ZZ9K_STATUS_OK)
		return FALSE;
	memcpy(&result, reply.payload.inline_data, sizeof(result));
	if (zz9k_get_be32(result.ceiling) == 0 ||
		zz9k_get_be32(result.ceiling_paula) == 0 ||
		zz9k_get_be32(result.ceiling_ax) == 0)
		return FALSE;
	if (out) {
		out->active = (UWORD)zz9k_get_be32(result.active_scene);
		out->base_paula = (UWORD)ZZ9K_AUDIO_BALANCE_CH1(
			zz9k_get_be32(result.baseline));
		out->base_ax = (UWORD)ZZ9K_AUDIO_BALANCE_CH2(
			zz9k_get_be32(result.baseline));
		out->ceiling_paula = (UWORD)zz9k_get_be32(result.ceiling_paula);
		out->ceiling_ax = (UWORD)zz9k_get_be32(result.ceiling_ax);
	}
	boundary = zz9k_get_be32(result.ceiling);
	save_status = zz9k_get_be32(result.save_status);
	if (out_save)
		*out_save = save_status;
	return TRUE;
}

/* Installs the firmware state fields as one snapshot so their boundary agrees. */
static BOOL audio_install_live_state(void)
{
	struct audio_model live = model;

	if (!audio_control_state_get(&live, NULL))
		return FALSE;
	if (live.active >= AUDIO_SCENE_COUNT)
		return FALSE;
	model.active = live.active;
	model.base_paula = live.base_paula;
	model.base_ax = live.base_ax;
	model.ceiling_paula = live.ceiling_paula;
	model.ceiling_ax = live.ceiling_ax;
	return TRUE;
}

/* Supplies the firmware's eight scene defaults before saved CFG keys overlay them. */
static void audio_scene_defaults(struct audio_model *out)
{
	static const struct audio_scene defaults[AUDIO_SCENE_COUNT] = {
		{ 23900, { 50, 50, 50, 50, 50, 50, 50, 50, 50, 50 }, 50, 100, 50, "" },
		{ 23900, { 50, 50, 50, 50, 50, 50, 50, 50, 50, 50 }, 50, 80, 50, "" },
		{ 16000, { 55, 55, 50, 50, 50, 50, 50, 50, 50, 50 }, 50, 75, 50, "" },
		{ 18000, { 50, 50, 50, 50, 50, 50, 55, 55, 50, 50 }, 50, 75, 50, "" },
		{ 23900, { 50, 50, 50, 50, 50, 50, 50, 50, 50, 50 }, 60, 70, 50, "" },
		{ 12000, { 45, 45, 50, 50, 50, 50, 50, 50, 45, 45 }, 50, 90, 50, "" },
		{ 23900, { 50, 50, 50, 50, 50, 50, 50, 50, 50, 50 }, 55, 75, 50, "" },
		{ 23900, { 50, 50, 50, 50, 50, 50, 50, 50, 50, 50 }, 50, 60, 50, "" }
	};
	UWORD i;

	memset(out, 0, sizeof(*out));
	for (i = 0; i < AUDIO_SCENE_COUNT; ++i) {
		out->scenes[i] = defaults[i];
		snprintf(out->scenes[i].name, sizeof(out->scenes[i].name),
			"Scene %u", (unsigned)i + 1U);
	}
	out->base_paula = 36;
	out->base_ax = 72;
	out->ceiling_paula = 48;
	out->ceiling_ax = 80;
}

/* Makes a CFG label firmware-safe: printable ASCII, trimmed and bounded. */
static void audio_clean_name(char *out, const char *in)
{
	UWORD n = 0;

	while (*in && n < AUDIO_NAME_CHARS) {
		if (*in >= 0x20 && *in <= 0x7e)
			out[n++] = *in;
		++in;
	}
	while (n != 0 && out[n - 1] == ' ')
		--n;
	out[n] = 0;
}

/* Decodes the saved CFG name chunks while refusing malformed hand-edited text. */
static void audio_seed_name(struct audio_scene *scene,
	const struct zzcfg_values *values, UWORD index)
{
	UWORD mask = values->audio_scene_mask[index];
	UWORD at = 0;
	UWORD chunk;

	if ((mask & (1U << 8)) == 0)
		return;
	for (chunk = 0; chunk < AUDIO_NAME_CHUNKS && at < AUDIO_NAME_CHARS;
		++chunk) {
		UWORD word;
		UBYTE first;
		UBYTE second;

		if ((mask & (1U << (8U + chunk))) == 0)
			break;
		word = values->audio_scene_nm[index][chunk];
		first = (UBYTE)(word >> 8);
		second = (UBYTE)word;
		if (first < 0x20 || first > 0x7e)
			break;
		scene->name[at++] = (char)first;
		if (second < 0x20 || second > 0x7e || at == AUDIO_NAME_CHARS)
			break;
		scene->name[at++] = (char)second;
	}
	if (at != 0)
		scene->name[at] = 0;
}

/* Seeds a complete editor snapshot from the saved card file and clamps its inputs. */
static void audio_seed_editor_state(struct audio_model *out)
{
	const struct zzcfg_values *values = &cfg.saved;
	UWORD scene;
	UWORD pair;

	audio_scene_defaults(out);
	for (scene = 0; scene < AUDIO_SCENE_COUNT; ++scene) {
		UWORD mask = values->audio_scene_mask[scene];

		if (mask & 1U)
			out->scenes[scene].lpf = values->audio_scene_lpf[scene];
		for (pair = 0; pair < 5U; ++pair) {
			UWORD packed = values->audio_scene_eq[scene][pair];

			if (mask & (1U << (pair + 1U))) {
				out->scenes[scene].eq[pair * 2U] = packed / 128U;
				out->scenes[scene].eq[pair * 2U + 1U] = packed % 128U;
			}
		}
		if (mask & (1U << 6)) {
			out->scenes[scene].prefactor = values->audio_scene_out[scene] / 128U;
			out->scenes[scene].volume = values->audio_scene_out[scene] % 128U;
		}
		if (mask & (1U << 7))
			out->scenes[scene].pan = values->audio_scene_pan[scene];
		audio_seed_name(&out->scenes[scene], values, scene);
		if (out->scenes[scene].lpf < AUDIO_LPF_MIN ||
			out->scenes[scene].lpf > AUDIO_LPF_MAX)
			out->scenes[scene].lpf = AUDIO_LPF_MAX;
		if (out->scenes[scene].prefactor > AUDIO_RANGE_MAX)
			out->scenes[scene].prefactor = AUDIO_RANGE_MAX;
		if (out->scenes[scene].volume > AUDIO_RANGE_MAX)
			out->scenes[scene].volume = AUDIO_RANGE_MAX;
		if (out->scenes[scene].pan > AUDIO_RANGE_MAX)
			out->scenes[scene].pan = AUDIO_RANGE_MAX;
		for (pair = 0; pair < AUDIO_EQ_BANDS; ++pair) {
			if (out->scenes[scene].eq[pair] > AUDIO_RANGE_MAX)
				out->scenes[scene].eq[pair] = AUDIO_RANGE_MAX;
		}
	}
	if (values->audio_baseline_present) {
		out->base_paula = values->audio_baseline >> 8;
		out->base_ax = values->audio_baseline & 255U;
	}
	if (values->audio_ceiling_paula_present &&
		values->audio_ceiling_ax_present) {
		out->ceiling_paula = values->audio_ceiling_paula;
		out->ceiling_ax = values->audio_ceiling_ax;
	}
	if (out->ceiling_paula == 0 || out->ceiling_paula > ZZ9K_AUDIO_CEILING_MAX)
		out->ceiling_paula = 48;
	if (out->ceiling_ax == 0 || out->ceiling_ax > ZZ9K_AUDIO_CEILING_MAX)
		out->ceiling_ax = 80;
	if (!values->audio_baseline_present) {
		out->base_paula = (UWORD)(((ULONG)out->ceiling_paula * 3UL) / 4UL);
		out->base_ax = (UWORD)((ULONG)out->base_paula * 2UL);
		if (out->base_ax > out->ceiling_ax)
			out->base_ax = out->ceiling_ax;
		if (out->base_paula > AUDIO_LEVEL_MAX)
			out->base_paula = AUDIO_LEVEL_MAX;
		if (out->base_ax > AUDIO_LEVEL_MAX)
			out->base_ax = AUDIO_LEVEL_MAX;
	}
	if (values->audio_active_present && values->audio_active < AUDIO_SCENE_COUNT)
		out->active = values->audio_active;
}

/* Clears edit tracking after an initial load, a completed save, or a full revert. */
static void audio_clear_edits(void)
{
	memset(changed_params, 0, sizeof(changed_params));
	memset(changed_names, 0, sizeof(changed_names));
	changed_active = FALSE;
	changed_baseline = FALSE;
	changed_calibration = FALSE;
	dirty = FALSE;
	save_pending = FALSE;
}

/* Marks an accepted live write and advances the paired save generation. */
static void audio_mark_dirty(void)
{
	dirty = TRUE;
	++edit_generation;
}

/* Sets plain-language unavailability for permanent and retryable startup failures. */
static BOOL audio_open_control_plane(void)
{
	available = FALSE;
	retryable = FALSE;
	if (!board_ax_present()) {
		audio_text(unavailable, sizeof(unavailable), "No ZZ9000AX daughterboard");
		return FALSE;
	}
	if (!board.zz9k_library) {
		audio_text(unavailable, sizeof(unavailable), "zz9k.library not installed");
		return FALSE;
	}
	if ((board.zz9k_caps & ZZ9K_CAP_AUDIO_CONTROL) == 0) {
		audio_text(unavailable, sizeof(unavailable),
			"Firmware lacks the audio control plane");
		return FALSE;
	}
	if (!audio_install_live_state()) {
		retryable = TRUE;
		audio_text(unavailable, sizeof(unavailable),
			"Card did not answer the audio control-state request");
		return FALSE;
	}
	available = TRUE;
	unavailable[0] = 0;
	return TRUE;
}

/* Seeds saved settings then gives the coherent live control state precedence. */
void audio_init(void)
{
	audio_seed_editor_state(&model);
	boundary = 0;
	edit_generation = 0;
	save_generation = 0;
	calibration_pending = FALSE;
	memset(meters, 0, sizeof(meters));
	audio_clear_edits();
	last_error[0] = 0;
	(void)audio_open_control_plane();
	/* Live control fields win at entry; scenes have no live readback. */
	revert_anchor = model;
}

/* Releases the debug file; mailbox state belongs to the card. */
void audio_shutdown(void)
{
	audio_log_enable(FALSE);
}

/* Reports whether the fully gated live editor is usable. */
BOOL audio_available(void)
{
	return available;
}

/* Returns a page-displayable explanation of why the editor is disabled. */
const char *audio_unavailable_reason(void)
{
	return unavailable;
}

/* Reports whether a Retry button can cure the present unavailable state. */
BOOL audio_retry_available(void)
{
	return retryable;
}

/* Retries only the transient firmware state request without discarding edits. */
BOOL audio_retry(void)
{
	if (!retryable)
		return FALSE;
	if (audio_open_control_plane()) {
		audio_text(last_error, sizeof(last_error), "Audio control state ready");
		return TRUE;
	}
	return FALSE;
}

/* Reports pending firmware-RAM state rather than merely pending card-file fields. */
BOOL audio_pending(void)
{
	return dirty || save_pending;
}

/* Returns the last user-facing live-control outcome. */
const char *audio_last_error(void)
{
	return last_error;
}

/* Returns the current live-selected scene. */
UWORD audio_active_scene(void)
{
	return model.active;
}

/* Returns a read-only scene model when its index is valid. */
const struct audio_scene *audio_scene(UWORD index)
{
	if (index >= AUDIO_SCENE_COUNT)
		return NULL;
	return &model.scenes[index];
}

/* Returns the live Paula baseline leg. */
UWORD audio_baseline_paula(void)
{
	return model.base_paula;
}

/* Returns the live AX baseline leg. */
UWORD audio_baseline_ax(void)
{
	return model.base_ax;
}

/* Returns the live Paula clean ceiling. */
UWORD audio_ceiling_paula(void)
{
	return model.ceiling_paula;
}

/* Returns the live AX clean ceiling. */
UWORD audio_ceiling_ax(void)
{
	return model.ceiling_ax;
}

/* Returns the firmware-reported AX-equivalent limit. */
ULONG audio_boundary(void)
{
	return boundary;
}

/* Computes the exact firmware-weighted mix level using integer arithmetic. */
ULONG audio_weighted_level(void)
{
	ULONG denominator = model.ceiling_paula ? model.ceiling_paula : 1UL;

	return ((ULONG)model.base_paula * model.ceiling_ax) / denominator +
		model.base_ax;
}

/* Keeps the maximum at least as high as firmware's current Paula level. */
UWORD audio_baseline_max_paula(void)
{
	ULONG max = model.ceiling_paula;
	ULONG free_weighted;

	if (model.ceiling_ax == 0)
		return 0;
	free_weighted = boundary;
	if (free_weighted > model.base_ax)
		free_weighted -= model.base_ax;
	else
		free_weighted = 0;
	free_weighted = free_weighted * model.ceiling_paula / model.ceiling_ax;
	if (free_weighted < max)
		max = free_weighted;
	if (max > AUDIO_LEVEL_MAX)
		max = AUDIO_LEVEL_MAX;
	if (max < model.base_paula && model.base_paula <= AUDIO_LEVEL_MAX)
		max = model.base_paula;
	return (UWORD)max;
}

/* Keeps the maximum at least as high as firmware's current AX level. */
UWORD audio_baseline_max_ax(void)
{
	ULONG max = model.ceiling_ax;
	ULONG used;
	ULONG free_weighted;

	if (model.ceiling_ax == 0 || model.ceiling_paula == 0)
		return 0;
	used = ((ULONG)model.base_paula * model.ceiling_ax) / model.ceiling_paula;
	free_weighted = boundary;
	if (free_weighted > used)
		free_weighted -= used;
	else
		free_weighted = 0;
	if (free_weighted < max)
		max = free_weighted;
	if (max > AUDIO_LEVEL_MAX)
		max = AUDIO_LEVEL_MAX;
	if (max < model.base_ax && model.base_ax <= AUDIO_LEVEL_MAX)
		max = model.base_ax;
	return (UWORD)max;
}

/* Reports whether the firmware has yet acknowledged the paired calibration write. */
BOOL audio_calibration_pending(void)
{
	return calibration_pending;
}

/* Reports whether this firmware advertised the separately gated meter protocol. */
BOOL audio_metering_available(void)
{
	return available && (board.zz9k_caps & ZZ9K_CAP_AUDIO_METERING) != 0;
}

/* Copies the most recent framed meter state for the requested direction. */
BOOL audio_get_meter(BOOL input, struct audio_meter *out)
{
	if (!out || !audio_metering_available())
		return FALSE;
	*out = meters[input ? 1 : 0];
	return out->valid;
}

/* Treats only timeouts as applied-or-coalesced; all other results are hard errors. */
static BOOL audio_write_accepted(int status, const char *ok, const char *pending,
	const char *failed)
{
	if (status == ZZ9K_STATUS_OK) {
		audio_text(last_error, sizeof(last_error), ok);
		return TRUE;
	}
	if (status == ZZ9K_STATUS_TIMEOUT) {
		audio_text(last_error, sizeof(last_error), pending);
		return TRUE;
	}
	audio_text(last_error, sizeof(last_error), failed);
	return FALSE;
}

/* Selects a scene live and retains it as a precise reversible edit. */
BOOL audio_select_scene(UWORD scene)
{
	ZZ9KAudioSceneSelectPayload payload;
	ZZ9KMailboxEntry reply;
	int status;

	if (!available || calibration_pending || scene >= AUDIO_SCENE_COUNT)
		return FALSE;
	memset(&payload, 0, sizeof(payload));
	zz9k_put_be32(payload.scene, scene);
	status = audio_mb_call(ZZ9K_OP_AUDIO_SCENE_SELECT, &payload,
		sizeof(payload), &reply);
	if (status != ZZ9K_STATUS_OK) {
		audio_text(last_error, sizeof(last_error), "Scene switch failed - retry");
		return FALSE;
	}
	model.active = scene;
	changed_active = TRUE;
	audio_mark_dirty();
	audio_text(last_error, sizeof(last_error), "Scene selected - Save to persist");
	return TRUE;
}

/* Writes both baseline legs atomically and restores both legs after a hard failure. */
BOOL audio_set_baseline(UWORD paula, UWORD ax)
{
	UWORD old_paula = model.base_paula;
	UWORD old_ax = model.base_ax;
	int status;

	if (!available || calibration_pending)
		return FALSE;
	status = audio_scene_write_commit(model.active,
		ZZ9K_AUDIO_SCENE_PARAM_BASELINE, ZZ9K_AUDIO_BALANCE_PACK(paula, ax));
	if (!audio_write_accepted(status, "Baseline committed - Save to persist",
		"Baseline committing - Save to persist", "Baseline write failed - retry")) {
		(void)audio_scene_write_commit(model.active,
			ZZ9K_AUDIO_SCENE_PARAM_BASELINE,
			ZZ9K_AUDIO_BALANCE_PACK(old_paula, old_ax));
		return FALSE;
	}
	model.base_paula = paula;
	model.base_ax = ax;
	changed_baseline = TRUE;
	audio_mark_dirty();
	return TRUE;
}

/* Applies the hardware-qualified 3/4 Paula, two-times-AX parity preset. */
BOOL audio_balance(void)
{
	ULONG paula = ((ULONG)model.ceiling_paula * 3UL) / 4UL;
	ULONG ax = paula * 2UL;

	if (ax > model.ceiling_ax)
		ax = model.ceiling_ax;
	if (paula > AUDIO_LEVEL_MAX)
		paula = AUDIO_LEVEL_MAX;
	if (ax > AUDIO_LEVEL_MAX)
		ax = AUDIO_LEVEL_MAX;
	return audio_set_baseline((UWORD)paula, (UWORD)ax);
}

/* Writes paired ceilings and waits for one future state snapshot to install them. */
BOOL audio_set_calibration(UWORD paula, UWORD ax)
{
	UWORD old_paula = model.ceiling_paula;
	UWORD old_ax = model.ceiling_ax;
	int status;

	if (!available || calibration_pending || paula < ZZ9K_AUDIO_CEILING_MIN ||
		paula > ZZ9K_AUDIO_CEILING_MAX || ax < ZZ9K_AUDIO_CEILING_MIN ||
		ax > ZZ9K_AUDIO_CEILING_MAX) {
		audio_text(last_error, sizeof(last_error), "Calibration must be 1..4095");
		return FALSE;
	}
	status = audio_scene_write_commit(model.active,
		ZZ9K_AUDIO_SCENE_PARAM_CALIBRATION,
		ZZ9K_AUDIO_CALIBRATION_PACK(paula, ax));
	if (!audio_write_accepted(status, "Calibration awaiting firmware state...",
		"Calibration awaiting firmware state...", "Calibration write failed - retry")) {
		(void)audio_scene_write_commit(model.active,
			ZZ9K_AUDIO_SCENE_PARAM_CALIBRATION,
			ZZ9K_AUDIO_CALIBRATION_PACK(old_paula, old_ax));
		return FALSE;
	}
	calibration_paula = paula;
	calibration_ax = ax;
	calibration_pending = TRUE;
	changed_calibration = TRUE;
	audio_mark_dirty();
	return TRUE;
}

/* Maps a supplied scene's editable parameter to its model storage. */
static UWORD *audio_scene_value(struct audio_scene *scene, UWORD param)
{
	if (param == ZZ9K_AUDIO_SCENE_PARAM_LPF)
		return &scene->lpf;
	if (param >= ZZ9K_AUDIO_SCENE_PARAM_EQ_BAND_1 &&
		param <= ZZ9K_AUDIO_SCENE_PARAM_EQ_BAND_10)
		return &scene->eq[param - ZZ9K_AUDIO_SCENE_PARAM_EQ_BAND_1];
	if (param == ZZ9K_AUDIO_SCENE_PARAM_PREFACTOR)
		return &scene->prefactor;
	if (param == ZZ9K_AUDIO_SCENE_PARAM_VOLUME)
		return &scene->volume;
	if (param == ZZ9K_AUDIO_SCENE_PARAM_PAN)
		return &scene->pan;
	return NULL;
}

/* Maps a parameter to a mutable field in the current scene model. */
static UWORD *audio_scene_field(UWORD scene, UWORD param)
{
	return audio_scene_value(&model.scenes[scene], param);
}

/* Commits a scene value and re-sends the old value on every hard error. */
BOOL audio_set_scene_value(UWORD scene, UWORD param, UWORD value)
{
	UWORD *field;
	UWORD old;
	int status;
	int rollback;
	if (!available || calibration_pending || scene >= AUDIO_SCENE_COUNT)
		return FALSE;
	field = audio_scene_field(scene, param);
	if (!field)
		return FALSE;
	old = *field;
	status = audio_scene_write_commit(scene, param, value);
	if (!audio_write_accepted(status, "Scene change committed - Save to persist",
		"Scene change committing - Save to persist", "Scene change failed - retry")) {
		rollback = audio_scene_write_commit(scene, param, old);
		if (rollback != ZZ9K_STATUS_OK) {
			*field = value;
			changed_params[scene] |= 1UL << param;
			audio_mark_dirty();
			audio_text(last_error, sizeof(last_error),
				"Scene change failed; firmware may hold the attempted value - Save");
		}
		return FALSE;
	}
	*field = value;
	changed_params[scene] |= 1UL << param;
	audio_mark_dirty();
	return TRUE;
}

/* Sends exactly one self-healing staged name burst and commits only its final chunk. */
static int audio_scene_rename_call(UWORD scene, const char *name)
{
	UWORD chunk;
	UWORD length = (UWORD)strlen(name);
	int status = ZZ9K_STATUS_OK;

	for (chunk = 0; chunk <= AUDIO_NAME_CHUNKS; ++chunk) {
		ULONG value = 0;
		ULONG flags = 0;
		UWORD at;

		if (chunk != 0) {
			at = (UWORD)((chunk - 1U) * 2U);
			if (at < length)
				value |= (ULONG)(UBYTE)name[at] << 8;
			if (at + 1U < length)
				value |= (UBYTE)name[at + 1U];
		}
		if (chunk == AUDIO_NAME_CHUNKS)
			flags = ZZ9K_AUDIO_SCENE_WRITE_FLAG_COMMIT;
		status = audio_scene_write_stage(scene, ZZ9K_AUDIO_SCENE_PARAM_NAME,
			value, flags);
		if (status != ZZ9K_STATUS_OK && status != ZZ9K_STATUS_TIMEOUT)
			return status;
	}
	return status;
}

/* Renames a scene atomically, re-sending the former full name after a failed burst. */
BOOL audio_rename_scene(UWORD scene, const char *name)
{
	char clean[AUDIO_NAME_CHARS + 1];
	char old[AUDIO_NAME_CHARS + 1];
	int status;

	if (!available || calibration_pending || scene >= AUDIO_SCENE_COUNT)
		return FALSE;
	audio_clean_name(clean, name);
	if (!clean[0])
		snprintf(clean, sizeof(clean), "Scene %u", (unsigned)scene + 1U);
	snprintf(old, sizeof(old), "%s", model.scenes[scene].name);
	status = audio_scene_rename_call(scene, clean);
	if (!audio_write_accepted(status, "Rename committed - Save to persist",
		"Rename committing - Save to persist", "Rename failed - retry")) {
		(void)audio_scene_rename_call(scene, old);
		return FALSE;
	}
	snprintf(model.scenes[scene].name, sizeof(model.scenes[scene].name),
		"%s", clean);
	changed_names[scene] = TRUE;
	audio_mark_dirty();
	return TRUE;
}

/* Starts the asynchronous firmware save and snapshots its edit generation. */
BOOL audio_save_start(char *msg, UWORD size)
{
	ZZ9KAudioSceneSavePayload payload;
	ZZ9KAudioSceneSaveResultPayload result;
	ZZ9KMailboxEntry reply;
	ULONG status;

	if (!dirty) {
		audio_text(msg, size, "No audio changes");
		return TRUE;
	}
	if (!available || calibration_pending) {
		audio_text(msg, size, calibration_pending ?
			"Calibration awaiting firmware state" : unavailable);
		return FALSE;
	}
	memset(&payload, 0, sizeof(payload));
	zz9k_put_be32(payload.scene, model.active);
	if (audio_mb_call(ZZ9K_OP_AUDIO_SCENE_SAVE, &payload,
		sizeof(payload), &reply) != ZZ9K_STATUS_OK) {
		audio_text(msg, size, "Save failed: no firmware reply");
		return FALSE;
	}
	memcpy(&result, reply.payload.inline_data, sizeof(result));
	status = zz9k_get_be32(result.status);
	if (status == ZZ9K_AUDIO_SCENE_SAVE_QUEUED) {
		save_generation = edit_generation;
		save_pending = TRUE;
		audio_text(msg, size, "Saving...");
		return TRUE;
	}
	if (status == ZZ9K_AUDIO_SCENE_SAVE_OK) {
		save_generation = edit_generation;
		save_pending = TRUE;
		return audio_save_poll(msg, size) == AUDIO_SAVE_DONE;
	}
	audio_text(msg, size, status == ZZ9K_AUDIO_SCENE_SAVE_BUSY ?
		"Save busy - retry" : status == ZZ9K_AUDIO_SCENE_SAVE_REJECTED ?
		"Save rejected: level over boundary" : "Save failed: SD card write error");
	return FALSE;
}

/* Settles a queued save only after the control-state status confirms its result. */
enum audio_save_state audio_save_poll(char *msg, UWORD size)
{
	ULONG status;

	if (!save_pending)
		return AUDIO_SAVE_DONE;
	if (!audio_control_state_get(NULL, &status))
		return AUDIO_SAVE_RUNNING;
	if (status == ZZ9K_AUDIO_SCENE_SAVE_QUEUED)
		return AUDIO_SAVE_RUNNING;
	save_pending = FALSE;
	if (status != ZZ9K_AUDIO_SCENE_SAVE_OK) {
		audio_text(msg, size, status == ZZ9K_AUDIO_SCENE_SAVE_REJECTED ?
			"Save rejected: level over boundary" : status == ZZ9K_AUDIO_SCENE_SAVE_IO_ERROR ?
			"Save failed: SD card write error" : "Save busy - retry");
		return AUDIO_SAVE_FAILED;
	}
	if (save_generation != edit_generation) {
		audio_text(msg, size, "Saved, but newer changes still need Save");
		return AUDIO_SAVE_FAILED;
	}
	revert_anchor = model;
	audio_clear_edits();
	audio_text(msg, size, "Saved - survives power-cycle");
	return AUDIO_SAVE_DONE;
}

/* Sets an absolute two-second DateStamp deadline for blocking revert acknowledgement. */
static void audio_calibration_deadline(struct DateStamp *deadline)
{
	DateStamp(deadline);
	deadline->ds_Tick += AUDIO_CALIBRATION_WAIT_TICKS;
	while (deadline->ds_Tick >= (LONG)AUDIO_TICKS_PER_MINUTE) {
		deadline->ds_Tick -= (LONG)AUDIO_TICKS_PER_MINUTE;
		deadline->ds_Minute++;
		if (deadline->ds_Minute >= (LONG)AUDIO_MINUTES_PER_DAY) {
			deadline->ds_Minute = 0;
			deadline->ds_Days++;
		}
	}
}

/* Compares two DateStamps without assuming a mailbox request has a fixed cost. */
static BOOL audio_deadline_reached(const struct DateStamp *now,
	const struct DateStamp *deadline)
{
	if (now->ds_Days != deadline->ds_Days)
		return now->ds_Days > deadline->ds_Days;
	if (now->ds_Minute != deadline->ds_Minute)
		return now->ds_Minute > deadline->ds_Minute;
	return now->ds_Tick >= deadline->ds_Tick;
}

/* Waits at most two wall-clock seconds and installs the matching firmware snapshot. */
static BOOL audio_wait_calibration(UWORD paula, UWORD ax)
{
	struct DateStamp deadline;
	struct DateStamp now;

	audio_calibration_deadline(&deadline);
	for (;;) {
		if (audio_install_live_state() && model.ceiling_paula == paula &&
			model.ceiling_ax == ax) {
			calibration_pending = FALSE;
			audio_text(last_error, sizeof(last_error),
				"Calibration committed - Save to persist");
			return TRUE;
		}
		DateStamp(&now);
		if (audio_deadline_reached(&now, &deadline))
			return FALSE;
		Delay(5);
	}
}

/* Replays only tracked edits back to saved state, in safe calibration-first order. */
BOOL audio_revert(char *msg, UWORD size)
{
	struct audio_model wanted;
	UWORD scene;
	UWORD param;
	BOOL ok = TRUE;

	if (!available) {
		audio_text(msg, size, unavailable);
		return FALSE;
	}
	wanted = revert_anchor;
	/* Finish an outstanding request before deliberately restoring the entry pair. */
	if (calibration_pending &&
		!audio_wait_calibration(calibration_paula, calibration_ax))
		ok = FALSE;
	if (ok && changed_calibration) {
		if (!audio_set_calibration(wanted.ceiling_paula, wanted.ceiling_ax) ||
			!audio_wait_calibration(wanted.ceiling_paula, wanted.ceiling_ax))
			ok = FALSE;
	}
	if (ok && changed_baseline &&
		(model.base_paula != wanted.base_paula || model.base_ax != wanted.base_ax) &&
		!audio_set_baseline(wanted.base_paula, wanted.base_ax))
		ok = FALSE;
	if (ok && changed_active && model.active != wanted.active &&
		!audio_select_scene(wanted.active))
		ok = FALSE;
	for (scene = 0; ok && scene < AUDIO_SCENE_COUNT; ++scene) {
		for (param = ZZ9K_AUDIO_SCENE_PARAM_LPF;
			param <= ZZ9K_AUDIO_SCENE_PARAM_PAN; ++param) {
			UWORD *value;

			if ((changed_params[scene] & (1UL << param)) == 0)
				continue;
			value = audio_scene_value(&wanted.scenes[scene], param);
			if (!value || *audio_scene_field(scene, param) == *value)
				continue;
			if (!audio_set_scene_value(scene, param, *value))
				ok = FALSE;
		}
		if (ok && changed_names[scene] &&
			strcmp(model.scenes[scene].name, wanted.scenes[scene].name) != 0 &&
			!audio_rename_scene(scene, wanted.scenes[scene].name))
			ok = FALSE;
	}
	if (ok) {
		model = wanted;
		calibration_pending = FALSE;
		audio_clear_edits();
		audio_text(msg, size, "Audio reverted to saved settings");
	} else {
		audio_text(msg, size, "Audio revert incomplete; retry");
	}
	return ok;
}

/* Reads one complete meter frame and refuses torn or future multi-frame replies. */
static void audio_read_meter(BOOL input)
{
	ZZ9KAudioMeterReadPayload payload;
	ZZ9KAudioMeterResultPayload result;
	ZZ9KMailboxEntry reply;
	struct audio_meter *meter = &meters[input ? 1 : 0];
	ULONG direction = input ? ZZ9K_AUDIO_METER_DIRECTION_CAPTURE :
		ZZ9K_AUDIO_METER_DIRECTION_OUTPUT;

	memset(&payload, 0, sizeof(payload));
	zz9k_put_be32(payload.direction, direction);
	zz9k_put_be32(payload.flags, ZZ9K_AUDIO_METER_RESULT_HOLD_RESET);
	if (audio_mb_call(ZZ9K_OP_AUDIO_METER_READ, &payload,
		sizeof(payload), &reply) != ZZ9K_STATUS_OK)
		return;
	memcpy(&result, reply.payload.inline_data, sizeof(result));
	if (zz9k_get_be32(result.direction) != direction ||
		zz9k_get_be32(result.frame_count) != 1U ||
		zz9k_get_be32(result.frame) != 0U)
		return;
	meter->peak_left = zz9k_get_be32(result.peak_hold_ch1);
	meter->peak_right = zz9k_get_be32(result.peak_hold_ch2);
	meter->clip = zz9k_get_be32(result.clip_count);
	meter->underrun = zz9k_get_be32(result.underrun_count);
	meter->overrun = zz9k_get_be32(result.overrun_count);
	meter->limiter = zz9k_get_be32(result.gain_reduction_events);
	meter->identity = zz9k_get_be32(result.identity);
	meter->valid = TRUE;
}

/* Advances calibration ACK and meter state machines independently of page visibility. */
void audio_tick(void)
{
	if (!available)
		return;
	if (calibration_pending && audio_install_live_state() &&
		model.ceiling_paula == calibration_paula &&
		model.ceiling_ax == calibration_ax) {
		calibration_pending = FALSE;
		audio_text(last_error, sizeof(last_error),
			"Calibration committed - Save to persist");
		app_models_changed();
	}
	if (audio_metering_available() && (fe_ticks() % 3U) == 0) {
		audio_read_meter(FALSE);
		audio_read_meter(TRUE);
	}
}
