/*
 * Amiga-side bindings for the shared FWUP protocol client.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <string.h>

#include <exec/types.h>

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
/* The mirror is the boot ROM window's last longword, which the FPGA
 * serves from card memory without waiting for the ARM. */
static uint32_t board_read_mirror(struct fwup_io *io) {
    return *(volatile ULONG *)((ULONG)io->ctx + ZZ_FWUP_STATUS_MIRROR_OFFSET);
}
static void board_idle(struct fwup_io *io) {
    (void)io;
    Delay(1);
}

UWORD fwup_board_capabilities(ULONG board) {
    return *(volatile UWORD *)(board + ZZ_REG_FW_CAPABILITIES);
}

void fwup_io_init_board(struct fwup_io *io, ULONG board) {
    UWORD caps = fwup_board_capabilities(board);

    io->read_status = board_read_status;
    io->write_cmd   = board_write_cmd;
    io->write_len   = board_write_len;
    io->buffer      = board_buffer;
    io->ctx         = (void *)board;
    io->read_mirror = (caps & ZZ_FW_CAP_FWUP_STATUS_MIRROR) ?
                      board_read_mirror : NULL;
    io->idle        = board_idle;
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
static int amiga_bundle_dir(const char *boot_path, char *dir,
                            ULONG dir_size) {
    const char *last = NULL;
    const char *p;
    ULONG prefix;

    if (!boot_path) return 0;
    for (p = boot_path; *p; p++) {
        if (*p == '/' || *p == ':' || *p == '\\') last = p;
    }
    prefix = last ? (ULONG)(last - boot_path + 1) : 0;
    if (prefix + 5 >= dir_size) return 0;
    if (prefix) memcpy(dir, boot_path, prefix);
    memcpy(dir + prefix, "ZZ9K/", 6);
    return 1;
}

static int amiga_module_path(char *path, ULONG path_size, const char *dir,
                             const char *name) {
    ULONG dir_len = (ULONG)strlen(dir);
    ULONG name_len = (ULONG)strlen(name);

    if (dir_len + name_len >= path_size) return 0;
    memcpy(path, dir, dir_len);
    memcpy(path + dir_len, name, name_len + 1);
    return 1;
}

static int amiga_module_extension(const char *name) {
    ULONG len = (ULONG)strlen(name);
    return len >= 4 && strcmp(name + len - 4, ".ZZM") == 0;
}

static int bundle_insert_module(struct fwup_bundle *bundle, const char *dir,
                                const char *name) {
    UWORD i;

    for (i = bundle->module_count; i > 0 &&
         strcmp(name, bundle->module_names[i - 1]) < 0; i--) {
        memcpy(bundle->module_names[i], bundle->module_names[i - 1],
               sizeof(bundle->module_names[i]));
        memcpy(bundle->module_paths[i], bundle->module_paths[i - 1],
               sizeof(bundle->module_paths[i]));
    }
    if (!amiga_module_path(bundle->module_paths[i],
                           sizeof(bundle->module_paths[i]), dir, name))
        return 0;
    memcpy(bundle->module_names[i], name, strlen(name) + 1);
    bundle->module_count++;
    return 1;
}

UWORD fwup_bundle_discover(const char *boot_path, struct fwup_bundle *bundle) {
    BPTR boot, dir_lock;
    struct FileInfoBlock *fib;
    char module_dir[256];

    if (!boot_path || !bundle || strlen(boot_path) >= sizeof(bundle->boot_path))
        return FWUP_ERR_FILE;
    boot = Open((STRPTR)boot_path, MODE_OLDFILE);
    if (!boot) return FWUP_ERR_FILE;
    Close(boot);

    memset(bundle, 0, sizeof(*bundle));
    memcpy(bundle->boot_path, boot_path, strlen(boot_path) + 1);
    if (!amiga_bundle_dir(boot_path, module_dir, sizeof(module_dir)))
        return FWUP_ERR_FILE;
    dir_lock = Lock(module_dir, ACCESS_READ);
    if (!dir_lock) return FWUP_OK;

    fib = (struct FileInfoBlock *)AllocMem(sizeof(*fib), MEMF_PUBLIC);
    if (!fib) {
        UnLock(dir_lock);
        return FWUP_ERR_NOMEM;
    }
    if (!Examine(dir_lock, fib)) {
        FreeMem(fib, sizeof(*fib));
        UnLock(dir_lock);
        return FWUP_ERR_FILE;
    }
    bundle->module_dir_present = TRUE;
    while (ExNext(dir_lock, fib)) {
        if (fib->fib_DirEntryType >= 0 ||
            !amiga_module_extension(fib->fib_FileName))
            continue;
        if (!fwup_module_name_valid(fib->fib_FileName)) {
            FreeMem(fib, sizeof(*fib));
            UnLock(dir_lock);
            return FWUP_ERR_BAD_NAME;
        }
        if (bundle->module_count == FWUP_BUNDLE_MAX_MODULES) {
            FreeMem(fib, sizeof(*fib));
            UnLock(dir_lock);
            return FWUP_ERR_BUNDLE;
        }
        if (!bundle_insert_module(bundle, module_dir, fib->fib_FileName)) {
            FreeMem(fib, sizeof(*fib));
            UnLock(dir_lock);
            return FWUP_ERR_FILE;
        }
    }
    FreeMem(fib, sizeof(*fib));
    UnLock(dir_lock);
    return FWUP_OK;
}

static int amiga_file_size(void *ctx, const char *path, uint32_t *size);

static int amiga_list_dir(void *ctx, const char *path,
                          fwup_bundle_visit_fn visit, void *visit_ctx) {
    const struct fwup_bundle *bundle = (const struct fwup_bundle *)ctx;
    UWORD i;
    uint32_t size;

    (void)path;
    if (!bundle->module_dir_present) return 0;
    for (i = 0; i < bundle->module_count; i++) {
        if (amiga_file_size(NULL, bundle->module_paths[i], &size) != 0 ||
            visit(visit_ctx, bundle->module_names[i], size) != 0)
            return -1;
    }
    return 1;
}

static int amiga_file_size(void *ctx, const char *path, uint32_t *size) {
    BPTR file;
    LONG length;

    (void)ctx;
    file = Open((STRPTR)path, MODE_OLDFILE);
    if (!file) return -1;
    if (Seek(file, 0, OFFSET_END) < 0 ||
        (length = Seek(file, 0, OFFSET_BEGINNING)) < 0) {
        Close(file);
        return -1;
    }
    Close(file);
    *size = (uint32_t)length;
    return 0;
}

static int amiga_open_file(void *ctx, const char *path, void **file) {
    BPTR handle;

    (void)ctx;
    handle = Open((STRPTR)path, MODE_OLDFILE);
    if (!handle) return -1;
    *file = (void *)handle;
    return 0;
}

static int amiga_read_file(void *ctx, void *file, uint8_t *buffer,
                           uint16_t capacity, uint16_t *used) {
    LONG n;

    (void)ctx;
    n = Read((BPTR)file, buffer, capacity);
    if (n < 0) return -1;
    *used = (uint16_t)n;
    return 0;
}

static void amiga_close_file(void *ctx, void *file) {
    (void)ctx;
    Close((BPTR)file);
}

struct amiga_bundle_progress {
    fwup_progress_fn callback;
    void *ctx;
};

static void amiga_progress(void *ctx, const char *target, uint32_t done,
                           uint32_t total) {
    struct amiga_bundle_progress *progress =
        (struct amiga_bundle_progress *)ctx;

    (void)target;
    if (progress->callback)
        progress->callback(progress->ctx, (ULONG)done, (LONG)total);
}

UWORD fwup_bundle_install(ULONG board, const struct fwup_bundle *bundle,
                          UWORD fw_caps, fwup_progress_fn cb, void *ctx,
                          char *failed_name, ULONG failed_name_size,
                          int *modules_skipped) {
    struct fwup_bundle_files files;
    struct fwup_io io;
    struct fwup_bundle_result result;
    struct amiga_bundle_progress progress;
    UWORD status;

    if (!bundle || !bundle->boot_path[0]) return FWUP_ERR_FILE;
    if (failed_name && failed_name_size) failed_name[0] = '\0';
    if (modules_skipped) *modules_skipped = FALSE;
    files.list_dir = amiga_list_dir;
    files.file_size = amiga_file_size;
    files.open = amiga_open_file;
    files.read = amiga_read_file;
    files.close = amiga_close_file;
    files.ctx = (void *)bundle;
    progress.callback = cb;
    progress.ctx = ctx;
    fwup_io_init_board(&io, board);
    status = (UWORD)fwup_install_bundle(&io, fw_caps, bundle->boot_path, &files,
                                        amiga_progress, &progress, &result);
    if (modules_skipped)
        *modules_skipped =
            result.outcome == FWUP_BUNDLE_BOOT_ONLY_NO_MODULE_DIR ? TRUE : FALSE;
    if (result.outcome == FWUP_BUNDLE_MODULE_FAILED &&
        failed_name && failed_name_size) {
        ULONG n = (ULONG)strlen(result.failed_module);
        if (n >= failed_name_size) n = failed_name_size - 1;
        memcpy(failed_name, result.failed_module, n);
        failed_name[n] = '\0';
    }
    return status;
}
