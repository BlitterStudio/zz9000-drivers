/*
 * ZZFwUpdate - Push a file onto the ZZ9000's FAT32 SD card from AmigaOS.
 *
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Talks to the firmware's REG_ZZ_FWUP_* protocol (firmware >= 2.1):
 *   1. stage NUL-terminated filename in shared buffer at board+0xA000
 *   2. CMD = OPEN, poll STATUS
 *   3. per chunk: stage bytes, LEN = chunk_bytes, CMD = WRITE, poll
 *   4. CMD = CLOSE (success) or CMD = ABORT (delete the partial)
 *
 * Typical use:
 *   ZZFwUpdate ram:BOOT.bin                  -> writes 0:/BOOT.bin
 *   ZZFwUpdate ram:BOOT.bin BOOT.bin         -> same, explicit dest name
 *   ZZFwUpdate sys:Storage/zz9000-2.1.0.bin BOOT.bin
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

#define MNT_MANUFACTURER   0x6d6e
#define ZZ9000_PRODUCT_Z3  4
#define ZZ9000_PRODUCT_Z2  3

/* Mirrors zz9000-firmware/ZZ9000_proto.sdk/ZZ9000OS/src/zz_regs.h. */
#define REG_FWUP_CMD       0xCA
#define REG_FWUP_LEN       0xCC
#define REG_FWUP_STATUS    0xCE
#define REG_FW_VERSION     0xC0
#define ZZ_BUFFER_OFFSET   0xA000

#define FWUP_CMD_OPEN      1
#define FWUP_CMD_WRITE     2
#define FWUP_CMD_CLOSE     3
#define FWUP_CMD_ABORT     4

#define FWUP_STATUS_BUSY   0xFFFF
#define FWUP_OK            0x0000

/* Stays well under the firmware's FWUP_MAX_CHUNK (24576) and matches
 * the chunk size the SD-boot path uses, so we share the same buffer
 * residency story. */
#define FWUP_CHUNK_BYTES   16384
#define FWUP_REQUIRED_FW_MAJOR 2
#define FWUP_REQUIRED_FW_MINOR 1

/* Roughly matches SD-boot's poll budget — each FatFs write can stall
 * for tens of ms on a slow SD card, so a small counter wouldn't
 * survive a real card under load. */
#define FWUP_POLL_LIMIT    2000000

static const char *fwup_strerror(UWORD code) {
    switch (code) {
    case 0x00: return "OK";
    case 0x02: return "no SD card / FAT not mounted on firmware";
    case 0x03: return "bad filename (flat-root, [A-Za-z0-9._-], <=64 chars)";
    case 0x04: return "firmware f_open failed";
    case 0x05: return "firmware f_write failed";
    case 0x06: return "firmware f_close/f_sync failed";
    case 0x07: return "protocol state error (WRITE/CLOSE without OPEN?)";
    case 0x08: return "chunk length out of range";
    case 0x09: return "unknown FWUP command";
    default:   return "unknown error";
    }
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

static UWORD poll_status(volatile UWORD *status_reg) {
    /* The firmware sets STATUS = 0xFFFF when it accepts a CMD and
     * clears it to the result once the FatFs call returns. Status
     * register sits in the cache-inhibit Zorro register window, so no
     * cache management is needed on the m68k side. */
    ULONG budget = FWUP_POLL_LIMIT;
    UWORD st;
    do {
        st = *status_reg;
        if (st != FWUP_STATUS_BUSY) return st;
    } while (--budget);
    return FWUP_STATUS_BUSY;
}

static int firmware_supports_fwup(UWORD fwrev) {
    UWORD major = fwrev >> 8;
    UWORD minor = fwrev & 0xff;

    return (major > FWUP_REQUIRED_FW_MAJOR) ||
           (major == FWUP_REQUIRED_FW_MAJOR &&
            minor >= FWUP_REQUIRED_FW_MINOR);
}

static void abort_transfer(volatile UWORD *cmd_reg,
                           volatile UWORD *status_reg,
                           const char *dest_name) {
    UWORD st;

    *cmd_reg = FWUP_CMD_ABORT;
    st = poll_status(status_reg);
    if (st == FWUP_OK) {
        printf("Partial transfer aborted; 0:/%s deleted on the card.\n",
               dest_name);
    } else {
        printf("WARNING: ABORT failed: %s (0x%04x); 0:/%s may remain.\n",
               fwup_strerror(st), st, dest_name);
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

    if (strlen(dest_name) == 0 || strlen(dest_name) > 64) {
        printf("ERROR: destination name must be 1..64 chars\n");
        return 1;
    }

    ULONG board = find_zz9000();
    if (!board) {
        printf("ERROR: ZZ9000 not found in expansion bus\n");
        return 1;
    }

    volatile UWORD *fw_version_reg = (volatile UWORD *)(board + REG_FW_VERSION);
    UWORD fwrev = *fw_version_reg;
    if (!firmware_supports_fwup(fwrev)) {
        printf("ERROR: firmware %u.%u does not support ZZFwUpdate; "
               "requires %u.%u or later\n",
               (unsigned)(fwrev >> 8), (unsigned)(fwrev & 0xff),
               FWUP_REQUIRED_FW_MAJOR, FWUP_REQUIRED_FW_MINOR);
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

    volatile UWORD *cmd_reg    = (volatile UWORD *)(board + REG_FWUP_CMD);
    volatile UWORD *len_reg    = (volatile UWORD *)(board + REG_FWUP_LEN);
    volatile UWORD *status_reg = (volatile UWORD *)(board + REG_FWUP_STATUS);
    UBYTE          *buffer     = (UBYTE *)(board + ZZ_BUFFER_OFFSET);

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

    *cmd_reg = FWUP_CMD_OPEN;
    UWORD st = poll_status(status_reg);
    if (st != FWUP_OK) {
        printf("ERROR: OPEN failed: %s (0x%04x)\n", fwup_strerror(st), st);
        FreeMem(chunk, FWUP_CHUNK_BYTES);
        Close(fh);
        return 1;
    }

    ULONG total = 0;
    LONG  n;
    int   rc = 0;

    while ((n = Read(fh, chunk, FWUP_CHUNK_BYTES)) > 0) {
        CopyMem(chunk, buffer, n);
        *len_reg = (UWORD)n;
        *cmd_reg = FWUP_CMD_WRITE;
        st = poll_status(status_reg);
        if (st != FWUP_OK) {
            printf("\nERROR: WRITE at offset %lu failed: %s (0x%04x)\n",
                   (unsigned long)total, fwup_strerror(st), st);
            rc = 1;
            break;
        }
        total += n;
        if (file_size > 0) {
            printf("\r  %lu / %ld bytes (%lu%%)",
                   (unsigned long)total, (long)file_size,
                   (unsigned long)((total * 100UL) / (ULONG)file_size));
        } else {
            printf("\r  %lu bytes", (unsigned long)total);
        }
        Flush(Output());
    }
    printf("\n");

    if (n < 0) {
        printf("ERROR: read error on %s\n", src_path);
        rc = 1;
    }

    if (rc == 0) {
        *cmd_reg = FWUP_CMD_CLOSE;
        st = poll_status(status_reg);
        if (st != FWUP_OK) {
            printf("ERROR: CLOSE failed: %s (0x%04x)\n", fwup_strerror(st), st);
            rc = 1;
        } else {
            printf("Done. %lu bytes written to 0:/%s\n",
                   (unsigned long)total, dest_name);
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
