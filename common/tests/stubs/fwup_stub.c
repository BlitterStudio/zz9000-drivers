/*
 * Stub FWUP transport for host tests.
 *
 * zzcfg_save() writes the rendered file to the board, which the host tests
 * do not exercise - they cover the parse/generate model. The real headers
 * are used (a quoted include from common/ finds them first regardless), so
 * only the definitions are stubbed out here.
 *
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <exec/types.h>

#include "fwup_client.h"
#include "fwup_amiga.h"

void fwup_io_init_board(struct fwup_io *io, ULONG board)
{ (void)io; (void)board; }

uint16_t fwup_open(struct fwup_io *io, const char *dest_name)
{ (void)io; (void)dest_name; return FWUP_OK; }

uint16_t fwup_write_chunk(struct fwup_io *io, const void *data, uint16_t len)
{ (void)io; (void)data; (void)len; return FWUP_OK; }

uint16_t fwup_close(struct fwup_io *io)
{ (void)io; return FWUP_OK; }

uint16_t fwup_abort(struct fwup_io *io)
{ (void)io; return FWUP_OK; }
