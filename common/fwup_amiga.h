/*
 * Amiga-side bindings for the shared FWUP protocol client.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Binds the portable fwup_client to a ZZ9000 Zorro board window and adds
 * the dos.library file-push convenience used by both ZZFwUpdate and ZZTop.
 */
#ifndef FWUP_AMIGA_H
#define FWUP_AMIGA_H

#include <exec/types.h>
#include "fwup_client.h"

/* Progress during a file push: bytes done so far, and total size
 * (or -1 if it could not be determined). Called once before the first
 * chunk and once after each chunk. May be NULL. */
typedef void (*fwup_progress_fn)(void *ctx, ULONG done, LONG total);

/* A release bundle consists of BOOT.bin and, when present, direct children
 * of its sibling ZZ9K directory. Names are retained for caller previews. */
#define FWUP_BUNDLE_MAX_MODULES 16
struct fwup_bundle {
    char boot_path[256];
    char module_dir[256];
    char module_paths[FWUP_BUNDLE_MAX_MODULES][256];
    char module_names[FWUP_BUNDLE_MAX_MODULES][13];
    UWORD module_count;
    LONG directory_error;
    int module_dir_present;
};

/* Discover BOOT.bin's sibling ZZ9K directory. A missing ZZ9K directory is
 * valid and yields an empty bundle. Other directory errors are recorded in
 * directory_error for the caller to report. */
UWORD fwup_bundle_discover(const char *boot_path, struct fwup_bundle *bundle);

/* Installs modules before BOOT.bin. Firmware without
 * ZZ_FW_CAP_FWUP_MODULE_DIR receives BOOT.bin only; modules_skipped then
 * reports the reboot-and-run-again path. failed_name receives a module
 * filename when a module prevents BOOT.bin from being installed. */
UWORD fwup_bundle_install(ULONG board, const struct fwup_bundle *bundle,
                          UWORD fw_caps, fwup_progress_fn cb, void *ctx,
                          char *failed_name, ULONG failed_name_size,
                          int *modules_skipped);

/* Read the firmware feature bits used to select optional protocol paths. */
UWORD fwup_board_capabilities(ULONG board);

/* Bind an fwup_io to a ZZ9000 board base (ConfigDev cd_BoardAddr). */
void  fwup_io_init_board(struct fwup_io *io, ULONG board);

/* Convenience wrappers over a board base. */
UWORD fwup_probe_board(ULONG board);            /* 1 supported, 0 not */
UWORD fwup_restore_board(ULONG board, const char *target);

/* Read src_path and push it to the card as dest_name, reporting progress
 * via cb. Aborts the partial transfer on any error. Returns an FWUP
 * status (FWUP_OK on success, FWUP_ERR_FILE/NOMEM for client-side faults). */
UWORD fwup_send_file(ULONG board, const char *src_path, const char *dest_name,
                     fwup_progress_fn cb, void *ctx);

#endif /* FWUP_AMIGA_H */
