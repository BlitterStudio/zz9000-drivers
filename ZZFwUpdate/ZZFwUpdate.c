/*
 * ZZFwUpdate - Push a file onto the ZZ9000's FAT32 SD card from AmigaOS.
 *
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Thin CLI over the shared FWUP protocol client (common/fwup_client.c +
 * common/fwup_amiga.c): this file owns argument parsing, input
 * validation messages, progress display and the RESTORE confirmation;
 * the register protocol and the file push live in the shared client.
 *
 * Typical use:
 *   ZZFwUpdate ram:BOOT.bin                  -> installs release bundle
 *   ZZFwUpdate ram:ZZ9000-3.0/               -> installs its release bundle
 *   ZZFwUpdate ram:BOOT.bin BOOT.bin          -> writes one file explicitly
 *   ZZFwUpdate sys:Storage/zz9000-fw.bin BOOT.bin
 *   ZZFwUpdate RESTORE [name] [-y]            -> promote saved backup
 *
 * A one-argument BOOT.bin or release directory installs its ZZ9K modules
 * before BOOT.bin. Other source-file forms retain the root-file push mode.
 *
 * Build: m68k-amigaos-gcc -O2 -noixemul -I../include -I../common
 *        -o ZZFwUpdate ZZFwUpdate.c ../common/fwup_amiga.c \
 *        ../common/fwup_client.c -lamiga
 */

#include <exec/types.h>
#include <libraries/expansion.h>
#include <libraries/expansionbase.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/expansion.h>
#include <proto/dos.h>
#include <stdio.h>
#include <string.h>

#include "zz9000_hw.h"
#include "fwup_client.h"
#include "fwup_amiga.h"

#define ZZFWUPDATE_VERSION "2.4"
#define ZZFWUPDATE_DATE    "25.10.2026"

static const char version[] __attribute__((used)) =
    "$VER: ZZFwUpdate " ZZFWUPDATE_VERSION " (" ZZFWUPDATE_DATE ")\r\n";

#define FWUP_TICKS_PER_SECOND 50UL

/* The shared client is the authority for root names and module targets. */
static int validate_dest_name(const char *name) {
    if (fwup_name_valid(name))
        return 1;

    printf("ERROR: destination must be a root filename "
           "([A-Za-z0-9._-]{1,64})\n");
    printf("       or ZZ9K/NAME.ZZM (NAME is [A-Z0-9_]{1,8})\n");
    return 0;
}

/* Strip any AmigaDOS volume/path prefix so the user can pass
 * "sys:Storage/BOOT.bin" and get "BOOT.bin" on the SD when they don't
 * provide an explicit destination name. */
static const char *basename_amigados(const char *p) {
    const char *last = p;
    for (const char *q = p; *q; q++) {
        if (*q == '/' || *q == ':') last = q + 1;
    }
    return last;
}

static void flush_stdout(void) {
    fflush(stdout);
    Flush(Output());
}

static ULONG progress_percent(ULONG total, ULONG file_size) {
    if (file_size == 0) return 0;
    if (total >= file_size) return 100;
    return (ULONG)(((unsigned long long)total * 100ULL) / file_size);
}

static void print_progress(ULONG total, LONG file_size, char marker) {
    if (file_size > 0) {
        printf("\r  %lu / %ld bytes (%lu%%) %c",
               (unsigned long)total, (long)file_size,
               (unsigned long)progress_percent(total, (ULONG)file_size),
               marker);
    } else {
        printf("\r  %lu bytes %c", (unsigned long)total, marker);
    }
    flush_stdout();
}

/* fwup_send_file reports progress once before the first chunk (done == 0)
 * and once after each chunk; we redraw the byte/percent line each time. */
struct cli_progress {
    ULONG done;
    LONG  file_size;
};

static void cli_progress_cb(void *ctx, ULONG done, LONG total) {
    struct cli_progress *c = (struct cli_progress *)ctx;

    c->done = done;
    c->file_size = total;
    print_progress(done, total, ' ');
}

static ULONG elapsed_ticks(const struct DateStamp *start,
                           const struct DateStamp *end) {
    LONG days = end->ds_Days - start->ds_Days;
    LONG minutes = end->ds_Minute - start->ds_Minute;
    LONG ticks = end->ds_Tick - start->ds_Tick;
    LONG total = ((days * 24L * 60L) + minutes) *
                 60L * FWUP_TICKS_PER_SECOND + ticks;

    return (total > 0) ? (ULONG)total : 0;
}

static void print_done(ULONG total, const char *dest_name, ULONG ticks) {
    printf("Done. %lu bytes written to 0:/%s",
           (unsigned long)total, dest_name);
    if (ticks > 0) {
        ULONG seconds = ticks / FWUP_TICKS_PER_SECOND;
        ULONG tenths = ((ticks % FWUP_TICKS_PER_SECOND) * 10UL) /
                       FWUP_TICKS_PER_SECOND;
        ULONG kb_per_sec = ((total / 1024UL) * FWUP_TICKS_PER_SECOND) / ticks;
        printf(" in %lu.%lus (%lu KB/s)",
               (unsigned long)seconds, (unsigned long)tenths,
               (unsigned long)kb_per_sec);
    }
    printf("\n");
}

static void print_cmd_error(const char *cmd, UWORD st) {
    if (st == FWUP_STATUS_BUSY) {
        printf("ERROR: %s timed out waiting for firmware\n", cmd);
    } else {
        printf("ERROR: %s failed: %s (0x%04x)\n",
               cmd, fwup_strerror(st), st);
    }
}

static int streq_ci(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 32);
        if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 32);
        if (ca != cb) return 0;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static int source_is_directory(const char *path) {
    struct FileInfoBlock info;
    BPTR lock = Lock((STRPTR)path, ACCESS_READ);
    int is_directory = 0;

    if (!lock) return 0;
    if (Examine(lock, &info) && info.fib_DirEntryType > 0)
        is_directory = 1;
    UnLock(lock);
    return is_directory;
}

static int make_release_boot_path(const char *directory, char *boot_path,
                                  size_t boot_path_size) {
    size_t directory_len = strlen(directory);
    const char *separator = "/";
    int length;

    if (directory_len > 0 &&
        (directory[directory_len - 1] == '/' ||
         directory[directory_len - 1] == ':'))
        separator = "";

    length = snprintf(boot_path, boot_path_size, "%s%sBOOT.bin", directory,
                      separator);
    return length >= 0 && (size_t)length < boot_path_size;
}

static int is_boot_image_path(const char *path) {
    return streq_ci(basename_amigados(path), "BOOT.bin");
}

/* Returns 1 if the user confirmed. A non-interactive stdin (EOF) counts
 * as "no", so a scripted run without -y safely declines rather than
 * silently restoring. */
static int prompt_confirm(const char *target) {
    char line[16];

    printf("Restore the backup of 0:/%s as the active firmware?\n", target);
    printf("The current 0:/%s will be discarded and no backup will remain.\n",
           target);
    printf("Continue? [y/N]: ");
    flush_stdout();

    if (!fgets(line, sizeof(line), stdin))
        return 0;
    return (line[0] == 'y' || line[0] == 'Y');
}

static int do_restore(ULONG board, const char *target, int force) {
    UWORD st;

    if (!fwup_probe_board(board)) {
        printf("ERROR: firmware does not support the FWUP protocol\n");
        printf("       Update BOOT.bin to a firmware build with FWUP support.\n");
        return 1;
    }

    if (!force && !prompt_confirm(target)) {
        printf("Restore cancelled.\n");
        return 0;
    }

    printf("Restoring backup of 0:/%s ...\n", target);
    flush_stdout();

    /* The firmware derives the matching backup (e.g. BOOT.bin -> BOOT.bak)
     * from the active name on its side. */
    st = fwup_restore_board(board, target);

    if (st == FWUP_OK) {
        printf("Done. Backup promoted to 0:/%s. Reboot to run it.\n", target);
        return 0;
    }
    if (st == FWUP_ERR_UNKNOWN) {
        printf("ERROR: this firmware does not support RESTORE.\n");
        printf("       Update to a newer BOOT.bin first.\n");
        return 1;
    }
    if (st == FWUP_ERR_NO_BACKUP) {
        printf("ERROR: no backup found for 0:/%s; nothing to restore.\n",
               target);
        return 1;
    }
    print_cmd_error("RESTORE", st);
    return 1;
}

static int do_push(ULONG board, const char *src_path, const char *dest_name) {
    struct cli_progress prog = { 0, -1 };
    struct DateStamp started;
    struct DateStamp finished;
    UWORD st;

    if (!fwup_probe_board(board)) {
        printf("ERROR: firmware does not support the FWUP file-push protocol\n");
        printf("       Update BOOT.bin to a firmware build with FWUP support.\n");
        return 1;
    }

    printf("ZZ9000 at 0x%08lx, pushing %s -> 0:/%s\n",
           (unsigned long)board, src_path, dest_name);
    flush_stdout();

    DateStamp(&started);
    st = fwup_send_file(board, src_path, dest_name, cli_progress_cb, &prog);
    DateStamp(&finished);
    printf("\n");

    if (st == FWUP_OK) {
        print_done(prog.done, dest_name, elapsed_ticks(&started, &finished));
        return 0;
    }

    /* fwup_send_file aborts any partially-written file on the card itself. */
    if (st == FWUP_ERR_FILE) {
        printf("ERROR: cannot read %s\n", src_path);
    } else if (st == FWUP_ERR_NOMEM) {
        printf("ERROR: out of memory for the %d-byte transfer buffer\n",
               FWUP_CHUNK_BYTES);
    } else {
        print_cmd_error("transfer", st);
        printf("Any partial 0:/%s was aborted on the card.\n", dest_name);
    }
    return 1;
}

static int do_install_bundle(ULONG board, const char *boot_path) {
    static struct fwup_bundle bundle;
    struct cli_progress prog = { 0, -1 };
    char failed_name[FWUP_NAME_MAX + 1];
    UWORD st;
    int modules_skipped = 0;

    if (!fwup_probe_board(board)) {
        printf("ERROR: firmware does not support the FWUP file-push protocol\n");
        printf("       Update BOOT.bin to a firmware build with FWUP support.\n");
        return 1;
    }

    st = fwup_bundle_discover(boot_path, &bundle);
    if (st != FWUP_OK) {
        printf("ERROR: cannot read %s\n", boot_path);
        return 1;
    }

    printf("ZZ9000 at 0x%08lx, installing release from %s\n",
           (unsigned long)board, boot_path);
    flush_stdout();

    failed_name[0] = '\0';
    st = fwup_bundle_install(board, &bundle, fwup_board_capabilities(board),
                             cli_progress_cb, &prog, failed_name,
                             sizeof(failed_name), &modules_skipped);
    printf("\n");

    if (st == FWUP_OK) {
        printf("Done. Firmware bundle installed.\n");
        if (modules_skipped)
            printf("Reboot, then run ZZFwUpdate again to install firmware modules.\n");
        return 0;
    }

    if (failed_name[0]) {
        printf("ERROR: module %s failed: %s (0x%04x)\n", failed_name,
               fwup_strerror(st), st);
    } else if (st == FWUP_ERR_FILE) {
        printf("ERROR: cannot read %s\n", boot_path);
    } else {
        print_cmd_error("bundle transfer", st);
    }
    return 1;
}

int main(int argc, char *argv[]) {
    const char *src_path;
    const char *dest_name;
    char release_boot_path[256];
    int install_bundle = 0;
    ULONG board;
    /* Restore mode: ZZFwUpdate RESTORE [name] [-y] */
    if (argc >= 2 && streq_ci(argv[1], "RESTORE")) {
        const char *target = NULL;
        int force = 0;
        int i;

        for (i = 2; i < argc; i++) {
            if (streq_ci(argv[i], "-y") || streq_ci(argv[i], "FORCE")) {
                force = 1;
            } else if (!target) {
                target = argv[i];
            } else {
                printf("ERROR: too many arguments for RESTORE\n");
                return 1;
            }
        }
        if (!target) target = "BOOT.bin";
        if (!validate_dest_name(target))
            return 1;

        board = zz9000_find_board(NULL);
        if (!board) {
            printf("ERROR: ZZ9000 not found in expansion bus\n");
            return 1;
        }
        return do_restore(board, target, force);
    }

    if (argc < 2 || argc > 3 || argv[1][0] == '?') {
        printf("Usage: %s <BOOT.bin|release-dir|source-file> [dest-name]\n",
               argv[0]);
        printf("       %s RESTORE [name] [-y]\n", argv[0]);
        printf("  BOOT.bin/release-dir without dest-name installs its ZZ9K modules\n");
        printf("  before BOOT.bin. A source-file [dest-name] pushes one file.\n");
        printf("  dest-name   : root filename, or ZZ9K/NAME.ZZM for a module.\n");
        printf("  RESTORE     : promote the saved backup back to the active\n");
        printf("                firmware [name] (default BOOT.bin). -y skips\n");
        printf("                the confirmation prompt.\n");
        return 0;
    }

    src_path = argv[1];
    if (argc == 2 && source_is_directory(src_path)) {
        if (!make_release_boot_path(src_path, release_boot_path,
                                    sizeof(release_boot_path))) {
            printf("ERROR: release directory path is too long\n");
            return 1;
        }
        src_path = release_boot_path;
        install_bundle = 1;
    } else if (argc == 2 && is_boot_image_path(src_path)) {
        install_bundle = 1;
    }

    if (!install_bundle) {
        dest_name = (argc == 3) ? argv[2] : basename_amigados(src_path);
        if (!validate_dest_name(dest_name))
            return 1;
    }

    board = zz9000_find_board(NULL);
    if (!board) {
        printf("ERROR: ZZ9000 not found in expansion bus\n");
        return 1;
    }

    if (install_bundle)
        return do_install_bundle(board, src_path);
    return do_push(board, src_path, dest_name);
}
