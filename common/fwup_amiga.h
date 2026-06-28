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
