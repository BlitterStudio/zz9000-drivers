/*
 * ZZNetBench — raw SANA-II throughput benchmark for ZZ9000Net.device.
 *
 * Modes (single-request pipeline in every mode — one IOSana2Req
 * re-issued synchronously; see the caveat in run_rx):
 *   RX   raw-mode receive loop (staging fallback path; the direct
 *        path refuses raw requests)
 *   RXD  negotiated-hook receive through the AmiNetXDuo extension
 *        pair — the number the F3 gate reads (KTD8)
 *   TX   transmit loop — verify on a receiver (KTD8); no FPGA TX
 *        counter exists to cross-check locally
 *   BUS  32-bit MMIO read-rate constant (KTD10 yardstick)
 *
 * A run counts only when the Overrun delta is ~0 and (KTD8) a
 * sender-side rate belongs beside every RX number on the peer; this
 * tool prints the Amiga side, the procedure (U8/README) records the
 * peer side.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <exec/types.h>
#include <exec/lists.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <devices/timer.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/timer.h>

#include "../sana2.h"
#include "../anxs2ext.h"
#include "../../include/zz9000_hw.h"

#define ZZNETBENCH_VERSION "1.0"

#ifndef DEVICE
#define DEVICE "Networks/ZZ9000Net.device"
#endif

#define ZZ_BENCH_MAX_FRAME 1518
#define ZZ_BENCH_DEF_SECS  5
#define ZZ_BENCH_DEF_DEPTH 8
#define ZZ_BENCH_DEF_SIZE  1500
#define ZZ_BENCH_HOOK_SLOTS 16

/* ---- negotiated-hook RX mode state (RXD) ---------------------------- */

static UBYTE  g_hook_bufs[ZZ_BENCH_HOOK_SLOTS][ZZ_BENCH_MAX_FRAME + 16];
static volatile ULONG g_hook_bytes;
static volatile ULONG g_hook_frames;
static volatile UBYTE g_hook_flags_seen;

/* RX_DIRECT: where would `len` payload bytes land? Round-robin the
 * slot pool; the 14 reserved bytes before each buffer satisfy
 * ANXD_S2_RX_LINK_HDR when negotiated. */
static UBYTE *bench_rxdirect(APTR ios2_data, ULONG len)
{
    (void)ios2_data;
    if (len > ZZ_BENCH_MAX_FRAME) return NULL;
    return g_hook_bufs[g_hook_frames % ZZ_BENCH_HOOK_SLOTS] + 14;
}

/* RX_FILLED: len bytes written at the answered pointer. */
static void bench_rxfilled(APTR ios2_data, ULONG len, ULONG sum, UBYTE flags)
{
    (void)ios2_data; (void)sum;
    g_hook_bytes   += len;
    g_hook_frames++;
    g_hook_flags_seen |= flags;
}

/* ---- helpers --------------------------------------------------------- */

static struct MsgPort *g_port;
static struct IOSana2Req *g_ios2;
static struct timerequest *g_timer;


/* proto/timer.h declares this extern; the tool owns the definition. */
struct Device *TimerBase;

/* SANA-II requires copy hooks in the buffer-management tag list; a
 * benchmark supplies trivial memcpy variants. The driver invokes them
 * through BMFunc, whose parameters arrive in a0/a1/d0 (net/device.h) —
 * the definitions must use the same register annotations or memcpy
 * receives garbage under the m68k compiler's stack ABI. */
static BOOL bench_memcpy_to(void *dst __asm("a0"), void *src __asm("a1"),
                            LONG len __asm("d0"))
{
    memcpy(dst, src, len);
    return 1;
}

static BOOL bench_memcpy_from(void *dst __asm("a0"), void *src __asm("a1"),
                             LONG len __asm("d0"))
{
    memcpy(dst, src, len);
    return 1;
}

static int open_device(const char *name, BOOL offer_hooks)
{
    struct TagItem tags[8];
    struct TagItem *t = tags;

    if (!(g_port = CreateMsgPort())) {
        printf("no msg port\n");
        return 0;
    }
    if (!(g_ios2 = (struct IOSana2Req *)CreateIORequest(
              g_port, sizeof(struct IOSana2Req)))) {
        printf("no IORequest\n");
        return 0;
    }

    *t++ = (struct TagItem){ S2_CopyToBuff,   (Tag)bench_memcpy_to };
    *t++ = (struct TagItem){ S2_CopyFromBuff, (Tag)bench_memcpy_from };
    if (offer_hooks) {
        *t++ = (struct TagItem){ ANXD_S2_RX_DIRECT,  (Tag)bench_rxdirect };
        *t++ = (struct TagItem){ ANXD_S2_RX_FILLED,  (Tag)bench_rxfilled };
    }
    *t = (struct TagItem){ TAG_DONE, 0 };

    g_ios2->ios2_BufferManagement = tags;

    if (OpenDevice((CONST_STRPTR)name, 0, g_ios2, 0)) {
        printf("cannot open %s\n", name);
        return 0;
    }
    return 1;
}

static void close_device(void)
{
    if (g_ios2 && g_ios2->ios2_Req.io_Device) {
        CloseDevice(g_ios2);
        g_ios2->ios2_Req.io_Device = NULL;
    }
    if (g_ios2) DeleteIORequest(g_ios2);
    if (g_port) DeleteMsgPort(g_port);
    g_ios2 = NULL;
    g_port = NULL;
}

static int open_timer(void)
{
    if (!(g_timer = (struct timerequest *)CreateIORequest(
              g_port, sizeof(struct timerequest))))
        return 0;
    if (OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_MICROHZ,
                   g_timer, 0)) {
        printf("cannot open timer.device\n");
        return 0;
    }
    TimerBase = g_timer->tr_node.io_Device;
    return 1;
}

static void close_timer(void)
{
    if (g_timer && g_timer->tr_node.io_Device) {
        CloseDevice(g_timer);
        g_timer->tr_node.io_Device = NULL;
    }
    if (g_timer) DeleteIORequest(g_timer);
    g_timer = NULL;
}

/* Microseconds since start via timer.device GetSysTime (integer math:
 * doubles would pull soft-float helpers into this tiny tool). */
static ULONG elapsed_us(const struct timeval *from)
{
    struct timeval now;
    GetSysTime(&now);
    return (ULONG)(now.tv_secs - from->tv_secs) * 1000000UL +
           (ULONG)((LONG)now.tv_micro - (LONG)from->tv_micro);
}

/* Mbit/s scaled by 100, from bytes and elapsed microseconds:
 * bytes*8*100/us = (bits * 1e2) / us = Mbit/s * 100. */
static ULONG mbits_x100(ULONG bytes, ULONG us)
{
    if (us == 0) return 0;
    return (ULONG)(((unsigned long long)bytes * 800ULL) /
                   (unsigned long long)us);
}

static struct Sana2DeviceStats g_stats_before, g_stats_after;

static void snapshot_before(void)
{
    g_ios2->ios2_Req.io_Command  = S2_GETGLOBALSTATS;
    g_ios2->ios2_Req.io_Flags    = SANA2IOF_QUICK;
    g_ios2->ios2_StatData        = &g_stats_before;
    DoIO(g_ios2);
}

static void snapshot_after(void)
{
    g_ios2->ios2_Req.io_Command  = S2_GETGLOBALSTATS;
    g_ios2->ios2_Req.io_Flags    = SANA2IOF_QUICK;
    g_ios2->ios2_StatData        = &g_stats_after;
    DoIO(g_ios2);

    printf("counters: rx=%ld tx=%ld ov=%ld bad=%ld unk=%ld\n",
           (long)(g_stats_after.PacketsReceived - g_stats_before.PacketsReceived),
           (long)(g_stats_after.PacketsSent    - g_stats_before.PacketsSent),
           (long)(g_stats_after.Overruns       - g_stats_before.Overruns),
           (long)(g_stats_after.BadData        - g_stats_before.BadData),
           (long)(g_stats_after.UnknownTypesReceived - g_stats_before.UnknownTypesReceived));
    if (g_stats_after.Overruns != g_stats_before.Overruns) {
        printf("WARNING: overruns != 0 — run does NOT count (KTD8)\n");
    }
}

static void go_online(void)
{
    g_ios2->ios2_Req.io_Command = S2_ONLINE;
    g_ios2->ios2_Req.io_Flags   = SANA2IOF_QUICK;
    DoIO(g_ios2);
}

/* ---- RX (raw staging path) ------------------------------------------- */

static UBYTE g_rxbuf[ZZ_BENCH_MAX_FRAME];

static void run_rx(int secs, int depth, int hook_mode)
{
    struct timeval t0;
    int replies = 0;
    volatile ULONG bytes = 0;

    /* A real posted-read window needs one IOSana2Req per slot (one
     * struct cannot back concurrent reads). This tool keeps one
     * request and re-issues synchronously: it measures the
     * request-to-completion pipeline including BeginIO/DoIO overhead —
     * the honest caveat printed below. AmiNetXDuo's batched path is
     * deliberately NOT used here (KTD8: raw layer = driver ceiling). */

    printf("rx%s: secs=%d (single-request pipeline)\n",
           hook_mode ? "d" : "", secs);
    snapshot_before();
    GetSysTime(&t0);

    while (elapsed_us(&t0) < (ULONG)secs * 1000000UL) {
        g_ios2->ios2_Req.io_Command = CMD_READ;
        g_ios2->ios2_Req.io_Flags   = hook_mode ? 0 : SANA2IOF_RAW;
        g_ios2->ios2_PacketType     = 0x0800;
        g_ios2->ios2_Data           = g_rxbuf;
        g_ios2->ios2_DataLength     = sizeof(g_rxbuf);
        if (DoIO(g_ios2) == 0) {
            replies++;
            if (!hook_mode) {
                bytes += g_ios2->ios2_DataLength;
            }
        } else {
            break;
        }
    }

    {
        ULONG us = elapsed_us(&t0);
        ULONG counted = hook_mode ? g_hook_bytes : (ULONG)bytes;
        ULONG mb = mbits_x100(counted, us);
        printf("rx%s-result: frames=%ld bytes=%lu secs=%lu.%02lu mbits=%lu.%02lu\n",
               hook_mode ? "d" : "", (long)replies,
               (unsigned long)counted,
               (unsigned long)(us / 1000000UL),
               (unsigned long)((us / 10000UL) % 100UL),
               (unsigned long)(mb / 100UL), (unsigned long)(mb % 100UL));
    }

    snapshot_after();
}

/* ---- TX --------------------------------------------------------------- */

static UBYTE g_txbuf[ZZ_BENCH_MAX_FRAME];

static void run_tx(int secs, int size)
{
    struct timeval t0;
    ULONG frames = 0;

    if (size > ZZ_BENCH_MAX_FRAME) size = ZZ_BENCH_MAX_FRAME;
    memset(g_txbuf, 0xAA, sizeof(g_txbuf));

    snapshot_before();
    GetSysTime(&t0);

    printf("tx: size=%d secs=%d (single-request pipeline)\n", size, secs);
    while (elapsed_us(&t0) < (ULONG)secs * 1000000UL) {
        g_ios2->ios2_Req.io_Command = CMD_WRITE;
        g_ios2->ios2_Req.io_Flags   = SANA2IOF_RAW;
        g_ios2->ios2_PacketType     = 0x0800;
        g_ios2->ios2_Data           = g_txbuf;
        g_ios2->ios2_DataLength     = size;
        if (DoIO(g_ios2) != 0) {
            printf("tx error %ld wire %ld\n",
                   (long)g_ios2->ios2_Req.io_Error,
                   (long)g_ios2->ios2_WireError);
            break;
        }
        frames++;
    }

    {
        ULONG us = elapsed_us(&t0);
        ULONG mb = mbits_x100((ULONG)frames * (ULONG)size, us);
        printf("tx-result: frames=%lu secs=%lu.%02lu mbits=%lu.%02lu\n",
               (unsigned long)frames,
               (unsigned long)(us / 1000000UL),
               (unsigned long)((us / 10000UL) % 100UL),
               (unsigned long)(mb / 100UL), (unsigned long)(mb % 100UL));
        printf("NOTE (KTD8): a TX number is reportable only beside a\n"
               "matching receiver-side count on the peer.\n");
    }
}

/* ---- BUS (environment constants) -------------------------------------- */
static void run_bus(int secs)
{
    struct ZZ9000Board board;
    volatile ULONG *window;
    struct timeval t0;
    ULONG reads = 0;

    if (!zz9000_find_board(&board) || !board.address) {
        printf("bus: ZZ9000 not found\n");
        return;
    }
    window = (volatile ULONG *)(board.address + 0x2000);

    GetSysTime(&t0);
    while (elapsed_us(&t0) < (ULONG)secs * 1000000UL) {
        ULONG v = *window; /* one 32-bit MMIO read of the RX window header */
        (void)v;
        reads++;
    }
    {
        ULONG us = elapsed_us(&t0);
        ULONG mb = mbits_x100(reads * 4UL, us);
        printf("bus-read-result: reads=%lu secs=%lu.%02lu mb_per_s=%lu.%02lu "
               "(32-bit MMIO reads of the RX window header)\n",
               (unsigned long)reads,
               (unsigned long)(us / 1000000UL),
               (unsigned long)((us / 10000UL) % 100UL),
               (unsigned long)(mb / 100UL), (unsigned long)(mb % 100UL));
        printf("NOTE: worst-case single-word read rate, not the streaming "
               "rate; U7 records it as the KTD10 yardstick.\n");
    }
}

/* ---- main -------------------------------------------------------------- */

static void usage(void)
{
    printf("ZZNetBench %s — raw SANA-II throughput benchmark\n"
           "usage: ZZNetBench RX [secs] [depth] | RXD [secs] | "
           "TX [secs] [size] | BUS [secs]\n"
           "  RX  raw-mode receive (staging fallback path)\n"
           "  RXD negotiated-hook receive (direct drain; F3 gate number)\n"
           "  TX  transmit blast (verify on a receiver, KTD8)\n"
           "  BUS MMIO read bandwidth constant (KTD10 yardstick)\n",
           ZZNETBENCH_VERSION);
}

int main(int argc, char **argv)
{
    int secs = ZZ_BENCH_DEF_SECS;
    int rc = 0;

    if (argc < 2) { usage(); return 0; }

    if (argc >= 3) {
        secs = atoi(argv[2]);
        if (secs <= 0) { usage(); return 20; }
    }

    if (!strcmp(argv[1], "BUS")) {
        if (!open_device(DEVICE, FALSE)) { rc = 20; goto out; }
        if (!open_timer())               { rc = 20; goto out; }
        run_bus(secs);
        goto out;
    }

    if (!open_device(DEVICE, !strcmp(argv[1], "RXD"))) { rc = 20; goto out; }
    if (!open_timer())                                  { rc = 20; goto out; }
    go_online();

    if (!strcmp(argv[1], "RX")) {
        int depth = (argc >= 4) ? atoi(argv[3]) : ZZ_BENCH_DEF_DEPTH;
        if (depth <= 0) depth = ZZ_BENCH_DEF_DEPTH;
        run_rx(secs, depth, 0);
    } else if (!strcmp(argv[1], "RXD")) {
        run_rx(secs, 1, 1);
        printf("hook counters: bytes=%lu frames=%lu flags_seen=0x%02x "
               "(SUMMED|VERIFIED|CONTINUES bits as delivered)\n",
               (unsigned long)g_hook_bytes,
               (unsigned long)g_hook_frames, g_hook_flags_seen);
    } else if (!strcmp(argv[1], "TX")) {
        int size = (argc >= 4) ? atoi(argv[3]) : ZZ_BENCH_DEF_SIZE;
        run_tx(secs, size);
    } else {
        usage();
        rc = 20;
    }

out:
    close_timer();
    close_device();
    return rc;
}
