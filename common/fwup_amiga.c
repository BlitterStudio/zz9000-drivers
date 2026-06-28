/*
 * Amiga-side bindings for the shared FWUP protocol client.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "zz9000_hw.h"
#include "fwup_client.h"
#include "fwup_amiga.h"

/* The FWUP status register sits in the cache-inhibit Zorro register
 * window, so plain volatile access is correct with no cache management. */
static uint16_t board_read_status(struct fwup_io *io) {
    return *(volatile UWORD *)((ULONG)io->ctx + ZZ_REG_FWUP_STATUS);
}
static void board_write_cmd(struct fwup_io *io, uint16_t cmd) {
    *(volatile UWORD *)((ULONG)io->ctx + ZZ_REG_FWUP_CMD) = (UWORD)cmd;
}
static void board_write_len(struct fwup_io *io, uint16_t len) {
    *(volatile UWORD *)((ULONG)io->ctx + ZZ_REG_FWUP_LEN) = (UWORD)len;
}
static uint8_t *board_buffer(struct fwup_io *io) {
    return (uint8_t *)((ULONG)io->ctx + ZZ_BUFFER_OFFSET);
}

void fwup_io_init_board(struct fwup_io *io, ULONG board) {
    io->read_status = board_read_status;
    io->write_cmd   = board_write_cmd;
    io->write_len   = board_write_len;
    io->buffer      = board_buffer;
    io->ctx         = (void *)board;
}

UWORD fwup_probe_board(ULONG board) {
    struct fwup_io io;
    fwup_io_init_board(&io, board);
    return (UWORD)fwup_probe(&io);
}

UWORD fwup_restore_board(ULONG board, const char *target) {
    struct fwup_io io;
    fwup_io_init_board(&io, board);
    return (UWORD)fwup_restore(&io, target);
}

UWORD fwup_send_file(ULONG board, const char *src_path, const char *dest_name,
                     fwup_progress_fn cb, void *ctx) {
    struct fwup_io io;
    BPTR  fh;
    UBYTE *chunk;
    LONG  file_size = -1;
    ULONG total = 0;
    LONG  n;
    UWORD st;

    if (!fwup_name_valid(dest_name)) return FWUP_ERR_BAD_NAME;

    fwup_io_init_board(&io, board);

    fh = Open((STRPTR)src_path, MODE_OLDFILE);
    if (!fh) return FWUP_ERR_FILE;

    /* Size up-front for a progress estimate; not required by the protocol. */
    if (Seek(fh, 0, OFFSET_END) >= 0) file_size = Seek(fh, 0, OFFSET_BEGINNING);

    chunk = (UBYTE *)AllocMem(FWUP_CHUNK_BYTES, MEMF_PUBLIC);
    if (!chunk) { Close(fh); return FWUP_ERR_NOMEM; }

    st = (UWORD)fwup_open(&io, dest_name);
    if (st != FWUP_OK) {
        FreeMem(chunk, FWUP_CHUNK_BYTES);
        Close(fh);
        return st;
    }

    if (cb) cb(ctx, 0, file_size);

    while ((n = Read(fh, chunk, FWUP_CHUNK_BYTES)) > 0) {
        st = (UWORD)fwup_write_chunk(&io, chunk, (uint16_t)n);
        if (st != FWUP_OK) {
            fwup_abort(&io);
            FreeMem(chunk, FWUP_CHUNK_BYTES);
            Close(fh);
            return st;
        }
        total += (ULONG)n;
        if (cb) cb(ctx, total, file_size);
    }

    if (n < 0) {
        fwup_abort(&io);
        FreeMem(chunk, FWUP_CHUNK_BYTES);
        Close(fh);
        return FWUP_ERR_FILE;
    }

    st = (UWORD)fwup_close(&io);
    if (st != FWUP_OK) fwup_abort(&io);

    FreeMem(chunk, FWUP_CHUNK_BYTES);
    Close(fh);
    return st;
}
