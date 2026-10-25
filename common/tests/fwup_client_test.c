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
static char          sim_open_names[16][128];
static unsigned      sim_open_count;
static unsigned char sim_file[SIM_FILE_MAX];
static size_t        sim_file_len;
static uint16_t      sim_pending_len;
static int           sim_have_backup;
static int           sim_fail_write;  /* next WRITE returns FWUP_ERR_WRITE */
static char          sim_restore_target[128];
static uint32_t      sim_mirror;      /* status mirror longword */
static uint8_t       sim_mirror_seq;
static int           sim_mirror_frozen;  /* mirror never advances */
static unsigned      sim_mirror_delay;   /* mirror reads before it advances */
static unsigned      sim_mirror_reads;
static unsigned      sim_register_reads;
static uint16_t      sim_mirror_pending; /* status not yet published */
static int           sim_mirror_has_pending;
static uint8_t       sim_buffer[FWUP_CHUNK_BYTES + 256];

static void sim_reset(void) {
    sim_dumb = 0;
    sim_status = FWUP_OK;
    sim_open = 0;
    sim_name[0] = '\0';
    sim_open_count = 0;
    sim_file_len = 0;
    sim_pending_len = 0;
    sim_have_backup = 0;
    sim_fail_write = 0;
    sim_restore_target[0] = '\0';
    sim_mirror = 0x12345678u; /* whatever the buffer held before */
    sim_mirror_seq = 0;
    sim_mirror_frozen = 0;
    sim_mirror_delay = 0;
    sim_mirror_reads = 0;
    sim_register_reads = 0;
    sim_mirror_has_pending = 0;
    memset(sim_buffer, 0, sizeof(sim_buffer));
}

static uint16_t sim_read_status(struct fwup_io *io) {
    (void)io;
    sim_register_reads++;
    return sim_status;
}

static void sim_publish(uint16_t status) {
    sim_mirror_seq++;
    sim_mirror = ((uint32_t)FWUP_MIRROR_MAGIC << 24) |
                 ((uint32_t)sim_mirror_seq << 16) | status;
}

/* The mirror advances once the command "finishes": after
 * sim_mirror_delay reads, or never when frozen. */
static uint32_t sim_read_mirror(struct fwup_io *io) {
    (void)io;
    sim_mirror_reads++;
    if (sim_mirror_has_pending && !sim_mirror_frozen) {
        if (sim_mirror_delay == 0) {
            sim_publish(sim_mirror_pending);
            sim_mirror_has_pending = 0;
        } else {
            sim_mirror_delay--;
        }
    }
    return sim_mirror;
}
static void     sim_write_len(struct fwup_io *io, uint16_t len) { (void)io; sim_pending_len = len; }
static uint8_t *sim_buffer_ptr(struct fwup_io *io) { (void)io; return sim_buffer; }

static void sim_write_cmd(struct fwup_io *io, uint16_t cmd) {
    (void)io;
    if (sim_dumb) { sim_status = FWUP_OK; return; }

    switch (cmd) {
    case FWUP_CMD_OPEN:
        if (!fwup_name_valid((const char *)sim_buffer)) { sim_status = FWUP_ERR_BAD_NAME; break; }
        strcpy(sim_name, (const char *)sim_buffer);
        if (sim_open_count < 16) {
            strcpy(sim_open_names[sim_open_count], sim_name);
            sim_open_count++;
        }
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
    sim_mirror_pending = sim_status;
    sim_mirror_has_pending = 1;
}

static struct fwup_io sim_io = {
    sim_read_status, sim_write_cmd, sim_write_len, sim_buffer_ptr, NULL,
    NULL, NULL
};

static struct fwup_io sim_mirror_io = {
    sim_read_status, sim_write_cmd, sim_write_len, sim_buffer_ptr, NULL,
    sim_read_mirror, NULL
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
    char toolong[FWUP_NAME_MAX + 2];

    printf("test_name_valid\n");
    CHECK(fwup_name_valid("BOOT.bin"), "legacy root target accepted");
    CHECK(fwup_name_valid("a_b-c.123"), "legacy root charset accepted");
    CHECK(fwup_name_valid("ZZ9K/AUDIO_2.ZZM"), "module target accepted");
    CHECK(fwup_module_name_valid("A.ZZM"), "short module name accepted");
    CHECK(fwup_module_name_valid("ABCDEFGH.ZZM"), "eight-char module accepted");
    CHECK(!fwup_name_valid("ZZ9K/audio.ZZM"), "lowercase module rejected");
    CHECK(!fwup_name_valid("OTHER/A.ZZM"), "other subdirectory rejected");
    CHECK(!fwup_name_valid("ZZ9K/A.BAK"), "module backup target rejected");
    CHECK(!fwup_module_name_valid("ABCDEFGHI.ZZM"), "long module name rejected");
    CHECK(!fwup_name_valid(""), "empty rejected");
    CHECK(!fwup_name_valid("has space.bin"), "space rejected");
    CHECK(!fwup_name_valid("sub/dir.bin"), "slash rejected");
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

/* ---- bundle host-file seam ---- */

struct test_source {
    const char *path;
    const char *data;
};

struct test_reader {
    const struct test_source *source;
    size_t offset;
};

static const struct test_source bundle_sources[] = {
    { "release/BOOT.bin", "boot" },
    { "release/ZZ9K/BETA.ZZM", "beta" },
    { "release/ZZ9K/ALPHA.ZZM", "alpha" }
};
static const char *bundle_entries[] = {
    "BETA.ZZM", "ZZ9000.CFG", "ALPHA.ZZM"
};
static struct test_reader bundle_reader;
static unsigned bundle_list_calls;
static uint32_t bundle_progress_done;
static uint32_t bundle_progress_total;

static const struct test_source *bundle_source(const char *path) {
    size_t i;

    for (i = 0; i < sizeof(bundle_sources) / sizeof(bundle_sources[0]); i++) {
        if (strcmp(bundle_sources[i].path, path) == 0) return &bundle_sources[i];
    }
    return NULL;
}

static int bundle_visit_dir(void *ctx, const char *path,
                            fwup_bundle_visit_fn visit, void *visit_ctx) {
    size_t i;

    (void)ctx;
    bundle_list_calls++;
    if (strcmp(path, "release/ZZ9K/") != 0) return -1;
    for (i = 0; i < sizeof(bundle_entries) / sizeof(bundle_entries[0]); i++) {
        const char *name = bundle_entries[i];
        const struct test_source *source;
        char source_path[64];

        snprintf(source_path, sizeof(source_path), "release/ZZ9K/%s", name);
        source = bundle_source(source_path);
        if (visit(visit_ctx, name,
                  source ? (uint32_t)strlen(source->data) : 0) != 0)
            return -1;
    }
    return 1;
}

static int bundle_file_size(void *ctx, const char *path, uint32_t *size) {
    const struct test_source *source;

    (void)ctx;
    source = bundle_source(path);
    if (!source) return -1;
    *size = (uint32_t)strlen(source->data);
    return 0;
}

static int bundle_open(void *ctx, const char *path, void **file) {
    const struct test_source *source;

    (void)ctx;
    source = bundle_source(path);
    if (!source) return -1;
    bundle_reader.source = source;
    bundle_reader.offset = 0;
    *file = &bundle_reader;
    return 0;
}

static int bundle_read(void *ctx, void *file, uint8_t *buffer,
                       uint16_t capacity, uint16_t *used) {
    struct test_reader *reader = (struct test_reader *)file;
    size_t remaining;

    (void)ctx;
    remaining = strlen(reader->source->data) - reader->offset;
    if (remaining > capacity) remaining = capacity;
    memcpy(buffer, reader->source->data + reader->offset, remaining);
    reader->offset += remaining;
    *used = (uint16_t)remaining;
    return 0;
}

static void bundle_close(void *ctx, void *file) {
    (void)ctx;
    (void)file;
}

static void bundle_progress(void *ctx, const char *target, uint32_t done,
                            uint32_t total) {
    (void)ctx;
    (void)target;
    bundle_progress_done = done;
    bundle_progress_total = total;
}

static const struct fwup_bundle_files bundle_files = {
    bundle_visit_dir, bundle_file_size, bundle_open, bundle_read, bundle_close,
    NULL
};

static void test_bundle_order_and_cfg_exclusion(void) {
    struct fwup_bundle_result result;

    printf("test_bundle_order_and_cfg_exclusion\n");
    sim_reset();
    bundle_list_calls = 0;
    bundle_progress_done = 0;
    bundle_progress_total = 0;
    CHECK(fwup_install_bundle(&sim_io, 1U << 14, "release/BOOT.bin",
                              &bundle_files, bundle_progress, NULL, &result) ==
          FWUP_OK, "bundle install succeeds");
    CHECK(bundle_list_calls == 1, "module directory discovered once");
    CHECK(sim_open_count == 3, "two modules and BOOT opened");
    CHECK(strcmp(sim_open_names[0], "ZZ9K/ALPHA.ZZM") == 0,
          "modules install in name order");
    CHECK(strcmp(sim_open_names[1], "ZZ9K/BETA.ZZM") == 0,
          "second module follows first");
    CHECK(strcmp(sim_open_names[2], "BOOT.bin") == 0, "BOOT installs last");
    CHECK(bundle_progress_done == 13 && bundle_progress_total == 13,
          "progress aggregates module and boot bytes");
    CHECK(result.outcome == FWUP_BUNDLE_INSTALLED, "module bundle outcome");
}

static void test_bundle_module_failure_stops_boot(void) {
    struct fwup_bundle_result result;

    printf("test_bundle_module_failure_stops_boot\n");
    sim_reset();
    sim_fail_write = 1;
    CHECK(fwup_install_bundle(&sim_io, 1U << 14, "release/BOOT.bin",
                              &bundle_files, NULL, NULL, &result) ==
          FWUP_ERR_WRITE, "module failure returned");
    CHECK(result.outcome == FWUP_BUNDLE_MODULE_FAILED,
          "module failure outcome reported");
    CHECK(strcmp(result.failed_module, "ALPHA.ZZM") == 0,
          "failed module identity reported");
    CHECK(sim_open_count == 1 && strcmp(sim_open_names[0], "ZZ9K/ALPHA.ZZM") == 0,
          "BOOT not opened after module failure");
}

static void test_bundle_without_module_capability(void) {
    struct fwup_bundle_result result;

    printf("test_bundle_without_module_capability\n");
    sim_reset();
    bundle_list_calls = 0;
    CHECK(fwup_install_bundle(&sim_io, 0, "release/BOOT.bin", &bundle_files,
                              NULL, NULL, &result) == FWUP_OK,
          "boot-only fallback succeeds");
    CHECK(bundle_list_calls == 0, "fallback does not inspect module directory");
    CHECK(sim_open_count == 1 && strcmp(sim_open_names[0], "BOOT.bin") == 0,
          "fallback sends BOOT only");
    CHECK(result.outcome == FWUP_BUNDLE_BOOT_ONLY_NO_MODULE_DIR,
          "fallback outcome exposed");
}

/* With a mirror the client learns every result from it and never reads
 * the status register, which would hold the Amiga's bus cycle while the
 * firmware's FatFs call runs. */
static void test_mirror_send_never_reads_register(void) {
    static const char payload[] = "mirror payload";

    printf("test_mirror_send_never_reads_register\n");
    sim_reset();
    sim_mirror_delay = 1000;  /* firmware busy for a while each command */
    CHECK(fwup_open(&sim_mirror_io, "BOOT.bin") == FWUP_OK, "open OK");
    sim_mirror_delay = 1000;
    CHECK(fwup_write_chunk(&sim_mirror_io, payload, sizeof(payload) - 1) ==
          FWUP_OK, "write OK");
    sim_fail_write = 1;
    sim_mirror_delay = 10;
    CHECK(fwup_write_chunk(&sim_mirror_io, payload, 4) == FWUP_ERR_WRITE,
          "write error delivered through the mirror");
    CHECK(fwup_abort(&sim_mirror_io) == FWUP_OK, "abort OK");
    CHECK(sim_register_reads == 0, "status register never read");
    CHECK(sim_file_len == 0, "abort discarded the transfer");
}

/* A stale mirror value (the sequence the buffer happened to hold) is not
 * a completion: the client keeps waiting until the sequence moves. */
static void test_mirror_waits_for_new_sequence(void) {
    printf("test_mirror_waits_for_new_sequence\n");
    sim_reset();
    sim_mirror = ((uint32_t)FWUP_MIRROR_MAGIC << 24) | (1u << 16) |
                 FWUP_ERR_STATE;  /* a previous command's result */
    sim_mirror_seq = 1;
    sim_mirror_delay = 500;
    CHECK(fwup_open(&sim_mirror_io, "BOOT.bin") == FWUP_OK,
          "stale ERR_STATE ignored; the open's own OK returned");
    CHECK(sim_mirror_reads > 500, "client waited for the new sequence");
}

/* Before the firmware's first completion the mirror holds ROM padding,
 * whose byte 1 can equal the first published sequence. The first valid
 * word must still count as the completion. */
static void test_mirror_first_publication_after_invalid_word(void) {
    printf("test_mirror_first_publication_after_invalid_word\n");
    sim_reset();
    sim_mirror = 0x12015678u;  /* invalid magic, sequence byte 1 */
    sim_mirror_seq = 0;        /* the first completion publishes seq 1 */
    CHECK(fwup_open(&sim_mirror_io, "BOOT.bin") == FWUP_OK, "open OK");
    CHECK(sim_register_reads == 0, "completion taken from the mirror");
    CHECK(sim_mirror_reads < 10, "no wait past the completion");
}

/* A mirror that never advances is a timeout. The client must not read
 * the status register then: on hardware that read holds the bus until
 * the ARM answers, which may be never. */
static void test_mirror_frozen_times_out_without_register(void) {
    printf("test_mirror_frozen_times_out_without_register\n");
    sim_reset();
    sim_mirror_frozen = 1;
    CHECK(fwup_open(&sim_mirror_io, "BOOT.bin") == FWUP_STATUS_BUSY,
          "stalled mirror reported as a timeout");
    CHECK(sim_register_reads == 0, "register never read after the stall");
}

int main(void) {
    test_name_valid();
    test_strerror();
    test_probe_supported();
    test_probe_unsupported();
    test_full_send();
    test_write_error();
    test_restore();
    test_mirror_send_never_reads_register();
    test_mirror_waits_for_new_sequence();
    test_mirror_first_publication_after_invalid_word();
    test_mirror_frozen_times_out_without_register();
    test_bundle_order_and_cfg_exclusion();
    test_bundle_module_failure_stops_boot();
    test_bundle_without_module_capability();
    printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
