/*
 * ZZTop online firmware update: find the latest GitHub release, compare
 * it with the running firmware and download its BOOT.bin for this board.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Installation itself stays with the FWUP path the Firmware page already
 * uses for a chosen file; this module only produces a verified image.
 */
#ifndef ZZTOP_FWONLINE_H
#define ZZTOP_FWONLINE_H

#include <exec/types.h>

#include "fw_release.h"

#define FWONLINE_REPO "BlitterStudio/zz9000-firmware"

/* Caller services for long network operations. */
struct fwonline_hooks {
	int (*keep_running)(void *ctx);                 /* 0 cancels */
	void (*progress)(void *ctx, ULONG done, LONG total);
	void *ctx;
};

struct fwonline_release {
	char tag[32];                 /* e.g. "v2.8.1" */
	struct fwrel_version version;
	enum fwrel_compare compare;   /* against the running firmware */
	BOOL asset_listed;            /* release has the chosen variant */
};

/* Query the latest (non-prerelease) release. FALSE with `msg` set when
 * the query fails or was cancelled (*cancelled set). */
BOOL fwonline_check(int variant, const struct fwonline_hooks *hooks,
	struct fwonline_release *out, BOOL *cancelled, char *msg, ULONG size);

/* Download one release ZIP, extract its verified bundle into a fresh bounded
 * T: drawer, and return the full path of its BOOT.bin in `boot_path`.
 * A failed or cancelled download removes the drawer. The caller must call
 * fwonline_remove_bundle after installing or declining the downloaded bundle. */
BOOL fwonline_download(const char *tag, int variant, char *boot_path,
	ULONG boot_path_size, const struct fwonline_hooks *hooks,
	BOOL *cancelled, char *msg, ULONG size);

/* Remove the private T: drawer created by fwonline_download. */
void fwonline_remove_bundle(const char *boot_path);

/* Installed version as ZZTop knows it: major.minor from the card, the
 * patch from the card when it reports one, otherwise from the record of
 * the last release ZZTop installed for this major.minor. */
void fwonline_installed(struct fwrel_version *out, BOOL *patch_known);

/* Remember the release ZZTop just installed (ENV: and ENVARC:), or forget
 * it after a manual install or restore (tag NULL). */
void fwonline_record_install(const char *tag);

#endif /* ZZTOP_FWONLINE_H */
