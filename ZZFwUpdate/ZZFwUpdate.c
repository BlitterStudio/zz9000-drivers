/*
 * ZZFwUpdate - Push a file onto the ZZ9000's FAT32 SD card from AmigaOS.
 *
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Talks to firmware builds that expose the REG_ZZ_FWUP_* protocol:
 *   1. stage NUL-terminated filename in shared buffer at board+0xA000
 *   2. CMD = OPEN, poll STATUS
 *   3. per chunk: stage bytes, LEN = chunk_bytes, CMD = WRITE, poll
 *   4. CMD = CLOSE (success) or CMD = ABORT (delete the partial)
 *
 * Typical use:
 *   ZZFwUpdate ram:BOOT.bin                  -> writes 0:/BOOT.bin
 *   ZZFwUpdate ram:BOOT.bin BOOT.bin         -> same, explicit dest name
 *   ZZFwUpdate sys:Storage/zz9000-fw.bin BOOT.bin
 *
 * Filename rules on the SD side: flat root, [A-Za-z0-9._-], max 64
 * chars. The firmware will reject anything else with FWUP_ERR_BAD_NAME.
 *
 * Build: m68k-amigaos-gcc -O2 -noixemul -o ZZFwUpdate ZZFwUpdate.c -lamiga
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <libraries/expansion.h>
#include <libraries/expansionbase.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/expansion.h>
#include <proto/dos.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ZZFWUPDATE_VERSION "2.1"
#define ZZFWUPDATE_DATE    "17.05.2026"

static const char version[] __attribute__((used)) =
    "$VER: ZZFwUpdate " ZZFWUPDATE_VERSION " (" ZZFWUPDATE_DATE ")\r\n";

#define MNT_MANUFACTURER   0x6d6e
#define ZZ9000_PRODUCT_Z3  4
#define ZZ9000_PRODUCT_Z2  3

/* Mirrors zz9000-firmware/ZZ9000_proto.sdk/ZZ9000OS/src/zz_regs.h. */
#define REG_FWUP_CMD       0xCA
#define REG_FWUP_LEN       0xCC
#define REG_FWUP_STATUS    0xCE
#define ZZ_BUFFER_OFFSET   0xA000

#define FWUP_CMD_OPEN      1
#define FWUP_CMD_WRITE     2
#define FWUP_CMD_CLOSE     3
#define FWUP_CMD_ABORT     4

#define FWUP_STATUS_BUSY   0xFFFF
#define FWUP_OK            0x0000
#define FWUP_ERR_STATE     0x0007
#define FWUP_ERR_LEN       0x0008

/* Stays well under the firmware's FWUP_MAX_CHUNK (24576) and matches
 * the chunk size the SD-boot path uses, so we share the same buffer
 * residency story. */
#define FWUP_CHUNK_BYTES   16384

/* Roughly matches SD-boot's poll budget — each FatFs write can stall
 * for tens of ms on a slow SD card, so a small counter wouldn't
 * survive a real card under load. */
#define FWUP_POLL_LIMIT    2000000
#define FWUP_SPINNER_INTERVAL 32768UL
#define FWUP_TICKS_PER_SECOND 50UL

struct fwup_progress;
static void update_busy_progress(struct fwup_progress *progress);

static const char *fwup_strerror(UWORD code) {
    switch (code) {
    case 0x00: return "OK";
    case 0x02: return "no SD card / FAT not mounted on firmware";
    case 0x03: return "bad filename (flat-root, [A-Za-z0-9._-], <=64 chars)";
    case 0x04: return "firmware f_open failed";
    case 0x05: return "firmware f_write failed";
    case 0x06: return "firmware f_close/f_sync failed";
    case FWUP_ERR_STATE: return "protocol state error (WRITE/CLOSE without OPEN?)";
    case FWUP_ERR_LEN: return "chunk length out of range";
    case 0x09: return "unknown FWUP command";
    case FWUP_STATUS_BUSY: return "timed out waiting for firmware";
    default:   return "unknown error";
    }
}

static int valid_dest_char(char c) {
    return (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') ||
           c == '.' || c == '_' || c == '-';
}

static int validate_dest_name(const char *name) {
    size_t len = strlen(name);
    size_t i;

    if (len == 0 || len > 64) {
        printf("ERROR: destination name must be 1..64 chars\n");
        return 0;
    }

    for (i = 0; i < len; i++) {
        if (!valid_dest_char(name[i])) {
            printf("ERROR: destination name may only contain "
                   "A-Z, a-z, 0-9, '.', '_' and '-'\n");
            printf("       Bad character at position %lu\n",
                   (unsigned long)(i + 1));
            return 0;
        }
    }

    return 1;
}

static ULONG find_zz9000(void) {
    struct ExpansionBase *ExpBase;
    struct ConfigDev *cd = NULL;
    ULONG addr = 0;

    ExpBase = (struct ExpansionBase *)OpenLibrary((CONST_STRPTR)"expansion.library", 0);
    if (!ExpBase) return 0;

    while ((cd = FindConfigDev(cd, MNT_MANUFACTURER, ZZ9000_PRODUCT_Z3)))
        { addr = (ULONG)cd->cd_BoardAddr; break; }

    if (!addr) {
        cd = NULL;
        while ((cd = FindConfigDev(cd, MNT_MANUFACTURER, ZZ9000_PRODUCT_Z2)))
            { addr = (ULONG)cd->cd_BoardAddr; break; }
    }

    CloseLibrary((struct Library *)ExpBase);
    return addr;
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

static UWORD poll_status(volatile UWORD *status_reg,
                         struct fwup_progress *progress) {
    /* The firmware sets STATUS = 0xFFFF when it accepts a CMD and
     * clears it to the result once the FatFs call returns. Status
     * register sits in the cache-inhibit Zorro register window, so no
     * cache management is needed on the m68k side. */
    ULONG budget = FWUP_POLL_LIMIT;
    UWORD st;
    do {
        st = *status_reg;
        if (st != FWUP_STATUS_BUSY) return st;
        if (progress && ((budget & (FWUP_SPINNER_INTERVAL - 1)) == 0))
            update_busy_progress(progress);
    } while (--budget);
    return FWUP_STATUS_BUSY;
}

static int probe_fwup_protocol(volatile UWORD *cmd_reg,
                               volatile UWORD *len_reg,
                               volatile UWORD *status_reg) {
    UWORD st;

    /* Old firmware leaves these formerly-unused registers reading as
     * OK, so use a non-destructive command that only a FWUP-capable
     * firmware answers with a protocol error. ABORT first also clears
     * any stale interrupted transfer. */
    *cmd_reg = FWUP_CMD_ABORT;
    st = poll_status(status_reg, NULL);
    if (st == FWUP_STATUS_BUSY) return 0;

    *len_reg = 0;
    *cmd_reg = FWUP_CMD_WRITE;
    st = poll_status(status_reg, NULL);
    return (st == FWUP_ERR_STATE || st == FWUP_ERR_LEN);
}

static void abort_transfer(volatile UWORD *cmd_reg,
                           volatile UWORD *status_reg,
                           const char *dest_name) {
    UWORD st;

    *cmd_reg = FWUP_CMD_ABORT;
    st = poll_status(status_reg, NULL);
    if (st == FWUP_OK) {
        printf("Partial transfer aborted; 0:/%s deleted on the card.\n",
               dest_name);
    } else if (st == FWUP_STATUS_BUSY) {
        printf("WARNING: ABORT timed out; 0:/%s may remain.\n", dest_name);
    } else {
        printf("WARNING: ABORT failed: %s (0x%04x); 0:/%s may remain.\n",
               fwup_strerror(st), st, dest_name);
    }
}

static void flush_stdout(void) {
    fflush(stdout);
    Flush(Output());
}

struct fwup_progress {
    ULONG total;
    LONG file_size;
    UWORD spinner_pos;
};

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

static void update_busy_progress(struct fwup_progress *progress) {
    static const char spinner[] = "|/-\\";

    print_progress(progress->total, progress->file_size,
                   spinner[progress->spinner_pos & 3]);
    progress->spinner_pos++;
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

int main(int argc, char *argv[]) {
    if (argc < 2 || argc > 3 || argv[1][0] == '?') {
        printf("Usage: %s <source-file> [dest-name]\n", argv[0]);
        printf("  source-file : AmigaDOS path of the file to push\n");
        printf("                (e.g. ram:BOOT.bin)\n");
        printf("  dest-name   : filename on the SD card; defaults to\n");
        printf("                source basename. Must match\n");
        printf("                [A-Za-z0-9._-]{1,64} on a flat root.\n");
        return 0;
    }

    const char *src_path  = argv[1];
    const char *dest_name = (argc == 3) ? argv[2] : basename_amigados(src_path);

    if (!validate_dest_name(dest_name))
        return 1;

    ULONG board = find_zz9000();
    if (!board) {
        printf("ERROR: ZZ9000 not found in expansion bus\n");
        return 1;
    }

    volatile UWORD *cmd_reg    = (volatile UWORD *)(board + REG_FWUP_CMD);
    volatile UWORD *len_reg    = (volatile UWORD *)(board + REG_FWUP_LEN);
    volatile UWORD *status_reg = (volatile UWORD *)(board + REG_FWUP_STATUS);
    UBYTE          *buffer     = (UBYTE *)(board + ZZ_BUFFER_OFFSET);

    if (!probe_fwup_protocol(cmd_reg, len_reg, status_reg)) {
        printf("ERROR: firmware does not support the FWUP file-push protocol\n");
        printf("       Update BOOT.bin to a firmware build with FWUP support.\n");
        return 1;
    }

    BPTR fh = Open((STRPTR)src_path, MODE_OLDFILE);
    if (!fh) {
        printf("ERROR: cannot open %s for reading\n", src_path);
        return 1;
    }

    /* Determine size via Seek (END) for a progress estimate. Not
     * strictly required by the protocol, but the user wants feedback
     * — a multi-MB BOOT.bin transfer is otherwise silent. */
    LONG file_size = -1;
    if (Seek(fh, 0, OFFSET_END) >= 0) {
        file_size = Seek(fh, 0, OFFSET_BEGINNING);
    }

    UBYTE *chunk = (UBYTE *)AllocMem(FWUP_CHUNK_BYTES, MEMF_PUBLIC);
    if (!chunk) {
        printf("ERROR: out of memory for %d-byte transfer buffer\n",
               FWUP_CHUNK_BYTES);
        Close(fh);
        return 1;
    }

    /* Stage filename + NUL in the shared buffer for the OPEN command.
     * The firmware caps reads at 256 bytes, so a sub-64 byte name is
     * safely within that. */
    size_t name_len = strlen(dest_name);
    CopyMem((UBYTE *)dest_name, buffer, name_len);
    buffer[name_len] = '\0';

    printf("ZZ9000 at 0x%08lx, pushing %s -> 0:/%s",
           (unsigned long)board, src_path, dest_name);
    if (file_size >= 0) printf(" (%ld bytes)", (long)file_size);
    printf("\n");

    struct DateStamp started;
    struct DateStamp finished;
    DateStamp(&started);

    *cmd_reg = FWUP_CMD_OPEN;
    UWORD st = poll_status(status_reg, NULL);
    if (st != FWUP_OK) {
        print_cmd_error("OPEN", st);
        FreeMem(chunk, FWUP_CHUNK_BYTES);
        Close(fh);
        return 1;
    }

    ULONG total = 0;
    LONG  n;
    int   rc = 0;
    struct fwup_progress progress;

    progress.total = 0;
    progress.file_size = file_size;
    progress.spinner_pos = 0;

    while ((n = Read(fh, chunk, FWUP_CHUNK_BYTES)) > 0) {
        CopyMem(chunk, buffer, n);
        *len_reg = (UWORD)n;
        *cmd_reg = FWUP_CMD_WRITE;
        st = poll_status(status_reg, &progress);
        if (st != FWUP_OK) {
            printf("\n");
            if (st == FWUP_STATUS_BUSY) {
                printf("ERROR: WRITE timed out at offset %lu "
                       "after %ld-byte chunk\n",
                       (unsigned long)total, (long)n);
            } else {
                printf("ERROR: WRITE at offset %lu failed: %s (0x%04x)\n",
                       (unsigned long)total, fwup_strerror(st), st);
            }
            rc = 1;
            break;
        }
        total += n;
        progress.total = total;
        print_progress(total, file_size, ' ');
    }

    if (n < 0) {
        printf("\n");
        printf("ERROR: read error on %s\n", src_path);
        rc = 1;
    }

    if (rc == 0) {
        print_progress(total, file_size, ' ');
        printf("\n");
        *cmd_reg = FWUP_CMD_CLOSE;
        st = poll_status(status_reg, NULL);
        if (st != FWUP_OK) {
            print_cmd_error("CLOSE", st);
            rc = 1;
        } else {
            DateStamp(&finished);
            print_done(total, dest_name, elapsed_ticks(&started, &finished));
        }
    }

    if (rc != 0) {
        /* Tell the firmware to delete the partially-written file
         * rather than leaving a corrupt one on the card. */
        abort_transfer(cmd_reg, status_reg, dest_name);
    }

    FreeMem(chunk, FWUP_CHUNK_BYTES);
    Close(fh);
    return rc;
}
