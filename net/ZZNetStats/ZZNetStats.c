/*
 * ZZNetStats - dump SANA-II global stats for ZZ9000Net.device
 *
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Usage:
 *   ZZNetStats [DEVICE=<name>] [UNIT=<n>]
 *   ZZNetStats <name> [<unit>]
 *   ZZNetStats [...] MONITOR [SECS=<n>]
 *
 * Defaults: DEVICE=Networks/ZZ9000Net.device UNIT=0
 *
 * Prints every field of Sana2DeviceStats plus the firmware RX backlog
 * status registers. Run twice around a throughput test and diff by eye
 * to see whether the driver or firmware is dropping frames.
 *
 * MONITOR mode (issue #29 residual-stall probe): opens the device once and
 * loops, printing ONE compact line every SECS seconds (default 2) until
 * Ctrl-C. Launch it BEFORE starting the transfer and redirect to a local
 * (non-network) file, e.g.:
 *     ZZNetStats MONITOR >RAM:netmon.log
 * S2_GETGLOBALSTATS is a synchronous, non-network DoIO to the device, so
 * this keeps sampling even while network-dependent commands are wedged.
 * Read the log across the stall:
 *   - Rx keeps climbing            -> firmware still delivers inbound frames;
 *                                     the stack is ignoring them.
 *   - Rx frozen, FwRdy/Empty climb -> firmware stopped feeding / driver not
 *                                     consuming the backlog.
 *   - lines STOP entirely          -> the sampler task itself was descheduled
 *                                     -> a true scheduler stall, not a net block.
 */

#include <exec/types.h>
#include <exec/io.h>
#include <exec/memory.h>
#include <devices/sana2.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/expansion.h>
#include <clib/alib_protos.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zz9000_hw.h"

#define ZZNETSTATS_VERSION "2.3"
#define ZZNETSTATS_DATE    "01.07.2026"

static const char version[] __attribute__((used)) =
    "$VER: ZZNetStats " ZZNETSTATS_VERSION " (" ZZNETSTATS_DATE ")\r\n";

#define DEFAULT_DEVICE "Networks/ZZ9000Net.device"
#define DEFAULT_UNIT   0

static void print_firmware_rx_stats(void)
{
    ULONG regs = zz9000_find_board(NULL);
    if (!regs) {
        printf("FirmwareRXStatus     = unavailable\n");
        return;
    }

    UWORD status = zz9000_read_reg16(regs, ZZ_REG_ETH_RX_STATUS);
    UWORD stats  = zz9000_read_reg16(regs, ZZ_REG_ETH_RX_STATS);

    printf("FirmwareRXReady      = %u\n",  (unsigned)(status & 0x00ff));
    printf("FirmwareRXReserved   = %u\n",  (unsigned)((status >> 8) & 0x007f));
    printf("FirmwareRXBackpress  = %u\n",  (unsigned)((status >> 15) & 1));
    printf("FirmwareRXDropped    = %u\n",  (unsigned)((stats >> 8) & 0x00ff));
    printf("FirmwareRXPauseSent  = %u\n",  (unsigned)(stats & 0x00ff));
}

static int parse_args(int argc, char **argv, const char **dev, LONG *unit,
                      int *monitor, LONG *secs)
{
    int i;
    int positional = 0;

    for (i = 1; i < argc; i++) {
        if (strncmp(argv[i], "DEVICE=", 7) == 0) {
            *dev = argv[i] + 7;
        } else if (strncmp(argv[i], "UNIT=", 5) == 0) {
            *unit = atol(argv[i] + 5);
        } else if (strcmp(argv[i], "MONITOR") == 0) {
            *monitor = 1;
        } else if (strncmp(argv[i], "SECS=", 5) == 0) {
            *secs = atol(argv[i] + 5);
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

/* issue #29 ACK-probe fields pulled from S2_GETSPECIALSTATS. */
struct ackprobe {
    ULONG empty;        /* RxEmptySlot */
    ULONG p445_in;      /* inbound TCP frames from server (src 445)   */
    ULONG srv_ack;      /* server's cumulative ack of the Amiga's data */
    ULONG srv_ack_upd;  /* # times srv_ack advanced                    */
    ULONG p445_out;     /* outbound TCP frames to server (dst 445)     */
    ULONG tx_seq;       /* Amiga's last send seq                       */
    ULONG tx_seq_max;   /* highest send seq+payload the Amiga reached  */
};

/* Fetch all special-stat records once and extract the ACK-probe fields by
 * name. Missing records stay 0 (older device without the probe). */
static void fetch_ackprobe(struct IOSana2Req *req, struct ackprobe *out)
{
    UBYTE sbuf[sizeof(struct Sana2SpecialStatHeader) +
               8 * sizeof(struct Sana2SpecialStatRecord)]
        __attribute__((aligned(4)));
    struct Sana2SpecialStatHeader *h = (struct Sana2SpecialStatHeader *)sbuf;
    struct Sana2SpecialStatRecord *rec =
        (struct Sana2SpecialStatRecord *)(h + 1);
    ULONG i;

    memset(out, 0, sizeof(*out));
    memset(sbuf, 0, sizeof(sbuf));
    h->RecordCountMax        = 8;
    req->ios2_Req.io_Error   = 0;
    req->ios2_Req.io_Command = S2_GETSPECIALSTATS;
    req->ios2_StatData       = h;
    DoIO((struct IORequest *)req);
    if (req->ios2_Req.io_Error)
        return;
    for (i = 0; i < h->RecordCountSupplied; i++) {
        const char *s = rec[i].String ? (char *)rec[i].String : "";
        ULONG       c = rec[i].Count;
        if      (!strcmp(s, "RxEmptySlot")) out->empty       = c;
        else if (!strcmp(s, "P445In"))      out->p445_in     = c;
        else if (!strcmp(s, "SrvAck"))      out->srv_ack     = c;
        else if (!strcmp(s, "SrvAckUpd"))   out->srv_ack_upd = c;
        else if (!strcmp(s, "P445Out"))     out->p445_out    = c;
        else if (!strcmp(s, "TxSeq"))       out->tx_seq      = c;
        else if (!strcmp(s, "TxSeqMax"))    out->tx_seq_max  = c;
    }
}

/* MONITOR mode: sample once per `secs` seconds until Ctrl-C. One line per
 * sample, flushed immediately so a RAM: redirect captures live progress
 * even if the transfer wedges. */
static int monitor_loop(struct IOSana2Req *req, LONG secs)
{
    ULONG regs = zz9000_find_board(NULL);
    ULONG n = 0;

    if (secs < 1) secs = 1;

    printf("# ZZNetStats MONITOR (every %lds) — Ctrl-C to stop\n", (long)secs);
    printf("# ACK probe: at the stall, TxSeq < SrvAck  ==>  the stack is\n");
    printf("#   retransmitting already-acked data (send-side/stack bug),\n");
    printf("#   NOT the card dropping the ACK. P445In climbing + SrvAck\n");
    printf("#   pinned = the server's ACK is reaching the stack.\n");
    printf("# n Rx Tx Empty | P445In SrvAck SrvAckUpd P445Out TxSeq TxSeqMax | FwRdy\n");
    fflush(stdout);

    for (;;) {
        struct Sana2DeviceStats stats;
        struct ackprobe ap;
        UWORD status = 0;

        memset(&stats, 0, sizeof(stats));
        req->ios2_Req.io_Error   = 0;
        req->ios2_Req.io_Command = S2_GETGLOBALSTATS;
        req->ios2_StatData       = &stats;
        DoIO((struct IORequest *)req);
        if (req->ios2_Req.io_Error) {
            printf("#%lu S2_GETGLOBALSTATS io_Error=%d\n",
                   (unsigned long)n, (int)req->ios2_Req.io_Error);
            fflush(stdout);
        } else {
            fetch_ackprobe(req, &ap);
            if (regs)
                status = zz9000_read_reg16(regs, ZZ_REG_ETH_RX_STATUS);
            /* seq/ack numbers in hex so advance/pin is obvious at a glance. */
            printf("%lu %lu %lu %lu | %lu %08lx %lu %lu %08lx %08lx | %u\n",
                   (unsigned long)n,
                   (unsigned long)stats.PacketsReceived,
                   (unsigned long)stats.PacketsSent,
                   (unsigned long)ap.empty,
                   (unsigned long)ap.p445_in,
                   (unsigned long)ap.srv_ack,
                   (unsigned long)ap.srv_ack_upd,
                   (unsigned long)ap.p445_out,
                   (unsigned long)ap.tx_seq,
                   (unsigned long)ap.tx_seq_max,
                   (unsigned)(status & 0x00ff));
            fflush(stdout);
        }

        n++;

        /* Ctrl-C breaks out cleanly. */
        if (SetSignal(0L, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C)
            break;
        Delay((ULONG)secs * 50);   /* 50 ticks == 1 second */
        if (SetSignal(0L, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C)
            break;
    }

    printf("# stopped after %lu samples\n", (unsigned long)n);
    fflush(stdout);
    return 0;
}

int main(int argc, char **argv)
{
    const char *devname = DEFAULT_DEVICE;
    LONG        unit    = DEFAULT_UNIT;
    int         monitor = 0;
    LONG        secs    = 2;

    if (parse_args(argc, argv, &devname, &unit, &monitor, &secs) < 0) {
        printf("Usage: ZZNetStats [DEVICE=<name>] [UNIT=<n>] [MONITOR [SECS=<n>]]\n");
        printf("  Default: DEVICE=%s UNIT=%d\n", DEFAULT_DEVICE, DEFAULT_UNIT);
        printf("  MONITOR: loop one line/SECS (default 2) until Ctrl-C.\n");
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

    if (monitor) {
        int rc = monitor_loop(req, secs);
        CloseDevice((struct IORequest *)req);
        DeleteIORequest((struct IORequest *)req);
        DeleteMsgPort(port);
        return rc;
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

        /* issue #29: device special stats (RxEmptySlot — empty RX slots the
         * framer skipped instead of acking). Generic printer: shows whatever
         * records the device supplies, by name. */
        {
            /* Longword-aligned: SANA-II writes ULONG fields through the
             * header/record pointers, and m68k requires 4-byte alignment
             * for those accesses. A bare UBYTE[] only guarantees byte
             * alignment, so force it. */
            UBYTE sbuf[sizeof(struct Sana2SpecialStatHeader) +
                       8 * sizeof(struct Sana2SpecialStatRecord)]
                __attribute__((aligned(4)));
            struct Sana2SpecialStatHeader *h = (struct Sana2SpecialStatHeader *)sbuf;
            struct Sana2SpecialStatRecord *rec =
                (struct Sana2SpecialStatRecord *)(h + 1);
            ULONG i;

            memset(sbuf, 0, sizeof(sbuf));
            h->RecordCountMax        = 8;
            req->ios2_Req.io_Error   = 0;
            req->ios2_Req.io_Command = S2_GETSPECIALSTATS;
            req->ios2_StatData       = h;
            DoIO((struct IORequest *)req);
            if (!req->ios2_Req.io_Error) {
                for (i = 0; i < h->RecordCountSupplied; i++) {
                    printf("%-20s = %lu\n",
                           rec[i].String ? (char *)rec[i].String : (char *)"?",
                           (unsigned long)rec[i].Count);
                }
            }
        }
    }

    CloseDevice((struct IORequest *)req);
    DeleteIORequest((struct IORequest *)req);
    DeleteMsgPort(port);
    return 0;
}
