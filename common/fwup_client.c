/*
 * Shared ZZ9000 firmware-file push (FWUP) protocol client — portable core.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <string.h>

#include "fwup_client.h"

/* A non-yielding caller retains the original poll-count timeout. Board
 * callers yield between status reads, so use a bounded 30-second deadline
 * instead of stretching that count into many hours. */
#define FWUP_POLL_LIMIT 2000000UL
#ifndef FWUP_LEGACY_POLL_IDLES
#define FWUP_LEGACY_POLL_IDLES 1500UL /* about 30 s of one-tick yields */
#endif

/* A 2.8.1 receiver runs FWUP FatFs operations in its core-0 Zorro service
 * loop. Keep its ARM-serviced status register untouched while that work
 * starts, then poll it at one-tick intervals. */
#ifndef FWUP_LEGACY_OPEN_QUIET_TICKS
#define FWUP_LEGACY_OPEN_QUIET_TICKS 100UL /* 2 s: stale TMP unlink */
#endif
#ifndef FWUP_LEGACY_WRITE_QUIET_TICKS
#define FWUP_LEGACY_WRITE_QUIET_TICKS 2UL /* 40 ms: two typical 16 KiB writes */
#endif
#ifndef FWUP_LEGACY_CLOSE_QUIET_TICKS
#define FWUP_LEGACY_CLOSE_QUIET_TICKS 50UL /* 1 s: sync/rename/backup */
#endif

/* Mirror polling: spin on the card-memory longword first (each read is a
 * short Zorro III cycle that never waits on the ARM), then yield between
 * reads. Past both budgets the command has timed out. */
#ifndef FWUP_MIRROR_SPINS
#define FWUP_MIRROR_SPINS 50000UL
#endif
#ifndef FWUP_MIRROR_IDLES
#define FWUP_MIRROR_IDLES 1500UL /* about 30 s of one-tick yields */
#endif

static int valid_dest_char(char c) {
    return (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') ||
           c == '.' || c == '_' || c == '-';
}

static int valid_module_char(char c) {
    return (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

int fwup_module_name_valid(const char *name) {
    size_t len, i;

    if (!name) return 0;
    len = strlen(name);
    if (len < 5 || len > FWUP_MODULE_FILENAME_MAX ||
        strcmp(name + len - 4, ".ZZM") != 0) return 0;
    if (len - 4 > FWUP_MODULE_NAME_MAX) return 0;
    for (i = 0; i < len - 4; i++) {
        if (!valid_module_char(name[i])) return 0;
    }
    return 1;
}

int fwup_name_valid(const char *name) {
    static const char module_prefix[] = "ZZ9K/";
    size_t len, i;

    if (!name) return 0;
    if (strncmp(name, module_prefix, sizeof(module_prefix) - 1) == 0)
        return fwup_module_name_valid(name + sizeof(module_prefix) - 1);
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
    case FWUP_ERR_BUNDLE:    return "invalid firmware module bundle";
    case FWUP_ERR_CONFIG_SIZE: return "configuration exceeds file size limit";
    case FWUP_ERR_NOMEM:     return "out of memory";
    case FWUP_ERR_FILE:      return "cannot read the source file";
    case FWUP_STATUS_BUSY:   return "timed out waiting for firmware";
    default:                 return "unknown error";
    }
}

/* The firmware sets STATUS = 0xFFFF when it accepts a command and clears
 * it to the result once the FatFs call returns.  Board transports provide
 * idle, which makes this safe for legacy core-0 receivers; simple host
 * transports retain the historical tight polling behavior. */
static uint16_t poll_status(struct fwup_io *io) {
    unsigned long budget;
    uint16_t st;

    if (!io->idle) {
        budget = FWUP_POLL_LIMIT;
        do {
            st = io->read_status(io);
            if (st != FWUP_STATUS_BUSY) return st;
        } while (--budget);
        return FWUP_STATUS_BUSY;
    }

    for (budget = 0; budget < FWUP_LEGACY_POLL_IDLES; budget++) {
        st = io->read_status(io);
        if (st != FWUP_STATUS_BUSY) return st;
        io->idle(io);
    }
    return FWUP_STATUS_BUSY;
}

/* A completion is a valid mirror whose sequence differs from the one
 * read before the command was issued. Before the firmware's first
 * completion the mirror holds no valid word, and then any valid word is
 * the new one, whatever its sequence byte. */
static int mirror_completed(uint32_t now, uint32_t before) {
    if ((now >> 24) != FWUP_MIRROR_MAGIC) return 0;
    if ((before >> 24) != FWUP_MIRROR_MAGIC) return 1;
    return ((now >> 16) & 0xFFu) != ((before >> 16) & 0xFFu);
}

static uint16_t poll_mirror(struct fwup_io *io, uint32_t before) {
    unsigned long n;
    uint32_t now;

    for (n = 0; n < FWUP_MIRROR_SPINS; n++) {
        now = io->read_mirror(io);
        if (mirror_completed(now, before)) return (uint16_t)now;
    }
    if (io->idle) {
        for (n = 0; n < FWUP_MIRROR_IDLES; n++) {
            io->idle(io);
            now = io->read_mirror(io);
            if (mirror_completed(now, before)) return (uint16_t)now;
        }
    }
    /* Nothing else writes the mirror, so the command is still running or
     * the card is wedged. Reading the register now would hold the bus
     * until the ARM answers, which is the hang this path avoids. */
    return FWUP_STATUS_BUSY;
}

static unsigned long legacy_quiet_ticks(uint16_t cmd) {
    switch (cmd) {
    case FWUP_CMD_OPEN:  return FWUP_LEGACY_OPEN_QUIET_TICKS;
    case FWUP_CMD_WRITE: return FWUP_LEGACY_WRITE_QUIET_TICKS;
    case FWUP_CMD_CLOSE: return FWUP_LEGACY_CLOSE_QUIET_TICKS;
    default:             return 0;
    }
}

static void idle_ticks(struct fwup_io *io, unsigned long ticks) {
    while (ticks-- != 0) io->idle(io);
}

static uint16_t run_command(struct fwup_io *io, uint16_t cmd) {
    uint32_t before;

    if (!io->read_mirror) {
        io->write_cmd(io, cmd);
        if (io->idle) idle_ticks(io, legacy_quiet_ticks(cmd));
        return poll_status(io);
    }
    before = io->read_mirror(io);
    io->write_cmd(io, cmd);
    return poll_mirror(io, before);
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
    st = run_command(io, FWUP_CMD_ABORT);
    if (st == FWUP_STATUS_BUSY) return 0;

    io->write_len(io, 0);
    st = run_command(io, FWUP_CMD_WRITE);
    return (st == FWUP_ERR_STATE || st == FWUP_ERR_LEN) ? 1 : 0;
}

uint16_t fwup_open(struct fwup_io *io, const char *dest_name) {
    if (!fwup_name_valid(dest_name)) return FWUP_ERR_BAD_NAME;
    stage_name(io, dest_name);
    return run_command(io, FWUP_CMD_OPEN);
}

uint16_t fwup_write_chunk(struct fwup_io *io, const void *data, uint16_t len) {
    uint8_t *buffer;

    if (len == 0 || len > FWUP_CHUNK_BYTES) return FWUP_ERR_LEN;
    buffer = io->buffer(io);
    if (data != buffer) memcpy(buffer, data, len);
    io->write_len(io, len);
    return run_command(io, FWUP_CMD_WRITE);
}
uint16_t fwup_close(struct fwup_io *io) {
    return run_command(io, FWUP_CMD_CLOSE);
}

uint16_t fwup_abort(struct fwup_io *io) {
    return run_command(io, FWUP_CMD_ABORT);
}

uint16_t fwup_restore(struct fwup_io *io, const char *target) {
    if (!fwup_name_valid(target)) return FWUP_ERR_BAD_NAME;
    stage_name(io, target);
    return run_command(io, FWUP_CMD_RESTORE);
}

struct bundle_module {
    char name[FWUP_MODULE_FILENAME_MAX + 1];
    uint32_t size;
};

struct bundle_modules {
    struct bundle_module entries[FWUP_BUNDLE_MODULE_MAX];
    unsigned count;
    uint16_t status;
};

static int module_name_has_extension(const char *name) {
    size_t len;

    if (!name) return 0;
    len = strlen(name);
    return len >= 4 && strcmp(name + len - 4, ".ZZM") == 0;
}

static int collect_module(void *ctx, const char *name, uint32_t size) {
    struct bundle_modules *modules = (struct bundle_modules *)ctx;
    struct bundle_module entry;
    unsigned i;

    if (!module_name_has_extension(name)) return 0;
    if (!fwup_module_name_valid(name)) {
        modules->status = FWUP_ERR_BAD_NAME;
        return -1;
    }
    if (modules->count == FWUP_BUNDLE_MODULE_MAX) {
        modules->status = FWUP_ERR_BUNDLE;
        return -1;
    }

    memcpy(entry.name, name, strlen(name) + 1);
    entry.size = size;
    for (i = modules->count; i > 0 &&
         strcmp(entry.name, modules->entries[i - 1].name) < 0; i--)
        modules->entries[i] = modules->entries[i - 1];
    modules->entries[i] = entry;
    modules->count++;
    return 0;
}

static int make_module_dir(const char *boot_path, char *dir) {
    const char *last = NULL;
    const char *p;
    size_t prefix;

    if (!boot_path) return 0;
    for (p = boot_path; *p; p++) {
        if (*p == '/' || *p == ':' || *p == '\\') last = p;
    }
    prefix = last ? (size_t)(last - boot_path + 1) : 0;
    if (prefix + 5 >= FWUP_HOST_PATH_MAX) return 0;
    if (prefix) memcpy(dir, boot_path, prefix);
    memcpy(dir + prefix, "ZZ9K/", 6);
    return 1;
}

static int make_module_path(char *path, const char *dir, const char *name) {
    size_t dir_len = strlen(dir);
    size_t name_len = strlen(name);

    if (dir_len + name_len >= FWUP_HOST_PATH_MAX) return 0;
    memcpy(path, dir, dir_len);
    memcpy(path + dir_len, name, name_len + 1);
    return 1;
}

static uint16_t send_bundle_file(struct fwup_io *io,
                                 const struct fwup_bundle_files *files,
                                 const char *host_path, const char *target,
                                 uint32_t *done, uint32_t total,
                                 fwup_bundle_progress_fn progress, void *ctx) {
    void *file;
    uint16_t used, st;

    if (files->open(files->ctx, host_path, &file) != 0) return FWUP_ERR_FILE;
    st = fwup_open(io, target);
    if (st != FWUP_OK) {
        files->close(files->ctx, file);
        return st;
    }

    for (;;) {
        if (files->read(files->ctx, file, io->buffer(io), FWUP_CHUNK_BYTES,
                        &used) != 0) {
            st = FWUP_ERR_FILE;
            break;
        }
        if (used == 0) {
            st = FWUP_OK;
            break;
        }
        st = fwup_write_chunk(io, io->buffer(io), used);
        if (st != FWUP_OK) break;
        *done += used;
        if (progress) progress(ctx, target, *done, total);
    }

    if (st == FWUP_OK) st = fwup_close(io);
    if (st != FWUP_OK) fwup_abort(io);
    files->close(files->ctx, file);
    return st;
}

uint16_t fwup_install_bundle(struct fwup_io *io, uint16_t fw_caps,
                             const char *boot_host_path,
                             const struct fwup_bundle_files *files,
                             fwup_bundle_progress_fn progress, void *ctx,
                             struct fwup_bundle_result *result) {
    struct bundle_modules modules;
    char module_dir[FWUP_HOST_PATH_MAX];
    char module_path[FWUP_HOST_PATH_MAX];
    char target[FWUP_MODULE_TARGET_MAX + 1];
    uint32_t boot_size, total, done;
    unsigned i;
    int listed;
    uint16_t st;

    if (!io || !boot_host_path || !files || !files->list_dir ||
        !files->file_size || !files->open || !files->read || !files->close)
        return FWUP_ERR_FILE;

    if (result) {
        result->status = FWUP_OK;
        result->outcome = FWUP_BUNDLE_INSTALLED;
        result->failed_module[0] = '\0';
    }
    if (files->file_size(files->ctx, boot_host_path, &boot_size) != 0)
        return FWUP_ERR_FILE;
    total = boot_size;
    done = 0;
    modules.count = 0;
    modules.status = FWUP_OK;

    if ((fw_caps & (1U << 14)) != 0) {
        if (!make_module_dir(boot_host_path, module_dir)) return FWUP_ERR_FILE;
        listed = files->list_dir(files->ctx, module_dir, collect_module, &modules);
        if (listed < 0) return modules.status != FWUP_OK ?
                          modules.status : FWUP_ERR_FILE;
        for (i = 0; i < modules.count; i++) {
            if (total > 0xffffffffUL - modules.entries[i].size)
                return FWUP_ERR_BUNDLE;
            total += modules.entries[i].size;
        }
    } else if (result) {
        result->outcome = FWUP_BUNDLE_BOOT_ONLY_NO_MODULE_DIR;
    }

    if (progress) progress(ctx, "", done, total);
    for (i = 0; i < modules.count; i++) {
        if (!make_module_path(module_path, module_dir, modules.entries[i].name))
            return FWUP_ERR_FILE;
        memcpy(target, "ZZ9K/", 5);
        memcpy(target + 5, modules.entries[i].name,
               strlen(modules.entries[i].name) + 1);
        st = send_bundle_file(io, files, module_path, target, &done, total,
                              progress, ctx);
        if (st != FWUP_OK) {
            if (result) {
                result->status = st;
                result->outcome = FWUP_BUNDLE_MODULE_FAILED;
                memcpy(result->failed_module, modules.entries[i].name,
                       strlen(modules.entries[i].name) + 1);
            }
            return st;
        }
    }

    st = send_bundle_file(io, files, boot_host_path, "BOOT.bin", &done, total,
                          progress, ctx);
    if (result) result->status = st;
    return st;
}
