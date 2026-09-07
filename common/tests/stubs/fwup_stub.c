/*
 * In-memory board transport for configuration save tests. The real FWUP
 * client stages commands here; only CLOSE publishes the saved file.
 *
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <string.h>
#include <exec/types.h>
#include "fwup_client.h"
#include "fwup_amiga.h"

char fwup_test_saved_text[FWUP_CHUNK_BYTES];
uint16_t fwup_test_saved_len;
static uint8_t staging[FWUP_CHUNK_BYTES];
static char pending[FWUP_CHUNK_BYTES];
static uint16_t pending_len, chunk_len, status;
static int opened;

static uint16_t read_status(struct fwup_io *io)
{ (void)io; return status; }

static uint8_t *buffer(struct fwup_io *io)
{ (void)io; return staging; }

static void write_len(struct fwup_io *io, uint16_t len)
{ (void)io; chunk_len = len; }

static void write_cmd(struct fwup_io *io, uint16_t cmd)
{
    (void)io;
    status = FWUP_OK;
    switch (cmd) {
    case FWUP_CMD_OPEN:
        opened = 1;
        pending_len = 0;
        break;
    case FWUP_CMD_WRITE:
        if (!opened) { status = FWUP_ERR_STATE; break; }
        if (chunk_len == 0 || pending_len + chunk_len >= sizeof(pending)) {
            status = FWUP_ERR_LEN;
            break;
        }
        memcpy(pending + pending_len, staging, chunk_len);
        pending_len += chunk_len;
        break;
    case FWUP_CMD_CLOSE:
        if (!opened) { status = FWUP_ERR_STATE; break; }
        memcpy(fwup_test_saved_text, pending, pending_len);
        fwup_test_saved_text[pending_len] = '\0';
        fwup_test_saved_len = pending_len;
        opened = 0;
        break;
    case FWUP_CMD_ABORT:
        opened = 0;
        break;
    default:
        status = FWUP_ERR_UNKNOWN;
        break;
    }
}

void fwup_io_init_board(struct fwup_io *io, ULONG board)
{
    (void)board;
    io->read_status = read_status;
    io->write_cmd = write_cmd;
    io->write_len = write_len;
    io->buffer = buffer;
    io->ctx = NULL;
}
