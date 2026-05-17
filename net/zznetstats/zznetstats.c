/*
 * zznetstats — dump SANA-II global stats for ZZ9000Net.device
 *
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Usage:
 *   zznetstats [DEVICE=<name>] [UNIT=<n>]
 *   zznetstats <name> [<unit>]
 *
 * Defaults: DEVICE=Networks/ZZ9000Net.device UNIT=0
 *
 * Prints every field of Sana2DeviceStats plus the firmware RX backlog
 * status registers. Run twice around a throughput test and diff by eye
 * to see whether the driver or firmware is dropping frames.
 */

#include <exec/types.h>
#include <exec/io.h>
#include <exec/memory.h>
#include <devices/sana2.h>
#include <proto/exec.h>
#include <proto/expansion.h>
#include <clib/alib_protos.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ZZNETSTATS_VERSION "2.1"
#define ZZNETSTATS_DATE    "17.05.2026"

static const char version[] __attribute__((used)) =
    "$VER: zznetstats " ZZNETSTATS_VERSION " (" ZZNETSTATS_DATE ")\r\n";

#define DEFAULT_DEVICE "Networks/ZZ9000Net.device"
#define DEFAULT_UNIT   0
#define MNT_MANUFACTURER  0x6d6e
#define ZZ9000_PRODUCT_Z3 4
#define ZZ9000_PRODUCT_Z2 3
#define REG_ZZ_ETH_RX_STATUS 0x8c
#define REG_ZZ_ETH_RX_STATS  0x8e

static ULONG find_zz9000(void)
{
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

static void print_firmware_rx_stats(void)
{
    ULONG regs = find_zz9000();
    if (!regs) {
        printf("FirmwareRXStatus     = unavailable\n");
        return;
    }

    UWORD status = *(volatile UWORD *)(regs + REG_ZZ_ETH_RX_STATUS);
    UWORD stats  = *(volatile UWORD *)(regs + REG_ZZ_ETH_RX_STATS);

    printf("FirmwareRXReady      = %u\n",  (unsigned)(status & 0x00ff));
    printf("FirmwareRXReserved   = %u\n",  (unsigned)((status >> 8) & 0x007f));
    printf("FirmwareRXBackpress  = %u\n",  (unsigned)((status >> 15) & 1));
    printf("FirmwareRXDropped    = %u\n",  (unsigned)((stats >> 8) & 0x00ff));
    printf("FirmwareRXPauseSent  = %u\n",  (unsigned)(stats & 0x00ff));
}

static int parse_args(int argc, char **argv, const char **dev, LONG *unit)
{
    int i;
    int positional = 0;

    for (i = 1; i < argc; i++) {
        if (strncmp(argv[i], "DEVICE=", 7) == 0) {
            *dev = argv[i] + 7;
        } else if (strncmp(argv[i], "UNIT=", 5) == 0) {
            *unit = atol(argv[i] + 5);
        } else if (argv[i][0] == '?' && argv[i][1] == 0) {
            return -1;
        } else {
            /* positional: first non-keyword = device, second = unit */
            if (positional == 0) *dev = argv[i];
            else if (positional == 1) *unit = atol(argv[i]);
            positional++;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *devname = DEFAULT_DEVICE;
    LONG        unit    = DEFAULT_UNIT;

    if (parse_args(argc, argv, &devname, &unit) < 0) {
        printf("Usage: zznetstats [DEVICE=<name>] [UNIT=<n>]\n");
        printf("  Default: DEVICE=%s UNIT=%d\n", DEFAULT_DEVICE, DEFAULT_UNIT);
        return 0;
    }

    struct MsgPort *port = CreateMsgPort();
    if (!port) {
        printf("CreateMsgPort failed\n");
        return 20;
    }

    struct IOSana2Req *req = (struct IOSana2Req *)
        CreateIORequest(port, sizeof(struct IOSana2Req));
    if (!req) {
        printf("CreateIORequest failed\n");
        DeleteMsgPort(port);
        return 20;
    }

    if (OpenDevice((CONST_STRPTR)devname, unit, (struct IORequest *)req, 0) != 0) {
        printf("OpenDevice(\"%s\", unit=%ld) failed\n", devname, (long)unit);
        DeleteIORequest((struct IORequest *)req);
        DeleteMsgPort(port);
        return 20;
    }

    struct Sana2DeviceStats stats;
    memset(&stats, 0, sizeof(stats));

    req->ios2_Req.io_Command = S2_GETGLOBALSTATS;
    req->ios2_StatData       = &stats;

    DoIO((struct IORequest *)req);

    if (req->ios2_Req.io_Error) {
        printf("S2_GETGLOBALSTATS failed: io_Error=%d ios2_WireError=%ld\n",
               (int)req->ios2_Req.io_Error, (long)req->ios2_WireError);
    } else {
        printf("device               = %s\n",  devname);
        printf("unit                 = %ld\n", (long)unit);
        printf("PacketsReceived      = %lu\n", (unsigned long)stats.PacketsReceived);
        printf("PacketsSent          = %lu\n", (unsigned long)stats.PacketsSent);
        printf("BadData              = %lu\n", (unsigned long)stats.BadData);
        printf("Overruns             = %lu\n", (unsigned long)stats.Overruns);
        printf("UnknownTypesReceived = %lu\n", (unsigned long)stats.UnknownTypesReceived);
        printf("Reconfigurations     = %lu\n", (unsigned long)stats.Reconfigurations);
        printf("LastStart.tv_secs    = %lu\n", (unsigned long)stats.LastStart.tv_secs);
        printf("LastStart.tv_micro   = %lu\n", (unsigned long)stats.LastStart.tv_micro);
        print_firmware_rx_stats();
    }

    CloseDevice((struct IORequest *)req);
    DeleteIORequest((struct IORequest *)req);
    DeleteMsgPort(port);
    return 0;
}
