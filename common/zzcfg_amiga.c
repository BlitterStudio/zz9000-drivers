/*
 * Amiga-side client for the ZZ9000.CFG SD-card config file.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <exec/types.h>
#include <stdio.h>
#include <string.h>

#include "zz9000_hw.h"
#include "fwup_client.h"
#include "fwup_amiga.h"
#include "zzcfg_amiga.h"

/* Matches the FWUP client's poll budget: a raw read is one small FatFs
 * read, but a slow card under load can still stall for tens of ms. */
#define ZZCFG_POLL_LIMIT 2000000UL

static volatile UWORD *reg16(ULONG board, ULONG offset)
{
    return (volatile UWORD *)(board + offset);
}

UWORD zzcfg_read_raw(ULONG board, char *out, UWORD maxlen, UWORD *outlen)
{
    volatile UBYTE *buf = (volatile UBYTE *)(board + ZZ_BUFFER_OFFSET);
    unsigned long budget = ZZCFG_POLL_LIMIT;
    UWORD status, len, i;

    *outlen = 0;
    if (maxlen == 0) return ZZ_CFG_FILE_IO_ERROR;
    out[0] = '\0';

    /* Reset the handshake, then issue the read. The firmware processes
     * register writes in bus order, so no wait is needed in between. */
    *reg16(board, ZZ_REG_CONFIG_FILE) = 0;
    *reg16(board, ZZ_REG_CONFIG_FILE) = 1;

    do {
        status = *reg16(board, ZZ_REG_CONFIG_FILE);
        if (status != ZZ_CFG_FILE_IDLE) break;
    } while (--budget);

    if (status != ZZ_CFG_FILE_OK) return status;

    len = *reg16(board, ZZ_REG_CONFIG_FILE_LEN);
    if (len > maxlen - 1) len = maxlen - 1;

    for (i = 0; i < len; i++) {
        out[i] = (char)buf[i];
    }
    out[len] = '\0';
    *outlen = len;
    return ZZ_CFG_FILE_OK;
}

static char zzcfg_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static int zzcfg_is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

int zzcfg_extract_hdf(const char *text, UWORD len, char *out, UWORD outsz)
{
    UWORD pos = 0;

    while (pos < len) {
        const char *p;
        UWORD line_end = pos;
        UWORD n;

        while (line_end < len && text[line_end] != '\n') line_end++;

        /* trim leading whitespace */
        while (pos < line_end && zzcfg_is_space(text[pos])) pos++;
        p = &text[pos];

        if (line_end - pos > 4 &&
                zzcfg_lower(p[0]) == 'h' &&
                zzcfg_lower(p[1]) == 'd' &&
                zzcfg_lower(p[2]) == 'f') {
            UWORD v = pos + 3;
            while (v < line_end && zzcfg_is_space(text[v])) v++;
            if (v < line_end && text[v] == '=') {
                v++;
                while (v < line_end && zzcfg_is_space(text[v])) v++;
                n = 0;
                while (v < line_end && n < outsz - 1 &&
                        text[v] != '#' && text[v] != ';' &&
                        !zzcfg_is_space(text[v])) {
                    out[n++] = text[v++];
                }
                out[n] = '\0';
                if (n > 0) return 1;
            }
        }

        pos = line_end + 1;
    }

    out[0] = '\0';
    return 0;
}

UWORD zzcfg_generate(const struct zzcfg_values *v, char *out, UWORD outsz)
{
    static const char *vsync_names[] = { "off", "pal", "ntsc" };
    UWORD vsync = (v->vsync <= 2) ? v->vsync : 0;
    int n;

    n = snprintf(out, outsz,
        "# ZZ9000.CFG - ZZ9000 firmware configuration file\n"
        "# Written by ZZTop. Read once at cold boot (power-on);\n"
        "# soft resets do not re-read it. See the firmware manual\n"
        "# for all options: https://github.com/BlitterStudio/zz9000-firmware\n"
        "\n"
        "# HDMI output for Amiga native video: 800x600 or pal (720x576 50Hz)\n"
        "videocap_mode = %s\n"
        "\n"
        "# Match the exact Amiga chipset refresh rate: off, pal or ntsc\n"
        "nonstandard_vsync = %s\n"
        "\n"
        "# Scanlines: 0=off 1=classic 2=soft 3=gradient; parity 0/1\n"
        "scanline_mode = %u\n"
        "scanline_parity = %u\n"
        "\n"
        "# on = drivers use INT2 instead of INT6 (replaces ENV:ZZ9K_INT2)\n"
        "int2 = %s\n"
        "\n"
        "# Ethernet MAC override (replaces ENV:ZZ9K_MAC)\n"
        "%smac = %s\n"
        "\n"
        "# SD-card boot HDF image in the root of the card (default zz9000.hdf)\n"
        "%shdf = %s\n",
        v->videocap_pal ? "pal" : "800x600",
        vsync_names[vsync],
        (unsigned)(v->scanline_mode & 3),
        (unsigned)(v->scanline_parity & 1),
        v->int2 ? "on" : "off",
        v->mac[0] ? "" : "#", v->mac[0] ? v->mac : "68:82:F2:00:00:01",
        v->hdf[0] ? "" : "#", v->hdf[0] ? v->hdf : "zz9000.hdf");

    if (n < 0) return 0;
    if ((UWORD)n >= outsz) return (UWORD)(outsz - 1);
    return (UWORD)n;
}

UWORD zzcfg_save(ULONG board, const struct zzcfg_values *v)
{
    static char text[ZZCFG_MAX_SIZE];
    struct fwup_io io;
    UWORD len, st;

    len = zzcfg_generate(v, text, sizeof(text));
    if (len == 0) return FWUP_ERR_UNKNOWN;

    fwup_io_init_board(&io, board);

    st = (UWORD)fwup_open(&io, "ZZ9000.CFG");
    if (st != FWUP_OK) return st;

    st = (UWORD)fwup_write_chunk(&io, text, len);
    if (st != FWUP_OK) {
        fwup_abort(&io);
        return st;
    }

    st = (UWORD)fwup_close(&io);
    if (st != FWUP_OK) fwup_abort(&io);
    return st;
}
