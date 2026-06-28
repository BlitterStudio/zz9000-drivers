/*
 * Shared ZZ9000 firmware-file push (FWUP) protocol client — portable core.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <string.h>

#include "fwup_client.h"

/* Roughly matches SD-boot's poll budget — each FatFs write can stall for
 * tens of ms on a slow SD card, so a small counter wouldn't survive a
 * real card under load. */
#define FWUP_POLL_LIMIT 2000000UL

static int valid_dest_char(char c) {
    return (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') ||
           c == '.' || c == '_' || c == '-';
}

int fwup_name_valid(const char *name) {
    size_t len, i;
    if (!name) return 0;
    len = strlen(name);
    if (len == 0 || len > FWUP_NAME_MAX) return 0;
    for (i = 0; i < len; i++) {
        if (!valid_dest_char(name[i])) return 0;
    }
    return 1;
}

const char *fwup_strerror(uint16_t status) {
    switch (status) {
    case FWUP_OK:            return "OK";
    case FWUP_ERR_NO_SD:     return "no SD card / FAT not mounted on firmware";
    case FWUP_ERR_BAD_NAME:  return "bad filename (flat-root, [A-Za-z0-9._-], <=64 chars)";
    case FWUP_ERR_OPEN:      return "firmware f_open failed";
    case FWUP_ERR_WRITE:     return "firmware f_write failed";
    case FWUP_ERR_CLOSE:     return "firmware f_close/f_sync failed";
    case FWUP_ERR_STATE:     return "protocol state error (WRITE/CLOSE without OPEN?)";
    case FWUP_ERR_LEN:       return "chunk length out of range";
    case FWUP_ERR_UNKNOWN:   return "unknown FWUP command";
    case FWUP_ERR_NO_BACKUP: return "no backup file found to restore";
    case FWUP_ERR_RESTORE:   return "firmware restore (rename) failed";
    case FWUP_ERR_FILE:      return "cannot read the source file";
    case FWUP_ERR_NOMEM:     return "out of memory";
    case FWUP_STATUS_BUSY:   return "timed out waiting for firmware";
    default:                 return "unknown error";
    }
}

/* The firmware sets STATUS = 0xFFFF when it accepts a command and clears
 * it to the result once the FatFs call returns. */
static uint16_t poll_status(struct fwup_io *io) {
    unsigned long budget = FWUP_POLL_LIMIT;
    uint16_t st;
    do {
        st = io->read_status(io);
        if (st != FWUP_STATUS_BUSY) return st;
    } while (--budget);
    return FWUP_STATUS_BUSY;
}

static void stage_name(struct fwup_io *io, const char *name) {
    uint8_t *buf = io->buffer(io);
    size_t n = strlen(name);
    memcpy(buf, name, n);
    buf[n] = '\0';
}

uint16_t fwup_probe(struct fwup_io *io) {
    uint16_t st;

    /* Old firmware leaves these formerly-unused registers reading as OK,
     * so use a non-destructive command that only a FWUP-capable firmware
     * answers with a protocol error. ABORT first also clears any stale
     * interrupted transfer. */
    io->write_cmd(io, FWUP_CMD_ABORT);
    st = poll_status(io);
    if (st == FWUP_STATUS_BUSY) return 0;

    io->write_len(io, 0);
    io->write_cmd(io, FWUP_CMD_WRITE);
    st = poll_status(io);
    return (st == FWUP_ERR_STATE || st == FWUP_ERR_LEN) ? 1 : 0;
}

uint16_t fwup_open(struct fwup_io *io, const char *dest_name) {
    if (!fwup_name_valid(dest_name)) return FWUP_ERR_BAD_NAME;
    stage_name(io, dest_name);
    io->write_cmd(io, FWUP_CMD_OPEN);
    return poll_status(io);
}

uint16_t fwup_write_chunk(struct fwup_io *io, const void *data, uint16_t len) {
    if (len == 0 || len > FWUP_CHUNK_BYTES) return FWUP_ERR_LEN;
    memcpy(io->buffer(io), data, len);
    io->write_len(io, len);
    io->write_cmd(io, FWUP_CMD_WRITE);
    return poll_status(io);
}

uint16_t fwup_close(struct fwup_io *io) {
    io->write_cmd(io, FWUP_CMD_CLOSE);
    return poll_status(io);
}

uint16_t fwup_abort(struct fwup_io *io) {
    io->write_cmd(io, FWUP_CMD_ABORT);
    return poll_status(io);
}

uint16_t fwup_restore(struct fwup_io *io, const char *target) {
    if (!fwup_name_valid(target)) return FWUP_ERR_BAD_NAME;
    stage_name(io, target);
    io->write_cmd(io, FWUP_CMD_RESTORE);
    return poll_status(io);
}
