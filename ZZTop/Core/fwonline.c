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
	char directory[256];
	int zip_result;
	ULONG received;
	ULONG reported;
	LONG total;
};

static BOOL bundle_directory(const char *boot_path, char *directory,
	ULONG directory_size)
{
	const char *last;
	ULONG len;

	if (!boot_path)
		return FALSE;
	last = strrchr(boot_path, '/');
	if (!last)
		return FALSE;
	len = (ULONG)(last - boot_path);
	if (len == 0 || len >= directory_size)
		return FALSE;
	memcpy(directory, boot_path, len);
	directory[len] = '\0';
	return TRUE;
}

/* Remove only files inside a private download drawer whose BOOT.bin path we
 * created. This leaves no online payload behind after any outcome. */
void fwonline_remove_bundle(const char *boot_path)
{
	char directory[256];
	char module_directory[256];
	char member[256];
	BPTR lock;
	struct FileInfoBlock *fib;

	if (!bundle_directory(boot_path, directory, sizeof(directory)))
		return;
	snprintf(module_directory, sizeof(module_directory), "%s/ZZ9K", directory);
	lock = Lock((CONST_STRPTR)module_directory, ACCESS_READ);
	if (lock) {
		fib = AllocVec(sizeof(*fib), MEMF_ANY);
		if (fib && Examine(lock, fib)) {
			while (ExNext(lock, fib)) {
				if (fib->fib_DirEntryType >= 0)
					continue;
				snprintf(member, sizeof(member), "%s/%s", module_directory,
					fib->fib_FileName);
				DeleteFile((CONST_STRPTR)member);
			}
		}
		if (fib)
			FreeVec(fib);
		UnLock(lock);
	}
	DeleteFile((CONST_STRPTR)module_directory);
	DeleteFile((CONST_STRPTR)boot_path);
	DeleteFile((CONST_STRPTR)directory);
}

static BOOL make_bundle_directory(char *boot_path, ULONG boot_path_size,
	char *directory, ULONG directory_size)
{
	struct DateStamp stamp;
	BPTR created = 0;
	ULONG attempt;

	DateStamp(&stamp);
	for (attempt = 0; attempt < 16; attempt++) {
		snprintf(directory, directory_size,
			"T:ZZTop-fw-%08lx-%08lx-%08lx-%02lx",
			(unsigned long)stamp.ds_Days, (unsigned long)stamp.ds_Minute,
			(unsigned long)stamp.ds_Tick, (unsigned long)attempt);
		if (strlen(directory) >= directory_size ||
			strlen(directory) + sizeof("/BOOT.bin") > boot_path_size)
			return FALSE;
		created = CreateDir((CONST_STRPTR)directory);
		if (created)
			break;
	}
	if (!created)
		return FALSE;
	UnLock(created);
	snprintf(boot_path, boot_path_size, "%s/BOOT.bin", directory);
	return TRUE;
}

static int begin_bundle_entry(void *ctx, const char *path)
{
	struct download_sink *s = ctx;
	char target[256];
	char module_directory[256];
	BPTR lock;

	if (s->file ||
		snprintf(target, sizeof(target), "%s/%s", s->directory, path) >=
			(int)sizeof(target))
		return -1;
	if (strcmp(path, "BOOT.bin") != 0) {
		if (snprintf(module_directory, sizeof(module_directory), "%s/ZZ9K",
			s->directory) >= (int)sizeof(module_directory))
			return -1;
		lock = Lock((CONST_STRPTR)module_directory, ACCESS_READ);
		if (lock)
			UnLock(lock);
		else {
			lock = CreateDir((CONST_STRPTR)module_directory);
			if (!lock)
				return -1;
			UnLock(lock);
		}
	}
	s->file = Open((CONST_STRPTR)target, MODE_NEWFILE);
	return s->file ? 0 : -1;
}

static int write_bundle_entry(void *ctx, const uint8_t *data, size_t len)
{
	struct download_sink *s = ctx;

	return !s->file || Write(s->file, (APTR)data, (LONG)len) != (LONG)len;
}

static int end_bundle_entry(void *ctx, const char *path)
{
	struct download_sink *s = ctx;

	(void)path;
	if (!s->file)
		return -1;
	Close(s->file);
	s->file = 0;
	return 0;
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

BOOL fwonline_download(const char *tag, int variant, char *boot_path,
	ULONG boot_path_size, const struct fwonline_hooks *hooks,
	BOOL *cancelled, char *msg, ULONG size)
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
	if (!boot_path || boot_path_size == 0 ||
		!fwrel_asset_name(tag, variant, asset, sizeof(asset))) {
		snprintf(msg, size, "%s", "No release image for this board");
		return FALSE;
	}
	boot_path[0] = '\0';
	snprintf(url, sizeof(url), "%s%s/%s", DOWNLOAD_URL, tag, asset);

	memset(&s, 0, sizeof(s));
	s.base.hooks = hooks;
	s.total = -1;
	s.zip_result = FWREL_ZIP_MORE;
	if (!make_bundle_directory(boot_path, boot_path_size, s.directory,
		sizeof(s.directory))) {
		snprintf(msg, size, "%s", "Cannot create temporary directory in T:");
		return FALSE;
	}
	s.zip = fwrel_zip_new_bundle(begin_bundle_entry, write_bundle_entry,
		end_bundle_entry, &s);
	if (!s.zip) {
		snprintf(msg, size, "%s", "Not enough memory");
		fwonline_remove_bundle(boot_path);
		boot_path[0] = '\0';
		return FALSE;
	}
	sink.begin = download_begin;
	sink.data = download_data;
	sink.keep_running = keep_running;
	sink.ctx = &s;
	result = https_get(url, "application/octet-stream", &sink, &status,
		err, sizeof(err));
	if (s.file)
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
		snprintf(msg, size, "Downloaded bundle rejected: %s",
			s.zip_result == FWREL_ZIP_ERR_WRITE ?
			"cannot write it (is the disk full?)" :
			fwrel_zip_strerror(s.zip_result));
	} else {
		describe_failure(result, status, err, "download", msg, size);
	}
	fwrel_zip_free(s.zip);
	if (!ok) {
		fwonline_remove_bundle(boot_path);
		boot_path[0] = '\0';
	}
	return ok;
}
