/*
 * Shared ZZ9000 firmware-file push (FWUP) protocol client.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Portable core of the FWUP register protocol, shared by ZZFwUpdate (CLI)
 * and ZZTop (GUI). It talks to the board only through an injected
 * `struct fwup_io` seam, so the protocol logic is unit-testable on the
 * host against a simulated firmware. The Amiga-side board IO and file
 * push live in fwup_amiga.c.
 */
#ifndef FWUP_CLIENT_H
#define FWUP_CLIENT_H

#include <stddef.h>
#include <stdint.h>

/* Protocol commands (mirror firmware fw_update.h). */
#define FWUP_CMD_OPEN      1
#define FWUP_CMD_WRITE     2
#define FWUP_CMD_CLOSE     3
#define FWUP_CMD_ABORT     4
#define FWUP_CMD_RESTORE   5

/* Status codes. 0xFFFF means "still in flight"; the firmware-reported
 * results are 0x00..0x0B; 0x00F* are client-side (never seen on the wire). */
#define FWUP_STATUS_BUSY   0xFFFFu
#define FWUP_OK            0x0000u
#define FWUP_ERR_NO_SD     0x0002u
#define FWUP_ERR_BAD_NAME  0x0003u
#define FWUP_ERR_OPEN      0x0004u
#define FWUP_ERR_WRITE     0x0005u
#define FWUP_ERR_CLOSE     0x0006u
#define FWUP_ERR_STATE     0x0007u
#define FWUP_ERR_LEN       0x0008u
#define FWUP_ERR_UNKNOWN   0x0009u
#define FWUP_ERR_NO_BACKUP 0x000Au
#define FWUP_ERR_RESTORE   0x000Bu
#define FWUP_ERR_FILE      0x00FEu /* client: cannot read the source file */
#define FWUP_ERR_NOMEM     0x00FDu /* client: out of memory */

/* Largest chunk staged per WRITE. Stays under the firmware's 24576-byte
 * shared-buffer window and matches the SD-boot residency story. */
#define FWUP_CHUNK_BYTES   16384

/* Max destination filename length the firmware accepts (flat root). */
#define FWUP_NAME_MAX      64

/* Register/buffer access seam. On Amiga these do volatile MMIO at the
 * Zorro board window (fwup_amiga.c); host tests point them at a
 * simulated firmware. `read_status` returns FWUP_STATUS_BUSY until the
 * firmware finishes the last command. */
struct fwup_io {
    uint16_t (*read_status)(struct fwup_io *io);
    void     (*write_cmd)(struct fwup_io *io, uint16_t cmd);
    void     (*write_len)(struct fwup_io *io, uint16_t len);
    uint8_t *(*buffer)(struct fwup_io *io);
    void     *ctx;
};

/* Pure helpers. */
int         fwup_name_valid(const char *name); /* 1 if [A-Za-z0-9._-]{1,64} */
const char *fwup_strerror(uint16_t status);

/* Protocol primitives: issue the command and poll until the firmware
 * reports a result (or FWUP_STATUS_BUSY on timeout). */
uint16_t fwup_probe(struct fwup_io *io);        /* 1 supported, 0 not */
uint16_t fwup_open(struct fwup_io *io, const char *dest_name);
uint16_t fwup_write_chunk(struct fwup_io *io, const void *data, uint16_t len);
uint16_t fwup_close(struct fwup_io *io);
uint16_t fwup_abort(struct fwup_io *io);
uint16_t fwup_restore(struct fwup_io *io, const char *target);

#endif /* FWUP_CLIENT_H */
