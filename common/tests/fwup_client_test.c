/*
 * Host unit tests for the shared FWUP protocol client.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Drives the portable client against a simulated firmware that reacts to
 * each command write (consuming the staging buffer, accumulating the
 * file, and reporting a status) exactly as the real firmware does over
 * the Zorro register window.
 */
#include <stdio.h>
#include <string.h>

#include "fwup_client.h"

/* ---- simulated firmware behind the fwup_io seam ---- */

#define SIM_FILE_MAX (1u << 20)

static int           sim_dumb;        /* old firmware: every status reads OK */
static uint16_t      sim_status;
static int           sim_open;
static char          sim_name[128];
static unsigned char sim_file[SIM_FILE_MAX];
static size_t        sim_file_len;
static uint16_t      sim_pending_len;
static int           sim_have_backup;
static int           sim_fail_write;  /* next WRITE returns FWUP_ERR_WRITE */
static char          sim_restore_target[128];
static uint8_t       sim_buffer[FWUP_CHUNK_BYTES + 256];

static void sim_reset(void) {
    sim_dumb = 0;
    sim_status = FWUP_OK;
    sim_open = 0;
    sim_name[0] = '\0';
    sim_file_len = 0;
    sim_pending_len = 0;
    sim_have_backup = 0;
    sim_fail_write = 0;
    sim_restore_target[0] = '\0';
    memset(sim_buffer, 0, sizeof(sim_buffer));
}

static uint16_t sim_read_status(struct fwup_io *io) { (void)io; return sim_status; }
static void     sim_write_len(struct fwup_io *io, uint16_t len) { (void)io; sim_pending_len = len; }
static uint8_t *sim_buffer_ptr(struct fwup_io *io) { (void)io; return sim_buffer; }

static void sim_write_cmd(struct fwup_io *io, uint16_t cmd) {
    (void)io;
    if (sim_dumb) { sim_status = FWUP_OK; return; }

    switch (cmd) {
    case FWUP_CMD_OPEN:
        if (!fwup_name_valid((const char *)sim_buffer)) { sim_status = FWUP_ERR_BAD_NAME; break; }
        strcpy(sim_name, (const char *)sim_buffer);
        sim_open = 1;
        sim_file_len = 0;
        sim_status = FWUP_OK;
        break;
    case FWUP_CMD_WRITE:
        if (!sim_open) { sim_status = FWUP_ERR_STATE; break; }
        if (sim_pending_len == 0) { sim_status = FWUP_ERR_LEN; break; }
        if (sim_fail_write) { sim_status = FWUP_ERR_WRITE; break; }
        memcpy(sim_file + sim_file_len, sim_buffer, sim_pending_len);
        sim_file_len += sim_pending_len;
        sim_status = FWUP_OK;
        break;
    case FWUP_CMD_CLOSE:
        sim_status = sim_open ? FWUP_OK : FWUP_ERR_STATE;
        sim_open = 0;
        break;
    case FWUP_CMD_ABORT:
        sim_open = 0;
        sim_file_len = 0;
        sim_status = FWUP_OK;
        break;
    case FWUP_CMD_RESTORE:
        if (sim_open) { sim_status = FWUP_ERR_STATE; break; }
        strcpy(sim_restore_target, (const char *)sim_buffer);
        sim_status = sim_have_backup ? FWUP_OK : FWUP_ERR_NO_BACKUP;
        break;
    default:
        sim_status = FWUP_ERR_UNKNOWN;
        break;
    }
}

static struct fwup_io sim_io = {
    sim_read_status, sim_write_cmd, sim_write_len, sim_buffer_ptr, NULL
};

/* ---- tiny test harness ---- */

static int g_checks = 0;
static int g_fails = 0;
#define CHECK(cond, msg)                                                  \
    do {                                                                  \
        g_checks++;                                                       \
        if (!(cond)) { g_fails++; printf("  FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); } \
    } while (0)

static void test_name_valid(void) {
    printf("test_name_valid\n");
    CHECK(fwup_name_valid("BOOT.bin"), "BOOT.bin accepted");
    CHECK(fwup_name_valid("a_b-c.123"), "[A-Za-z0-9._-] accepted");
    CHECK(!fwup_name_valid(""), "empty rejected");
    CHECK(!fwup_name_valid("has space.bin"), "space rejected");
    CHECK(!fwup_name_valid("sub/dir.bin"), "slash rejected");
    char toolong[FWUP_NAME_MAX + 2];
    memset(toolong, 'a', sizeof(toolong) - 1);
    toolong[sizeof(toolong) - 1] = '\0';
    CHECK(!fwup_name_valid(toolong), "over 64 chars rejected");
}

static void test_strerror(void) {
    printf("test_strerror\n");
    CHECK(strcmp(fwup_strerror(FWUP_OK), "OK") == 0, "OK string");
    CHECK(fwup_strerror(FWUP_ERR_NO_BACKUP)[0] != '\0', "NO_BACKUP has a string");
    CHECK(fwup_strerror(FWUP_ERR_RESTORE)[0] != '\0', "RESTORE has a string");
    CHECK(fwup_strerror(0x1234)[0] != '\0', "unknown code has a string");
}

static void test_probe_supported(void) {
    printf("test_probe_supported\n");
    sim_reset();
    CHECK(fwup_probe(&sim_io) == 1, "FWUP-capable firmware probes supported");
}

static void test_probe_unsupported(void) {
    printf("test_probe_unsupported\n");
    sim_reset();
    sim_dumb = 1; /* old firmware: regs always read OK */
    CHECK(fwup_probe(&sim_io) == 0, "old firmware probes unsupported");
}

static void test_full_send(void) {
    printf("test_full_send\n");
    sim_reset();
    static const char payload[] = "the quick brown fox jumps over the lazy dog";
    CHECK(fwup_open(&sim_io, "BOOT.bin") == FWUP_OK, "open OK");
    CHECK(strcmp(sim_name, "BOOT.bin") == 0, "firmware received the dest name");
    /* two chunks */
    CHECK(fwup_write_chunk(&sim_io, payload, 20) == FWUP_OK, "write chunk 1 OK");
    CHECK(fwup_write_chunk(&sim_io, payload + 20, (uint16_t)(sizeof(payload) - 1 - 20)) == FWUP_OK,
          "write chunk 2 OK");
    CHECK(fwup_close(&sim_io) == FWUP_OK, "close OK");
    CHECK(sim_file_len == sizeof(payload) - 1, "all bytes delivered");
    CHECK(memcmp(sim_file, payload, sizeof(payload) - 1) == 0, "delivered bytes match source");
}

static void test_write_error(void) {
    printf("test_write_error\n");
    sim_reset();
    CHECK(fwup_open(&sim_io, "BOOT.bin") == FWUP_OK, "open OK");
    sim_fail_write = 1;
    CHECK(fwup_write_chunk(&sim_io, "xxxx", 4) == FWUP_ERR_WRITE, "write surfaces firmware error");
    CHECK(fwup_abort(&sim_io) == FWUP_OK, "abort OK after error");
    CHECK(sim_open == 0, "firmware transfer closed by abort");
}

static void test_restore(void) {
    printf("test_restore\n");
    sim_reset();
    sim_have_backup = 1;
    CHECK(fwup_restore(&sim_io, "BOOT.bin") == FWUP_OK, "restore OK when backup present");
    CHECK(strcmp(sim_restore_target, "BOOT.bin") == 0, "firmware received the restore target");

    sim_reset();
    sim_have_backup = 0;
    CHECK(fwup_restore(&sim_io, "BOOT.bin") == FWUP_ERR_NO_BACKUP, "restore reports missing backup");
}

int main(void) {
    test_name_valid();
    test_strerror();
    test_probe_supported();
    test_probe_unsupported();
    test_full_send();
    test_write_error();
    test_restore();
    printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
