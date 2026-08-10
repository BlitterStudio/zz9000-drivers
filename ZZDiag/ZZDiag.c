/*
 * ZZDiag - consolidated ZZ9000 diagnostics.
 *
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Usage:
 *   ZZDiag [samples] [delay_ticks] [capture.ppm]
 *
 * VideoCap decoding is adapted from the ZZVCapDiag branch used during
 * Video Toaster/genlock investigation.
 */

#include <exec/types.h>
#include <proto/dos.h>

#include <stdio.h>
#include <stdlib.h>

#include "zz9000_hw.h"

#define ZZDIAG_VERSION "1.4"
#define ZZDIAG_DATE    "10.08.2026"

#define ZZDIAG_CAPTURE_ROWS 320U

static const char zzdiag_capture_ppm_header[] =
    "P6\n1280 320\n255\n";

static const char version[] __attribute__((used)) =
    "$VER: ZZDiag " ZZDIAG_VERSION " (" ZZDIAG_DATE ")\r\n";

static void print_version_word(const char *name, UWORD raw)
{
    printf("%-24s = %u.%u (0x%04x)\n",
        name, (unsigned)((raw >> 8) & 0xff), (unsigned)(raw & 0xff),
        (unsigned)raw);
}

static void print_reg(const char *name, ULONG board_addr, ULONG offset)
{
    printf("%-24s = 0x%04x\n", name,
        (unsigned)zz9000_read_reg16(board_addr, offset));
}

static void print_eth_stats(ULONG board_addr)
{
    UWORD status = zz9000_read_reg16(board_addr, ZZ_REG_ETH_RX_STATUS);
    UWORD stats = zz9000_read_reg16(board_addr, ZZ_REG_ETH_RX_STATS);

    printf("EthernetRXReady        = %u\n", (unsigned)(status & 0x00ff));
    printf("EthernetRXReserved     = %u\n", (unsigned)((status >> 8) & 0x007f));
    printf("EthernetRXBackpress    = %u\n", (unsigned)((status >> 15) & 1));
    printf("EthernetRXDropped      = %u\n", (unsigned)((stats >> 8) & 0x00ff));
    printf("EthernetRXPauseSent    = %u\n", (unsigned)(stats & 0x00ff));
}

static void print_scanlines(ULONG board_addr)
{
    UWORD mode = zz9000_read_reg16(board_addr, ZZ_SCANLINE_MODE_REG) & 3;
    UWORD parity = zz9000_read_reg16(board_addr, ZZ_SCANLINE_PARITY_REG) & 1;
    const char *mode_name = "unknown";

    switch (mode) {
    case 0: mode_name = "off"; break;
    case 1: mode_name = "classic"; break;
    case 2: mode_name = "soft"; break;
    case 3: mode_name = "gradient"; break;
    }

    printf("ScanlineMode           = %u (%s)\n", (unsigned)mode, mode_name);
    printf("ScanlineParity         = %u (%s dark)\n", (unsigned)parity,
        parity ? "even" : "odd");
}

static void print_videocap(ULONG board_addr)
{
    UWORD vcap = zz9000_read_reg16(board_addr, ZZ_REG_VIDEOCAP_STATS);
    UWORD lines = vcap & ZZ_VCAP_LINES_MASK;
    UWORD hsmax = (vcap >> ZZ_VCAP_PW_MAX_TIER_SHIFT) & 3;
    UWORD hsmin = (vcap >> ZZ_VCAP_PW_MIN_TIER_SHIFT) & 3;
    UWORD edge = (vcap >> ZZ_VCAP_EDGE_SHIFT) & 1;
    UWORD interlace = (vcap >> ZZ_VCAP_INTERLACE_SHIFT) & 1;
    UWORD magic = zz9000_read_reg16(board_addr, ZZ_REG_VIDEOCAP_DIAG_MAGIC);

    printf("VideoCapRaw            = 0x%04x\n", (unsigned)vcap);
    printf("VideoCapLines          = %u\n", (unsigned)lines);
    printf("VideoCapHSyncMaxTier   = %u\n", (unsigned)hsmax);
    printf("VideoCapHSyncMinTier   = %u\n", (unsigned)hsmin);
    printf("VideoCapEdge           = %u\n", (unsigned)edge);
    printf("VideoCapInterlace      = %u\n", (unsigned)interlace);

    if (magic == ZZ_VCAP_DIAG_MAGIC) {
        UWORD flags = zz9000_read_reg16(board_addr, ZZ_REG_VIDEOCAP_DIAG_FLAGS);
        printf("VideoCapDiagMagic      = 0x%04x\n", (unsigned)magic);
        printf("VideoCapDiagVersion    = %u\n",
            (unsigned)((flags >> ZZ_VCAP_DIAG_VERSION_SHIFT) &
                       ZZ_VCAP_DIAG_VERSION_MASK));
        printf("VideoCapDiagFlags      = 0x%04x\n", (unsigned)flags);
        printf("VideoCapY3Max          = %u\n",
            (unsigned)zz9000_read_reg16(board_addr, ZZ_REG_VIDEOCAP_DIAG_Y3MAX));
        printf("VideoCapYSyncMax       = %u\n",
            (unsigned)zz9000_read_reg16(board_addr, ZZ_REG_VIDEOCAP_DIAG_YSYNC_MAX));
        printf("VideoCapXLen           = %u\n",
            (unsigned)zz9000_read_reg16(board_addr, ZZ_REG_VIDEOCAP_DIAG_XLEN));
        printf("VideoCapPhase          = %u\n",
            (unsigned)zz9000_read_reg16(board_addr, ZZ_REG_VIDEOCAP_DIAG_PHASE));
        printf("VideoCapRiseLines      = %u\n",
            (unsigned)zz9000_read_reg16(board_addr, ZZ_REG_VIDEOCAP_DIAG_RISELINES));
        printf("VideoCapFallLines      = %u\n",
            (unsigned)zz9000_read_reg16(board_addr, ZZ_REG_VIDEOCAP_DIAG_FALLLINES));
    } else {
        printf("VideoCapDiagMagic      = unsupported (0x%04x)\n", (unsigned)magic);
    }
}

static ULONG read_reg32(ULONG board_addr, ULONG offset)
{
    ULONG high = zz9000_read_reg16(board_addr, offset);
    ULONG low = zz9000_read_reg16(board_addr, offset + 2UL);

    return (high << 16) | low;
}

static int arm_videocap_probe(ULONG board_addr)
{
    UWORD magic = zz9000_read_reg16(board_addr, ZZ_REG_VCAP_PROBE_META);
    unsigned i;

    if (magic != ZZ_VCAP_PROBE_MAGIC) {
        printf("VideoCapProbe          = unsupported (0x%04x)\n",
            (unsigned)magic);
        return 0;
    }

    zz9000_write_reg16(board_addr, ZZ_REG_VCAP_PROBE_CONTROL, 1);
    for (i = 0; i < 25U; i++) {
        UWORD control = zz9000_read_reg16(board_addr,
            ZZ_REG_VCAP_PROBE_CONTROL + 2UL);

        if (((control >> 1) & 1U) == (control & 1U))
            return 1;
        Delay(1);
    }
    printf("VideoCapProbe          = re-arm acknowledgement timed out\n");
    return 0;
}

/* The capture AXI master writes 0x00RRGGBB. The big-endian Amiga sees that
 * little-endian DDR word as 0xBBGGRR00, so normalize the aperture value back
 * to the AXI representation before making the decisive word-for-word test. */
static ULONG normalize_capture_word(ULONG host_word)
{
    return ((host_word & 0x0000ff00UL) << 8) |
           ((host_word & 0x00ff0000UL) >> 8) |
           ((host_word & 0xff000000UL) >> 24);
}

static void print_videocap_probe_comparison(const struct ZZ9000Board *board)
{
    volatile const ULONG *capture;
    UWORD flags = 0;
    ULONG target;
    ULONG awaddr;
    unsigned matched = 0;
    unsigned i;

    for (i = 0; i < 25U; i++) {
        flags = zz9000_read_reg16(board->address,
            ZZ_REG_VCAP_PROBE_META + 2UL);
        if (flags & ZZ_VCAP_PROBE_VALID)
            break;
        Delay(1);
    }

    printf("VideoCapProbeFlags     = 0x%04x\n", (unsigned)flags);
    if (!(flags & ZZ_VCAP_PROBE_VALID)) {
        printf("VideoCapProbe          = timed out waiting for target burst\n");
        return;
    }

    target = read_reg32(board->address, ZZ_REG_VCAP_PROBE_TARGET);
    awaddr = read_reg32(board->address, ZZ_REG_VCAP_PROBE_AWADDR);
    printf("VideoCapProbeTarget    = line %lu, x %lu\n",
        (unsigned long)(target >> 16),
        (unsigned long)(target & 0xffffUL));
    printf("VideoCapProbeAWAddr    = 0x%08lx\n", (unsigned long)awaddr);

    capture = (volatile const ULONG *)(board->address +
        ZZ_VIDEOCAP_BOARD_OFFSET);
    for (i = 0; i < ZZ_VCAP_PROBE_WORDS; i++) {
        ULONG axi_word = read_reg32(board->address,
            ZZ_REG_VCAP_PROBE_DATA_BASE + (ULONG)i * 4UL);
        ULONG ddr_word = normalize_capture_word(capture[
            ZZ_VCAP_PROBE_TARGET_LINE * ZZ_VIDEOCAP_FULL_WIDTH +
            ZZ_VCAP_PROBE_TARGET_X + i]);
        const char *result = "DIFF";

        if (axi_word == ddr_word) {
            matched++;
            result = "MATCH";
        }
        printf("VideoCapProbe[%2u]     = AXI 0x%08lx DDR 0x%08lx %s\n",
            i, (unsigned long)axi_word, (unsigned long)ddr_word, result);
    }

    printf("VideoCapProbeMatch     = %u/%u\n", matched,
        (unsigned)ZZ_VCAP_PROBE_WORDS);
    if (matched == ZZ_VCAP_PROBE_WORDS)
        printf("VideoCapProbeResult    = seam data existed before DDR\n");
    else
        printf("VideoCapProbeResult    = DDR differs from accepted AXI data\n");
}

static void dump_sample(ULONG board_addr, int sample)
{
    UWORD hw = zz9000_read_reg16(board_addr, ZZ_REG_HW_VERSION);
    UWORD fw = zz9000_read_reg16(board_addr, ZZ_REG_FW_VERSION);
    UWORD audio = zz9000_read_reg16(board_addr, ZZ_REG_AUDIO_CONFIG);

    printf("\n[Sample %d]\n", sample);
    print_version_word("HardwareVersion", hw);
    print_version_word("FirmwareVersion", fw);
    print_reg("Config", board_addr, ZZ_REG_CONFIG);
    print_reg("Mode", board_addr, ZZ_REG_MODE);
    print_reg("AudioConfig", board_addr, ZZ_REG_AUDIO_CONFIG);
    printf("AXPresent              = %u\n", (unsigned)(audio & 1));
    print_reg("TemperatureRaw", board_addr, ZZ_REG_TEMPERATURE);
    print_reg("VoltageAuxRaw", board_addr, ZZ_REG_VOLTAGE_AUX);
    print_reg("VoltageIntRaw", board_addr, ZZ_REG_VOLTAGE_INT);
    print_reg("USBStatus", board_addr, ZZ_REG_USB_STATUS);
    print_reg("USBCapacity", board_addr, ZZ_REG_USB_CAPACITY);
    print_reg("USBProxyCommand", board_addr, ZZ_REG_USB_PROXY_CMD);
    print_reg("SDStatus", board_addr, ZZ_REG_SD_STATUS);
    print_reg("SDBootStatus", board_addr, ZZ_REG_SD_BOOT_STATUS);
    print_reg("SDCapacity", board_addr, ZZ_REG_SD_CAPACITY);
    print_scanlines(board_addr);
    print_eth_stats(board_addr);
    print_videocap(board_addr);
}

/* Dump the captured source rows before formatter scaling. A P6 PPM keeps the
 * diagnostic dependency-free and preserves every 24-bit capture sample.
 * Zorro III presents the little-endian DDR word 0x00RRGGBB to the big-endian
 * host as 0xBBGGRR00 (Picasso96 B8G8R8A8), hence the byte extraction below. */
static int dump_videocap_ppm(const struct ZZ9000Board *board, const char *path)
{
    const ULONG row_bytes = ZZ_VIDEOCAP_FULL_WIDTH * 3UL;
    volatile const ULONG *src;
    UBYTE *row;
    BPTR file;
    ULONG x, y;
    LONG seek_result;
    LONG final_pos;
    int ok = 1;

    if (board->zorro_version != 3) {
        printf("ERROR: capture dump requires the Zorro III aperture\n");
        return 0;
    }

    /* Some AmigaDOS handlers preserve an existing file's tail when it is
     * reopened with MODE_NEWFILE. Remove it first so a shorter retry cannot
     * masquerade as a giant PPM. A missing file is harmless here. */
    DeleteFile((CONST_STRPTR)path);
    file = Open((CONST_STRPTR)path, MODE_NEWFILE);
    if (!file) {
        printf("ERROR: cannot create %s (IoErr=%ld)\n", path, (long)IoErr());
        return 0;
    }

    row = AllocMem(row_bytes, MEMF_PUBLIC);
    if (!row) {
        printf("ERROR: cannot allocate capture row buffer\n");
        Close(file);
        return 0;
    }

    /* Keep this header literal. The no-ixemul Amiga formatter used by this
     * tool interpreted the two %u arguments with legacy word semantics and
     * produced "0 1280" instead of "1280 320" on real hardware. */
    if (Write(file, (APTR)zzdiag_capture_ppm_header,
            (LONG)(sizeof(zzdiag_capture_ppm_header) - 1)) !=
            (LONG)(sizeof(zzdiag_capture_ppm_header) - 1))
        ok = 0;

    src = (volatile const ULONG *)(board->address + ZZ_VIDEOCAP_BOARD_OFFSET);
    for (y = 0; ok && y < ZZDIAG_CAPTURE_ROWS; y++) {
        for (x = 0; x < ZZ_VIDEOCAP_FULL_WIDTH; x++) {
            ULONG pixel = src[y * ZZ_VIDEOCAP_FULL_WIDTH + x];
            row[x * 3UL + 0] = (UBYTE)(pixel >> 8);  /* red */
            row[x * 3UL + 1] = (UBYTE)(pixel >> 16); /* green */
            row[x * 3UL + 2] = (UBYTE)(pixel >> 24); /* blue */
        }
        if (Write(file, row, row_bytes) != (LONG)row_bytes)
            ok = 0;
    }

    seek_result = Seek(file, 0, OFFSET_END);
    final_pos = Seek(file, 0, OFFSET_CURRENT);
    if (seek_result == -1 || final_pos == -1)
        ok = 0;
    if (final_pos != (LONG)((sizeof(zzdiag_capture_ppm_header) - 1) +
            ZZDIAG_CAPTURE_ROWS * row_bytes))
        ok = 0;

    FreeMem(row, row_bytes);
    Close(file);

    if (!ok) {
        printf("ERROR: capture dump write failed (IoErr=%ld)\n", (long)IoErr());
        return 0;
    }

    printf("CapturePPM             = %s (1280x320, %ld bytes)\n", path,
        (long)final_pos);
    return 1;
}

static void usage(const char *name)
{
    printf("Usage: %s [samples] [delay_ticks] [capture.ppm]\n", name);
    printf("  samples     : number of dumps, default 1\n");
    printf("  delay_ticks : ticks between samples and before capture, default 50\n");
    printf("  capture.ppm : optional 1280x320 native-capture framebuffer dump\n");
}

int main(int argc, char **argv)
{
    struct ZZ9000Board board;
    int samples = 1;
    int delay_ticks = 50;
    const char *capture_path = NULL;
    int i;

    if (argc > 4 || (argc > 1 && argv[1][0] == '?')) {
        usage(argv[0]);
        return 0;
    }

    if (argc > 1) samples = atoi(argv[1]);
    if (argc > 2) delay_ticks = atoi(argv[2]);
    if (argc > 3) capture_path = argv[3];
    if (samples < 1) samples = 1;
    if (delay_ticks < 0) delay_ticks = 0;

    if (!zz9000_find_board(&board)) {
        printf("ERROR: ZZ9000 not found\n");
        return 20;
    }

    printf("ZZ9000 diagnostics\n");
    printf("BoardAddress           = 0x%08lx\n", (unsigned long)board.address);
    printf("ZorroVersion           = %u\n", (unsigned)board.zorro_version);
    printf("Product                = 0x%04x\n", (unsigned)board.product);
    printf("Samples                = %d\n", samples);
    printf("DelayTicks             = %d\n", delay_ticks);

    for (i = 0; i < samples; i++) {
        dump_sample(board.address, i + 1);
        if (i + 1 < samples && delay_ticks > 0) Delay(delay_ticks);
    }

    if (capture_path) {
        if (delay_ticks > 0) {
            printf("CaptureDelayTicks      = %d (switch to native screen now)\n",
                delay_ticks);
            Delay(delay_ticks);
        }
        if (arm_videocap_probe(board.address))
            print_videocap_probe_comparison(&board);
        if (!dump_videocap_ppm(&board, capture_path))
            return 20;
    }

    return 0;
}
