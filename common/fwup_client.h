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
#define FWUP_ERR_BUNDLE    0x00FBu /* client: invalid firmware module bundle */
#define FWUP_ERR_CONFIG_SIZE 0x00FCu /* client: generated config does not fit */
#define FWUP_ERR_NOMEM     0x00FDu /* client: out of memory */
#define FWUP_ERR_FILE      0x00FEu /* client: cannot read the source file */

/* Largest chunk staged per WRITE. Stays under the firmware's 24576-byte
 * shared-buffer window and matches the SD-boot residency story. */
#define FWUP_CHUNK_BYTES   16384

/* Destination filenames accepted by firmware. Legacy names remain flat at
 * the root; module files are the only permitted subdirectory targets. */
#define FWUP_NAME_MAX             64
#define FWUP_MODULE_NAME_MAX       8
#define FWUP_MODULE_FILENAME_MAX  12 /* NAME.ZZM */
#define FWUP_MODULE_TARGET_MAX    17 /* ZZ9K/NAME.ZZM */
#define FWUP_BUNDLE_MODULE_MAX   128
#define FWUP_HOST_PATH_MAX       512

/* Register/buffer access seam. On Amiga these do volatile MMIO at the
 * Zorro board window (fwup_amiga.c); host tests point them at a
 * simulated firmware. `read_status` returns FWUP_STATUS_BUSY until the
 * firmware finishes the last command.
 *
 * `read_mirror` (NULL when the firmware lacks it) reads the status
 * mirror longword at the end of the boot ROM window: FWUP_MIRROR_MAGIC in
 * bits 31..24, a sequence the firmware advances on every completed
 * command in bits 23..16, and that command's status in bits 15..0. The
 * FPGA answers it from card memory, while a status register read waits
 * for the ARM to finish its FatFs call with the Amiga's bus cycle held.
 * A client with a mirror therefore never reads the register while a
 * command runs. `idle` (may be NULL) yields the CPU during long waits. */
struct fwup_io {
    uint16_t (*read_status)(struct fwup_io *io);
    void     (*write_cmd)(struct fwup_io *io, uint16_t cmd);
    void     (*write_len)(struct fwup_io *io, uint16_t len);
    uint8_t *(*buffer)(struct fwup_io *io);
    void     *ctx;
    uint32_t (*read_mirror)(struct fwup_io *io);
    void     (*idle)(struct fwup_io *io);
};

#define FWUP_MIRROR_MAGIC 0x46u

/* Pure helpers. */
int         fwup_module_name_valid(const char *name); /* NAME.ZZM, NAME [A-Z0-9_]{1,8} */
int         fwup_name_valid(const char *name); /* legacy root name, or ZZ9K/NAME.ZZM */
const char *fwup_strerror(uint16_t status);

/* Portable host-file seam for a firmware bundle. list_dir returns 1 when
 * the directory was enumerated, 0 when it does not exist, and -1 on error.
 * read returns 0 on success and stores the byte count (zero at EOF).
 * file_size and open return 0 on success. */
typedef int (*fwup_bundle_visit_fn)(void *visit_ctx, const char *name,
                                    uint32_t size);
struct fwup_bundle_files {
    int (*list_dir)(void *ctx, const char *path, fwup_bundle_visit_fn visit,
                    void *visit_ctx);
    int (*file_size)(void *ctx, const char *path, uint32_t *size);
    int (*open)(void *ctx, const char *path, void **file);
    int (*read)(void *ctx, void *file, uint8_t *buffer, uint16_t capacity,
                uint16_t *used);
    void (*close)(void *ctx, void *file);
    void *ctx;
};

/* Progress covers the complete bundle rather than one source file. target
 * is the firmware destination currently being transferred. */
typedef void (*fwup_bundle_progress_fn)(void *ctx, const char *target,
                                        uint32_t done, uint32_t total);

enum fwup_bundle_outcome {
    FWUP_BUNDLE_INSTALLED,
    FWUP_BUNDLE_BOOT_ONLY_NO_MODULE_DIR,
    FWUP_BUNDLE_MODULE_FAILED
};

struct fwup_bundle_result {
    uint16_t status;
    enum fwup_bundle_outcome outcome;
    char failed_module[FWUP_MODULE_FILENAME_MAX + 1];
};

/* Install all valid sibling ZZ9K/*.ZZM files before BOOT.bin. fw_caps is
 * ZZ_REG_FW_CAPABILITIES; without bit 14 this installs BOOT.bin only and
 * reports FWUP_BUNDLE_BOOT_ONLY_NO_MODULE_DIR. */
uint16_t fwup_install_bundle(struct fwup_io *io, uint16_t fw_caps,
                             const char *boot_host_path,
                             const struct fwup_bundle_files *files,
                             fwup_bundle_progress_fn progress, void *ctx,
                             struct fwup_bundle_result *result);

/* Protocol primitives: issue the command and poll until the firmware
 * reports a result (or FWUP_STATUS_BUSY on timeout). */
uint16_t fwup_probe(struct fwup_io *io);        /* 1 supported, 0 not */
uint16_t fwup_open(struct fwup_io *io, const char *dest_name);
uint16_t fwup_write_chunk(struct fwup_io *io, const void *data, uint16_t len);
uint16_t fwup_close(struct fwup_io *io);
uint16_t fwup_abort(struct fwup_io *io);
uint16_t fwup_restore(struct fwup_io *io, const char *target);

#endif /* FWUP_CLIENT_H */
