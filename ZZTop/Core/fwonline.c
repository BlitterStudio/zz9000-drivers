/*
 * ZZTop online firmware update (see fwonline.h).
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/var.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <stdio.h>
#include <string.h>

#include "board.h"
#include "fwonline.h"
#include "https.h"

#define LATEST_URL \
	"https://api.github.com/repos/" FWONLINE_REPO "/releases/latest"
#define DOWNLOAD_URL "https://github.com/" FWONLINE_REPO "/releases/download/"
#define JSON_MAX 65536UL
#define INSTALLED_VAR "ZZTopFirmware"
/* Report download progress at most once per this many bytes. */
#define PROGRESS_STEP 16384UL

/* Every sink context starts with the caller's hooks, so one adapter
 * serves both requests. */
struct sink_base {
	const struct fwonline_hooks *hooks;
};

static int keep_running(void *ctx)
{
	const struct fwonline_hooks *hooks = ((struct sink_base *)ctx)->hooks;

	return !hooks->keep_running || hooks->keep_running(hooks->ctx);
}

/* Describe an HTTPS failure in `msg`; `what` names the request. */
static void describe_failure(enum https_result result, int status,
	const char *err, const char *what, char *msg, ULONG size)
{
	if (result == HTTPS_HTTP_STATUS && status == 403)
		snprintf(msg, size, "GitHub refused the %s (rate limit); try "
			"again later", what);
	else if (result == HTTPS_HTTP_STATUS)
		snprintf(msg, size, "GitHub answered the %s with HTTP %d", what,
			status);
	else
		snprintf(msg, size, "%s", err[0] ? err : "Network error");
}

/* --- Installed version ---------------------------------------------- */

void fwonline_installed(struct fwrel_version *out, BOOL *patch_known)
{
	char tag[32];
	struct fwrel_version recorded;

	out->major = board.fw_version >> 8;
	out->minor = board.fw_version & 0xff;
	out->patch = board.fw_patch;
	*patch_known = board.fw_patch_known;
	if (*patch_known)
		return;
	if (GetVar((STRPTR)INSTALLED_VAR, (STRPTR)tag, sizeof(tag),
			GVF_GLOBAL_ONLY) > 0 &&
	    fwrel_parse_tag(tag, &recorded) &&
	    recorded.major == out->major && recorded.minor == out->minor) {
		out->patch = recorded.patch;
		*patch_known = TRUE;
	}
}

void fwonline_record_install(const char *tag)
{
	if (tag)
		SetVar((STRPTR)INSTALLED_VAR, (STRPTR)tag, -1,
			GVF_GLOBAL_ONLY | GVF_SAVE_VAR);
	else
		DeleteVar((STRPTR)INSTALLED_VAR, GVF_GLOBAL_ONLY | GVF_SAVE_VAR);
}

/* --- Release check -------------------------------------------------- */

struct check_sink {
	struct sink_base base;
	char *data;
	ULONG len;
	BOOL overflow;
};

static int check_data(void *ctx, const UBYTE *data, ULONG len)
{
	struct check_sink *s = ctx;

	if (s->len + len >= JSON_MAX) {
		s->overflow = TRUE;
		return -1;
	}
	memcpy(s->data + s->len, data, len);
	s->len += len;
	return 0;
}

BOOL fwonline_check(int variant, const struct fwonline_hooks *hooks,
	struct fwonline_release *out, BOOL *cancelled, char *msg, ULONG size)
{
	struct check_sink s;
	struct https_sink sink;
	struct fwrel_version installed;
	BOOL patch_known;
	char asset[96];
	char err[160];
	enum https_result result;
	int status;
	BOOL ok = FALSE;

	*cancelled = FALSE;
	memset(out, 0, sizeof(*out));
	s.base.hooks = hooks;
	s.len = 0;
	s.overflow = FALSE;
	s.data = AllocVec(JSON_MAX, MEMF_ANY);
	if (!s.data) {
		snprintf(msg, size, "%s", "Not enough memory");
		return FALSE;
	}
	sink.begin = NULL;
	sink.data = check_data;
	sink.keep_running = keep_running;
	sink.ctx = &s;
	result = https_get(LATEST_URL, "application/vnd.github+json", &sink,
		&status, err, sizeof(err));
	if (result == HTTPS_CANCELLED) {
		*cancelled = TRUE;
		snprintf(msg, size, "%s", "Check cancelled");
	} else if (result == HTTPS_SINK_FAILED && s.overflow) {
		snprintf(msg, size, "%s", "Release information is too large");
	} else if (result == HTTPS_HTTP_STATUS && status == 404) {
		snprintf(msg, size, "%s", "No firmware release is published");
	} else if (result != HTTPS_OK) {
		describe_failure(result, status, err, "release query", msg, size);
	} else if (!fwrel_json_top_string(s.data, s.len, "tag_name",
			out->tag, sizeof(out->tag))) {
		snprintf(msg, size, "%s", "Release information has no tag");
	} else if (!fwrel_parse_tag(out->tag, &out->version)) {
		snprintf(msg, size, "Latest release %s is not a version number",
			out->tag);
	} else {
		fwonline_installed(&installed, &patch_known);
		out->compare = fwrel_compare(&installed, patch_known,
			&out->version);
		out->asset_listed = fwrel_asset_name(out->tag, variant, asset,
				sizeof(asset)) &&
			fwrel_json_has_string(s.data, s.len, asset);
		ok = TRUE;
	}
	FreeVec(s.data);
	return ok;
}

/* --- Download ------------------------------------------------------- */

struct download_sink {
	struct sink_base base;
	struct fwrel_zip *zip;
	BPTR file;
	int zip_result;
	ULONG received;
	ULONG reported;
	LONG total;
};

static int write_image(void *ctx, const uint8_t *data, size_t len)
{
	struct download_sink *s = ctx;

	return Write(s->file, (APTR)data, (LONG)len) != (LONG)len;
}

static int download_begin(void *ctx, LONG total)
{
	struct download_sink *s = ctx;

	s->total = total;
	if (s->base.hooks->progress)
		s->base.hooks->progress(s->base.hooks->ctx, 0, total);
	return 0;
}

static int download_data(void *ctx, const UBYTE *data, ULONG len)
{
	struct download_sink *s = ctx;
	const struct fwonline_hooks *hooks = s->base.hooks;

	s->received += len;
	if (hooks->progress && s->received - s->reported >= PROGRESS_STEP) {
		s->reported = s->received;
		hooks->progress(hooks->ctx, s->received, s->total);
	}
	s->zip_result = fwrel_zip_feed(s->zip, data, len);
	if (s->zip_result == FWREL_ZIP_DONE)
		return HTTPS_SINK_DONE;
	return s->zip_result == FWREL_ZIP_MORE ? 0 : -1;
}

BOOL fwonline_download(const char *tag, int variant, const char *path,
	const struct fwonline_hooks *hooks, BOOL *cancelled, char *msg,
	ULONG size)
{
	static char url[sizeof(DOWNLOAD_URL) + 160];
	struct download_sink s;
	struct https_sink sink;
	char asset[96];
	char err[160];
	enum https_result result;
	int status;
	BOOL ok = FALSE;

	*cancelled = FALSE;
	if (!fwrel_asset_name(tag, variant, asset, sizeof(asset))) {
		snprintf(msg, size, "%s", "No release image for this board");
		return FALSE;
	}
	snprintf(url, sizeof(url), "%s%s/%s", DOWNLOAD_URL, tag, asset);

	memset(&s, 0, sizeof(s));
	s.base.hooks = hooks;
	s.total = -1;
	s.zip_result = FWREL_ZIP_MORE;
	s.zip = fwrel_zip_new(write_image, &s);
	if (!s.zip) {
		snprintf(msg, size, "%s", "Not enough memory");
		return FALSE;
	}
	s.file = Open((CONST_STRPTR)path, MODE_NEWFILE);
	if (!s.file) {
		fwrel_zip_free(s.zip);
		snprintf(msg, size, "Cannot create %s", path);
		return FALSE;
	}
	sink.begin = download_begin;
	sink.data = download_data;
	sink.keep_running = keep_running;
	sink.ctx = &s;
	result = https_get(url, "application/octet-stream", &sink, &status,
		err, sizeof(err));
	Close(s.file);

	if (result == HTTPS_STOPPED && s.zip_result == FWREL_ZIP_DONE) {
		ok = TRUE;
	} else if (result == HTTPS_CANCELLED) {
		*cancelled = TRUE;
		snprintf(msg, size, "%s", "Download cancelled");
	} else if (result == HTTPS_HTTP_STATUS && status == 404) {
		snprintf(msg, size, "%s has no %s image", tag,
			fwrel_variant_name(variant));
	} else if (result == HTTPS_SINK_FAILED || result == HTTPS_OK) {
		/* The archive failed to verify, or ended before BOOT.bin
		 * was complete. */
		snprintf(msg, size, "Downloaded image rejected: %s",
			s.zip_result == FWREL_ZIP_ERR_WRITE ?
			"cannot write it (is the disk full?)" :
			fwrel_zip_strerror(s.zip_result));
	} else {
		describe_failure(result, status, err, "download", msg, size);
	}
	fwrel_zip_free(s.zip);
	if (!ok)
		DeleteFile((CONST_STRPTR)path);
	return ok;
}
