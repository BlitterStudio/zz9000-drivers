/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * ZZ9000 Poseidon USB Hardware Driver (zzusbhw.device)
 *
 * Copyright (C) 2026 Dimitris Panokostas <midwan@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <exec/resident.h>
#include <exec/errors.h>
#include <exec/memory.h>
#include <exec/lists.h>
#include <exec/tasks.h>
#include <exec/io.h>
#include <exec/execbase.h>

#include <libraries/expansion.h>

#include <devices/usbhardware.h>

#include <proto/exec.h>
#include <proto/expansion.h>

#include <stdint.h>
#include <string.h>

#include "zzusbhw.h"

struct ExecBase* SysBase;

#define STR(s) #s
#define XSTR(s) STR(s)

/*
 * lib_IdString format matches the canonical Amiga pattern
 *   "name version.revision (date) description"
 * so tools like `version` and Poseidon's internal identification
 * routines parse it consistently.
 */
#define DEVICE_ID_STRING DEVICE_NAME " " XSTR(DEVICE_VERSION) "." XSTR(DEVICE_REVISION) \
    " (18.4.2026) Poseidon USB driver for ZZ9000 " \
    "(C) Copyright 2026 Dimitris Panokostas"

/* USB request constants (from usb.h) */
#define USR_GET_STATUS        0x00
#define USR_CLEAR_FEATURE     0x01
#define USR_SET_FEATURE       0x03
#define USR_GET_DESCRIPTOR    0x06
#define USR_SET_ADDRESS       0x05
#define USR_SET_CONFIGURATION 0x09

#define URTF_IN               0x80
#define URTF_STANDARD         0x00
#define URTF_CLASS            0x20
#define URTF_DEVICE           0x00
#define URTF_OTHER            0x03

#define UDT_DEVICE            0x01
#define UDT_CONFIGURATION     0x02
#define UDT_STRING            0x03
#define UDT_HUB               0x29

#define UPSF_PORT_CONNECTION  0x0100
#define UPSF_PORT_ENABLE      0x0200
#define UPSF_PORT_SUSPEND     0x0400
#define UPSF_PORT_OVER_CURRENT 0x0800
#define UPSF_PORT_RESET       0x1000
#define UPSF_PORT_POWER       0x0001
#define UPSF_PORT_HIGH_SPEED  0x0004

#define UFS_PORT_POWER        0x08
#define UFS_PORT_RESET        0x04
#define UFS_PORT_ENABLE       0x01
#define UFS_PORT_SUSPEND      0x02
#define UFS_C_PORT_CONNECTION 0x10
#define UFS_C_PORT_ENABLE     0x11
#define UFS_C_PORT_SUSPEND    0x12
#define UFS_C_PORT_OVER_CURRENT 0x13
#define UFS_C_PORT_RESET      0x14

/* wPortChange bits use same UPSF_ layout as wPortStatus (byte-swapped USB spec). */
#define UPSF_C_PORT_CONNECTION 0x0100
#define UPSF_C_PORT_ENABLE     0x0200
#define UPSF_C_PORT_SUSPEND    0x0400
#define UPSF_C_PORT_OVER_CURRENT 0x0800
#define UPSF_C_PORT_RESET      0x1000

#define SWAP16(x) ((uint16_t)((uint16_t)(x) << 8) | ((uint16_t)(x) >> 8))

static struct ZZUSBBase *PollBase;

static void hotplug_poll_task(void);

asm("romtag:                                \n"
    "       dc.w    "XSTR(RTC_MATCHWORD)"   \n"
    "       dc.l    romtag                  \n"
    "       dc.l    endcode                 \n"
    "       dc.b    "XSTR(RTF_AUTOINIT)"    \n"
    "       dc.b    "XSTR(DEVICE_VERSION)"  \n"
    "       dc.b    "XSTR(NT_DEVICE)"       \n"
    "       dc.b    0                       \n"
    "       dc.l    _device_name            \n"
    "       dc.l    _device_id_string       \n"
    "       dc.l    _auto_init_tables       \n"
    "endcode:                               \n");

int __attribute__((no_reorder)) _start()
{
    return -1;
}

const char device_name[] = DEVICE_NAME;
const char device_id_string[] = DEVICE_ID_STRING;

/*
 * AmigaOS `version DEVS:zzusbhw.device FILE` scans the binary for a
 * `$VER:` tag and prints the version/revision that follows. Without
 * the tag, `version` falls back to a generic "v1.0" which defeats
 * field verification of driver deployment. Include a (date) after
 * the version number per standard Amiga $VER convention — some
 * tools require it to parse the version correctly.
 */
static const char __attribute__((used)) version_tag[] =
    "$VER: " DEVICE_NAME " " XSTR(DEVICE_VERSION) "." XSTR(DEVICE_REVISION)
    " (18.4.2026) Poseidon USB driver for ZZ9000 "
    "(C) Copyright 2026 Dimitris Panokostas";

/* Push a NUL-terminated string to the ZZ9000 serial debug channel. */
static void dstr(void* regs, char* str)
{
    while (*str) {
        *((volatile uint16_t*)((uint8_t*)regs + 0xF0)) = *str++;
    }
}

/* Push a single hex nibble / byte / longword to serial. */
static void dhex4(void* regs, uint32_t v)
{
    const char hex[] = "0123456789ABCDEF";
    *((volatile uint16_t*)((uint8_t*)regs + 0xF0)) = hex[v & 0xF];
}
static void dhex8(void* regs, uint32_t v)
{
    dhex4(regs, v >> 4);
    dhex4(regs, v);
}
static void dhex32(void* regs, uint32_t v)
{
    dhex8(regs, v >> 24);
    dhex8(regs, v >> 16);
    dhex8(regs, v >> 8);
    dhex8(regs, v);
}

/*
 * Alignment-safe memcpy. Replaces AmigaOS CopyMem, which has been
 * observed to silently no-op on this toolchain with GCC 15.2 when
 * either src or dst is at an odd address — Poseidon's iouh_Data
 * can legitimately arrive at odd alignments.
 *
 * The byte-loop at the bottom is the safe universal path. When
 * src and dst share 4-byte or 2-byte alignment we upgrade to
 * MOVE.L / MOVE.W copies. The fast path was proven correct
 * during mass-storage bring-up: FAT32 reads work end-to-end with
 * the fast path active, and the byte-level data verification
 * showed identical contents across repeat reads.
 */
static void safe_copy(const void *src, void *dst, uint32_t n)
{
    const uint8_t *s = (const uint8_t *)src;
    uint8_t *d = (uint8_t *)dst;

    /* Long-aligned fast path — both divisible by 4.
     * Use exec's CopyMemQuick (movem.l-based, ~10× faster than the C
     * long-per-move loop). Requires 4-byte-aligned src+dst and size
     * multiple of 4, which this branch already guarantees. */
    if (n >= 4 && ((((uintptr_t)s | (uintptr_t)d) & 3) == 0)) {
        uint32_t bulk = n & ~3U;
        CopyMemQuick((APTR)s, (APTR)d, bulk);
        s += bulk;
        d += bulk;
        n &= 3;
    }
    /* Word-aligned fast path — both divisible by 2. */
    else if (n >= 2 && ((((uintptr_t)s | (uintptr_t)d) & 1) == 0)) {
        const uint16_t *ws = (const uint16_t *)s;
        uint16_t *wd = (uint16_t *)d;
        uint32_t words = n >> 1;
        while (words--) *wd++ = *ws++;
        s = (const uint8_t *)ws;
        d = (uint8_t *)wd;
        n &= 1;
    }

    /* Byte tail, or unaligned-all-the-way. */
    while (n--) *d++ = *s++;
}

static int send_usb_cmd(volatile uint8_t *base, struct ZZUSBCommand *cmd,
                        void *data_out, uint32_t data_out_len)
{
    volatile struct ZZUSBCommand *result =
        (volatile struct ZZUSBCommand*)(base + 0xa000);
    volatile int delay;

    cmd->status = ZZUSB_STATUS_PENDING;
    /*
     * Use safe_copy for everything. AmigaOS CopyMem has been observed
     * to silently no-op on this toolchain when either src or dst is
     * at an odd address. Poseidon can hand us iouh_Data buffers at
     * arbitrary alignments, so play it safe across the board.
     */
    safe_copy(cmd, (void*)(base + 0xa000), ZZUSB_CMD_SIZE);

    if (data_out && data_out_len > 0 && data_out_len <= ZZUSB_MAX_XFER) {
        safe_copy(data_out, (void*)(base + 0xa000 + ZZUSB_DATA_OFFSET), data_out_len);
    }

    CacheClearU();

    *((volatile uint16_t*)(base + ZZ_REG_USB_PROXY_CMD)) = cmd->cmd;

    /*
     * Tight busy-wait poll for firmware response.
     *
     * Prior revisions used an inner delay loop of 1000 empty
     * iterations between status reads (~200us on 68020 per check).
     * For a typical 1-5ms bulk transfer that wastes dozens of
     * 200us windows, adding milliseconds of per-round-trip
     * overhead that compounds over every bulk chunk. Benchmark
     * showed this as the dominant throughput bottleneck vs the
     * Deneb reference.
     *
     * Replaced with a short inner delay (~10us) so the poll rate
     * matches the firmware response timing without hammering the
     * Zorro bus. Outer loop still bounded by cmd->timeout_ms so
     * a wedged firmware releases zz_Lock within the specified
     * deadline.
     *
     * Inner delay ~10us + status-read overhead ~5us = ~15us per
     * iteration. Outer count = timeout_ms * 66 gives us roughly
     * the right wall-clock bound. timeout_ms=0 falls back to
     * ~3 sec (200,000 iterations).
     */
    {
        uint32_t outer_limit = cmd->timeout_ms
                               ? (uint32_t)cmd->timeout_ms * 66
                               : 200000;
        uint32_t timeout = outer_limit;
        while (timeout-- > 0) {
            if (result->status != ZZUSB_STATUS_PENDING) break;
            delay = 50;        /* ~10us on 68020 */
            while (delay--);
        }
    }

    CacheClearU();

    return result->status;
}

static struct Library* __attribute__((used)) init_device(uint8_t *seg_list asm("a0"), struct Library *dev asm("d0"))
{
    struct Library* ExpansionBase;
    struct ConfigDev* cd = NULL;
    uint8_t* registers = NULL;

    SysBase = *(struct ExecBase **)4L;

    if (!(ExpansionBase = (struct Library*)OpenLibrary((uint8_t*)"expansion.library", 0L))) {
        return 0;
    }

    /* Prefer Zorro III (product 0x4) when available — Z3 bus is ~2× the
     * bandwidth of Z2 and dominates bulk throughput, since the mailbox
     * read/write at base+0xa000 is bus-bound rather than CPU-bound.
     * Fall back to Z2 (product 0x3) for cards installed in Z2 slots. */
    if ((cd = (struct ConfigDev*)FindConfigDev(cd, 0x6d6e, 0x4))) {
        registers = ((uint8_t*)cd->cd_BoardAddr);
    } else if ((cd = (struct ConfigDev*)FindConfigDev(cd, 0x6d6e, 0x3))) {
        registers = ((uint8_t*)cd->cd_BoardAddr);
    } else {
        CloseLibrary(ExpansionBase);
        return 0;
    }

    struct ZZUSBBase* ZZBase = (struct ZZUSBBase*)dev;
    if (!ZZBase) {
        CloseLibrary(ExpansionBase);
        return 0;
    }

    dev->lib_Node.ln_Type = NT_DEVICE;
    dev->lib_Node.ln_Name = (char *)device_name;
    dev->lib_Version = DEVICE_VERSION;
    dev->lib_Revision = DEVICE_REVISION;
    dev->lib_IdString = (char *)device_id_string;

    InitSemaphore(&ZZBase->zz_Lock);

    dstr(registers, "[zzusbhw] " XSTR(DEVICE_VERSION) "." XSTR(DEVICE_REVISION) "\r\n");

    struct ZZUSBUnit* unit = &ZZBase->zz_Units[0];
    unit->zz_Registers = registers;
    unit->zz_Enabled = TRUE;
    unit->zz_PortPresent = FALSE;
    unit->zz_PortDead = FALSE;
    unit->zz_RootHubAddr = 0;
    unit->zz_PortChange = 0;
    unit->zz_PortStatus = UPSF_PORT_POWER;
    unit->zz_Speed = 0;
    unit->zz_PendingIntXfer = NULL;
    unit->zz_BulkErrCount = 0;
    for (int ep = 0; ep < 16; ep++)
        unit->zz_IntPending[ep] = NULL;

    PollBase = ZZBase;
    /*
     * The poll task is created lazily on the first begin_io call,
     * not here. init_device runs during library AutoInit, before
     * MakeLibrary has sealed lib_Sum via SumLibrary; calling AddTask
     * in that window corrupts exec state and gurus with 80000004
     * (library checksum failure). See the matching block in
     * begin_io for the deferred creation.
     */
    ZZBase->zz_PollTask = NULL;

    unit->zz_Unit.unit_MsgPort.mp_MsgList.lh_Head = (struct Node *)&unit->zz_Unit.unit_MsgPort.mp_MsgList.lh_Tail;
    unit->zz_Unit.unit_MsgPort.mp_MsgList.lh_Tail = NULL;
    unit->zz_Unit.unit_MsgPort.mp_MsgList.lh_TailPred = (struct Node *)&unit->zz_Unit.unit_MsgPort.mp_MsgList.lh_Head;
    unit->zz_Unit.unit_flags = 0;
    unit->zz_Unit.unit_OpenCnt = 0;

    CloseLibrary(ExpansionBase);
    return dev;
}

static uint8_t* __attribute__((used)) expunge(struct Library *dev asm("a6"))
{
    return 0;
}

static void __attribute__((used)) open(struct Library *dev asm("a6"), struct IOUsbHWReq *ior asm("a1"),
                 uint32_t unitnum asm("d0"), uint32_t flags asm("d1"))
{
    struct ZZUSBBase* ZZBase = (struct ZZUSBBase*)dev;
    int io_err = IOERR_OPENFAIL;

    if (ior && unitnum < ZZ_NUM_PORTS) {
        struct ZZUSBUnit* unit = &ZZBase->zz_Units[unitnum];
        if (unit->zz_Enabled) {
            io_err = 0;
            ior->iouh_Req.io_Unit = (struct Unit*)unit;
            ior->iouh_Req.io_Unit->unit_flags = UNITF_ACTIVE;
            ior->iouh_Req.io_Unit->unit_OpenCnt = 1;
            dev->lib_OpenCnt++;
        }
    }

    ior->iouh_Req.io_Error = io_err;
}

static uint8_t* __attribute__((used)) close(struct Library *dev asm("a6"), struct IOUsbHWReq *ior asm("a1"))
{
    dev->lib_OpenCnt--;
    return 0;
}

static void complete_pending_intxfer(struct ZZUSBUnit *unit,
                                      volatile uint8_t *base);

/*
 * Abort every queued downstream interrupt IOR for this unit with
 * UHIOERR_USBOFFLINE. Used on hot-unplug from both the hub-INT
 * path and any other future context that detects device loss
 * before poll_int_pending's own offline-mapping sees it.
 *
 * The aborted IORs are appended to the caller's pending-reply
 * array; the actual ReplyMsg happens after zz_Lock is released.
 */
static void abort_int_iors_offline(struct ZZUSBUnit *unit,
                                    struct IOUsbHWReq **aborted,
                                    int *aborted_count,
                                    int aborted_max)
{
    for (int ep = 1; ep < 16; ep++) {
        struct IOUsbHWReq *p = unit->zz_IntPending[ep];
        if (!p) continue;
        unit->zz_IntPending[ep] = NULL;
        p->iouh_Actual = 0;
        p->iouh_Req.io_Error = UHIOERR_USBOFFLINE;
        if (aborted && aborted_count && *aborted_count < aborted_max) {
            aborted[(*aborted_count)++] = p;
        }
    }
}

static void update_port_state(struct ZZUSBUnit *unit,
                              volatile uint8_t *base,
                              struct IOUsbHWReq **aborted,
                              int *aborted_count,
                              int aborted_max)
{
    struct ZZUSBCommand chk;
    memset(&chk, 0, sizeof(chk));
    chk.cmd = ZZUSB_CMD_CHECK_PORT;

    if (send_usb_cmd(base, &chk, NULL, 0) == ZZUSB_STATUS_OK) {
        /*
         * Firmware says the port is still physically connected.
         * If we previously marked the device dead after an
         * unrecoverable error, refuse to re-enumerate: keep the
         * port reported as empty to Poseidon until the user
         * physically unplugs (which firmware detects as CCS=0
         * and takes the else-branch below). Without this sticky
         * state, a device that keeps babbling would trigger an
         * infinite disconnect/reconnect/babble loop.
         */
        if (unit->zz_PortDead) {
            /* Stay in the "not present" state. No state change. */
            return;
        }

        volatile struct ZZUSBCommand *r =
            (volatile struct ZZUSBCommand*)(base + 0xa000);
        UWORD port_status = UPSF_PORT_POWER | UPSF_PORT_CONNECTION;
        if (r->speed == ZZUSB_SPEED_HIGH) {
            port_status |= UPSF_PORT_HIGH_SPEED;
        }
        if (!unit->zz_PortPresent || unit->zz_Speed != r->speed) {
            unit->zz_PortPresent = TRUE;
            unit->zz_Speed = r->speed;
            unit->zz_PortStatus = port_status;
            unit->zz_PortChange = UPSF_C_PORT_CONNECTION;
            complete_pending_intxfer(unit, base);
        } else {
            unit->zz_PortStatus = port_status;
        }
    } else {
        /*
         * Firmware says no device on the port (PHYSICAL disconnect).
         * Clear the PortDead sticky state — if the user replugs, we
         * want to re-enumerate normally. Then finish the disconnect
         * bookkeeping if we thought the device was present.
         */
        unit->zz_PortDead = FALSE;
        if (unit->zz_PortPresent) {
            unit->zz_PortPresent = FALSE;
            unit->zz_Speed = 0;
            unit->zz_PortStatus = UPSF_PORT_POWER;
            unit->zz_PortChange = UPSF_C_PORT_CONNECTION;
            unit->zz_BulkErrCount = 0;
            abort_int_iors_offline(unit, aborted, aborted_count, aborted_max);
            complete_pending_intxfer(unit, base);
        }
    }
}

/*
 * Poll pending interrupt IORs on one unit. For each endpoint slot
 * with a pending IOR, issue an INT_XFER command to the firmware and,
 * only when firmware returns real data (actual > 0), fill the IOR
 * and ReplyMsg it. Empty polls (firmware returns actual=0) leave
 * the IOR queued. That's the async delivery pattern that lets
 * Poseidon's HID class see only genuine report-arrivals, which is
 * what Deneb does and what makes every device Just Work.
 *
 * IMPORTANT: send_usb_cmd issues a command to the ARM firmware and
 * blocks until the firmware responds. We release zz_Lock across the
 * send_usb_cmd call so begin_io calls from Poseidon aren't blocked
 * behind us (we only hold the lock for list manipulations).
 */
static void poll_int_pending(struct ZZUSBBase *base_dev,
                             struct ZZUSBUnit *unit)
{
    volatile uint8_t *base = (volatile uint8_t*)unit->zz_Registers;
    int ep;

    for (ep = 1; ep < 16; ep++) {
        struct IOUsbHWReq *reply_now = NULL;

        /*
         * Hold zz_Lock for the entire send_usb_cmd + result decode.
         * The shared firmware buffer is the serialized resource;
         * without this lock, begin_io and the poll task race each
         * other on the command mailbox. ReplyMsg is done AFTER the
         * lock is released to avoid re-entrancy deadlocks.
         */
        ObtainSemaphore(&base_dev->zz_Lock);

        struct IOUsbHWReq *ior = unit->zz_IntPending[ep];
        if (!ior) {
            ReleaseSemaphore(&base_dev->zz_Lock);
            continue;
        }

        struct ZZUSBCommand cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.cmd = ZZUSB_CMD_INT_XFER;
        cmd.dev_addr = ior->iouh_DevAddr;
        cmd.endpoint = ior->iouh_Endpoint;
        cmd.direction = (ior->iouh_Dir == UHDIR_IN) ? 0x80 : 0x00;
        cmd.max_pkt_size = ior->iouh_MaxPktSize;
        cmd.speed = unit->zz_Speed;
        cmd.data_length = ior->iouh_Length;
        cmd.interval = ior->iouh_Interval;
        /* Short NAK timeout so we don't hold zz_Lock long on idle
         * endpoints. Firmware returns quickly either with data or
         * with NAK. */
        cmd.timeout_ms = 10;

        uint16_t status = send_usb_cmd(base, &cmd,
                                       (ior->iouh_Dir == UHDIR_OUT) ? ior->iouh_Data : NULL,
                                       (ior->iouh_Dir == UHDIR_OUT) ? ior->iouh_Length : 0);

        if (status == ZZUSB_STATUS_OK) {
            volatile struct ZZUSBCommand *result =
                (volatile struct ZZUSBCommand*)(base + 0xa000);
            uint32_t actual = result->actual_length;

            /* Clamp against the caller's buffer length in case the
             * firmware returns a bogus actual_length after a data
             * buffer error — prevents heap corruption in Poseidon. */
            if (actual > ior->iouh_Length) actual = ior->iouh_Length;

            if (actual > 0) {
                if (ior->iouh_Dir == UHDIR_IN && ior->iouh_Data) {
                    safe_copy((void*)(base + 0xa000 + ZZUSB_DATA_OFFSET),
                              ior->iouh_Data, actual);
                }
                ior->iouh_Actual = actual;
                ior->iouh_Req.io_Error = 0;
                unit->zz_IntPending[ep] = NULL;
                reply_now = ior;
            }
            /* actual == 0: leave IOR queued. Firmware NAK'd; try
             * again on next poll cycle. This is the key difference
             * from synchronous delivery — HID class driver never
             * sees a spurious 0-byte success that would trigger
             * the cursor-slide bug. */
        } else if (status != ZZUSB_STATUS_TIMEOUT &&
                   status != ZZUSB_STATUS_NAK) {
            /* Hard error — fail the IOR with a retryable code.
             * Matches the v1.53 crash-safety policy: specific
             * error codes (STALL/OVERFLOW/CRC) have triggered
             * crashes in Poseidon's class-driver recovery paths,
             * so map to TIMEOUT unless it's genuine offline. */
            ior->iouh_Actual = 0;
            if (status == ZZUSB_STATUS_OFFLINE)
                ior->iouh_Req.io_Error = UHIOERR_USBOFFLINE;
            else
                ior->iouh_Req.io_Error = UHIOERR_TIMEOUT;
            unit->zz_IntPending[ep] = NULL;
            reply_now = ior;
        }

        ReleaseSemaphore(&base_dev->zz_Lock);

        if (reply_now && !(reply_now->iouh_Req.io_Flags & IOF_QUICK)) {
            ReplyMsg(&reply_now->iouh_Req.io_Message);
        }
    }
}

static void hotplug_poll_task(void)
{
    /*
     * Async interrupt-delivery loop. The v1.52 design:
     *  - begin_io UHCMD_INTXFER stashes IORs in zz_IntPending[ep]
     *    and defers the reply.
     *  - Signal() from begin_io wakes us up.
     *  - We scan pending IORs, issue firmware polls, reply only
     *    when real data arrives (actual > 0) or a hard error hits.
     *  - Idle loop Wait()s on our signal; zero CPU when no pending.
     */
    BYTE sig = AllocSignal(-1);
    ULONG mask = (sig >= 0) ? (1UL << sig) : (1UL << 16);

    PollBase->zz_PollSignal = mask;

    for (;;) {
        int any_pending = 0;

        for (int u = 0; u < ZZ_NUM_PORTS; u++) {
            struct ZZUSBUnit *unit = &PollBase->zz_Units[u];
            if (!unit->zz_Enabled) continue;

            poll_int_pending(PollBase, unit);

            for (int ep = 1; ep < 16; ep++) {
                if (unit->zz_IntPending[ep]) {
                    any_pending = 1;
                    break;
                }
            }
        }

        if (!any_pending) {
            Wait(mask);
        }
    }
}

static void handle_roothub_control(struct ZZUSBUnit *unit,
                                   struct IOUsbHWReq *ior,
                                   volatile uint8_t *base)
{
    uint8_t reqtype = ior->iouh_SetupData.bmRequestType;
    uint8_t request = ior->iouh_SetupData.bRequest;
    uint16_t wValue = SWAP16(ior->iouh_SetupData.wValue);
    uint16_t wIndex = SWAP16(ior->iouh_SetupData.wIndex);
    uint16_t port = wIndex;

    if ((reqtype & 0x60) == URTF_STANDARD) {
        switch (request) {
        case USR_GET_STATUS:
            if ((reqtype & 0x1f) == URTF_DEVICE) {
                uint8_t status[2] = { 0x01, 0x00 };
                uint16_t len = (ior->iouh_Length < 2) ? ior->iouh_Length : 2;
                if (ior->iouh_Data) {
                    safe_copy(status, ior->iouh_Data, len);
                }
                ior->iouh_Actual = len;
                ior->iouh_Req.io_Error = 0;
                return;
            }
            break;
        case USR_GET_DESCRIPTOR:
            if (wValue == (UDT_DEVICE << 8)) {
                static const uint8_t devdesc[18] = {
                    18, 0x01,             /* bLength, bDescriptorType = DEVICE */
                    0x00, 0x02,           /* bcdUSB = 2.0 */
                    0x09,                 /* bDeviceClass = HUB */
                    0x00,                 /* bDeviceSubClass */
                    0x01,                 /* bDeviceProtocol = single TT */
                    64,                   /* bMaxPacketSize0 */
                    0x6d, 0x6e,           /* idVendor = MNT */
                    0x00, 0x01,           /* idProduct */
                    0x01, 0x00,           /* bcdDevice */
                    0x01,                 /* iManufacturer */
                    0x02,                 /* iProduct */
                    0x00,                 /* iSerialNumber */
                    0x01                  /* bNumConfigurations */
                };
                uint16_t len = (ior->iouh_Length < 18) ? ior->iouh_Length : 18;
                if (ior->iouh_Data) {
                    safe_copy(devdesc, ior->iouh_Data, len);
                }
                ior->iouh_Actual = len;
                ior->iouh_Req.io_Error = 0;
                return;
            }
            if (wValue == (UDT_CONFIGURATION << 8)) {
                static const uint8_t cfgdesc[25] = {
                    9, 0x02,              /* Config descriptor */
                    25, 0x00,             /* wTotalLength */
                    0x01,                 /* bNumInterfaces */
                    0x01,                 /* bConfigurationValue */
                    0x00,                 /* iConfiguration */
                    0xe0,                 /* bmAttributes: bus powered */
                    0x01,                 /* bMaxPower = 2mA */
                    /* Interface descriptor */
                    9, 0x04,              /* bLength, bDescriptorType = INTERFACE */
                    0x00,                 /* bInterfaceNumber */
                    0x00,                 /* bAlternateSetting */
                    0x01,                 /* bNumEndpoints */
                    0x09,                 /* bInterfaceClass = HUB */
                    0x00,                 /* bInterfaceSubClass */
                    0x00,                 /* bInterfaceProtocol */
                    0x00,                 /* iInterface */
                    /* Endpoint descriptor (interrupt IN) */
                    7, 0x05,              /* bLength, bDescriptorType = ENDPOINT */
                    0x81,                 /* bEndpointAddress = EP1 IN */
                    0x03,                 /* bmAttributes = INTERRUPT */
                    0x08, 0x00,
                    12
                };
                uint16_t len = (ior->iouh_Length < 25) ? ior->iouh_Length : 25;
                if (ior->iouh_Data) {
                    safe_copy((void*)cfgdesc, ior->iouh_Data, len);
                }
                ior->iouh_Actual = len;
                ior->iouh_Req.io_Error = 0;
                return;
            }
            if ((wValue >> 8) == UDT_HUB) {
                uint8_t hubdesc[9] = {
                    9,                    /* bLength */
                    0x29,                 /* bDescriptorType = HUB */
                    ZZ_NUM_PORTS,         /* bNbrPorts */
                    0x00, 0x00,           /* wHubCharacteristics */
                    0x01,                 /* bPwrOn2PwrGood = 2ms */
                    0x00,                 /* bHubContrCurrent */
                    0x00,                 /* DeviceRemovable */
                    0xff                  /* PortPwrCtrlMask */
                };
                uint16_t len = (ior->iouh_Length < 9) ? ior->iouh_Length : 9;
                if (ior->iouh_Data) {
                    safe_copy((void*)hubdesc, ior->iouh_Data, len);
                }
                ior->iouh_Actual = len;
                ior->iouh_Req.io_Error = 0;
                return;
            }
            if ((wValue >> 8) == UDT_STRING) {
                uint8_t string_index = wValue & 0xff;
                if (string_index == 0) {
                    uint8_t lang_desc[4] = {
                        4, 0x03, 0x09, 0x04
                    };
                    uint16_t len = (ior->iouh_Length < 4) ? ior->iouh_Length : 4;
                    if (ior->iouh_Data) {
                        safe_copy((void*)lang_desc, ior->iouh_Data, len);
                    }
                    ior->iouh_Actual = len;
                    ior->iouh_Req.io_Error = 0;
                    return;
                } else if (string_index == 1) {
                    /* Manufacturer: "MNT Research GmbH" — 17 chars
                     * UTF-16LE = 34 bytes + 2-byte header = 36. */
                    static const uint8_t mfr_desc[] = {
                        36, 0x03,
                        'M', 0, 'N', 0, 'T', 0, ' ', 0,
                        'R', 0, 'e', 0, 's', 0, 'e', 0,
                        'a', 0, 'r', 0, 'c', 0, 'h', 0,
                        ' ', 0, 'G', 0, 'm', 0, 'b', 0,
                        'H', 0
                    };
                    uint16_t len = (ior->iouh_Length < mfr_desc[0]) ? ior->iouh_Length : mfr_desc[0];
                    if (ior->iouh_Data) {
                        safe_copy((void*)mfr_desc, ior->iouh_Data, len);
                    }
                    ior->iouh_Actual = len;
                    ior->iouh_Req.io_Error = 0;
                    return;
                } else if (string_index == 2) {
                    /* Product: "ZZ9000 USB Root Hub" — 19 chars
                     * UTF-16LE = 38 bytes + 2-byte header = 40.
                     * Previous array declared bLength=24 but held
                     * only 22 bytes, so Poseidon read 2 bytes of
                     * garbage past the end and displayed the
                     * product as "ZZ9000 USB?". */
                    static const uint8_t prod_desc[] = {
                        40, 0x03,
                        'Z', 0, 'Z', 0, '9', 0, '0', 0,
                        '0', 0, '0', 0, ' ', 0, 'U', 0,
                        'S', 0, 'B', 0, ' ', 0, 'R', 0,
                        'o', 0, 'o', 0, 't', 0, ' ', 0,
                        'H', 0, 'u', 0, 'b', 0
                    };
                    uint16_t len = (ior->iouh_Length < prod_desc[0]) ? ior->iouh_Length : prod_desc[0];
                    if (ior->iouh_Data) {
                        safe_copy((void*)prod_desc, ior->iouh_Data, len);
                    }
                    ior->iouh_Actual = len;
                    ior->iouh_Req.io_Error = 0;
                    return;
                }
            }
            break;

        case USR_SET_ADDRESS:
            unit->zz_RootHubAddr = wValue;
            ior->iouh_Actual = 0;
            ior->iouh_Req.io_Error = 0;
            return;

        case USR_GET_CONFIGURATION:
            {
                uint8_t cfg_val = 1;
                uint16_t len = (ior->iouh_Length < 1) ? ior->iouh_Length : 1;
                if (ior->iouh_Data) {
                    safe_copy(&cfg_val, ior->iouh_Data, len);
                }
                ior->iouh_Actual = len;
                ior->iouh_Req.io_Error = 0;
                return;
            }

        case USR_SET_CONFIGURATION:
            ior->iouh_Actual = 0;
            ior->iouh_Req.io_Error = 0;
            return;
        }
    }

    if ((reqtype & 0x60) == URTF_CLASS) {
        switch (request) {
        case USR_GET_DESCRIPTOR:
            if ((reqtype & 0x1f) == URTF_DEVICE && (wValue >> 8) == UDT_HUB) {
                uint8_t hubdesc[9] = {
                    9,
                    0x29,
                    ZZ_NUM_PORTS,
                    0x00, 0x00,
                    0x01,
                    0x00,
                    0x00,
                    0xff
                };
                uint16_t len = (ior->iouh_Length < 9) ? ior->iouh_Length : 9;
                if (ior->iouh_Data) {
                    safe_copy((void*)hubdesc, ior->iouh_Data, len);
                }
                ior->iouh_Actual = len;
                ior->iouh_Req.io_Error = 0;
                return;
            }
            break;

        case USR_GET_STATUS:
            if ((reqtype & 0x1f) == URTF_DEVICE) {
                uint16_t hub_status[2] = { 0, 0 };
                uint16_t len = (ior->iouh_Length < 4) ? ior->iouh_Length : 4;
                if (ior->iouh_Data) {
                    safe_copy(hub_status, ior->iouh_Data, len);
                }
                ior->iouh_Actual = len;
                ior->iouh_Req.io_Error = 0;
                return;
            }
            if ((reqtype & 0x1f) == URTF_OTHER && port > 0 && port <= ZZ_NUM_PORTS) {
                uint16_t ps[2];
                ps[0] = unit->zz_PortStatus;
                ps[1] = unit->zz_PortChange;
                uint16_t len = (ior->iouh_Length < 4) ? ior->iouh_Length : 4;
                if (ior->iouh_Data) {
                    safe_copy(ps, ior->iouh_Data, len);
                }
                ior->iouh_Actual = len;
                ior->iouh_Req.io_Error = 0;
                return;
            }
            break;

        case USR_SET_FEATURE:
            if ((reqtype & 0x1f) == URTF_DEVICE) {
                ior->iouh_Actual = 0;
                ior->iouh_Req.io_Error = 0;
                return;
            }
            if ((reqtype & 0x1f) == URTF_OTHER && port > 0 && port <= ZZ_NUM_PORTS) {
                switch (wValue) {
                case UFS_PORT_RESET:
                    {
                        unit->zz_PortStatus |= UPSF_PORT_RESET;
                        unit->zz_PortChange &= ~UPSF_C_PORT_RESET;

                        struct ZZUSBCommand rcmd;
                        volatile uint8_t *rbase = (volatile uint8_t*)unit->zz_Registers;
                        memset(&rcmd, 0, sizeof(rcmd));
                        rcmd.cmd = ZZUSB_CMD_RESET_PORT;
                        rcmd.timeout_ms = 5000;

                        uint16_t rstatus = send_usb_cmd(rbase, &rcmd, NULL, 0);

                        unit->zz_PortStatus &= ~UPSF_PORT_RESET;
                        if (rstatus == ZZUSB_STATUS_OK) {
                            volatile struct ZZUSBCommand *rresult =
                                (volatile struct ZZUSBCommand*)(rbase + 0xa000);
                            unit->zz_Speed = rresult->speed;
                            unit->zz_PortStatus |= UPSF_PORT_ENABLE;
                            if (unit->zz_Speed == ZZUSB_SPEED_HIGH) {
                                unit->zz_PortStatus |= UPSF_PORT_HIGH_SPEED;
                            }
                        }
                        unit->zz_PortChange |= UPSF_C_PORT_RESET;

                        complete_pending_intxfer(unit, base);

                        ior->iouh_Actual = 0;
                        ior->iouh_Req.io_Error = 0;
                        return;
                    }
                case UFS_PORT_POWER:
                    unit->zz_PortStatus |= UPSF_PORT_POWER;
                    ior->iouh_Actual = 0;
                    ior->iouh_Req.io_Error = 0;
                    return;
                case UFS_PORT_SUSPEND:
                    unit->zz_PortStatus |= UPSF_PORT_SUSPEND;
                    ior->iouh_Actual = 0;
                    ior->iouh_Req.io_Error = 0;
                    return;
                }
            }
            break;

case USR_CLEAR_FEATURE:
            if ((reqtype & 0x1f) == URTF_DEVICE) {
                ior->iouh_Actual = 0;
                ior->iouh_Req.io_Error = 0;
                return;
            }
            if ((reqtype & 0x1f) == URTF_OTHER && port > 0 && port <= ZZ_NUM_PORTS) {
                switch (wValue) {
                case UFS_PORT_ENABLE:
                    unit->zz_PortStatus &= ~UPSF_PORT_ENABLE;
                    ior->iouh_Actual = 0;
                    ior->iouh_Req.io_Error = 0;
                    return;
                case UFS_C_PORT_CONNECTION:
                    unit->zz_PortChange &= ~UPSF_C_PORT_CONNECTION;
                    ior->iouh_Actual = 0;
                    ior->iouh_Req.io_Error = 0;
                    return;
                case UFS_C_PORT_ENABLE:
                    unit->zz_PortChange &= ~UPSF_C_PORT_ENABLE;
                    ior->iouh_Actual = 0;
                    ior->iouh_Req.io_Error = 0;
                    return;
                case UFS_C_PORT_RESET:
                    unit->zz_PortChange &= ~UPSF_C_PORT_RESET;
                    ior->iouh_Actual = 0;
                    ior->iouh_Req.io_Error = 0;
                    return;
                case UFS_C_PORT_SUSPEND:
                    unit->zz_PortChange &= ~UPSF_PORT_SUSPEND;
                    ior->iouh_Actual = 0;
                    ior->iouh_Req.io_Error = 0;
                    return;
                case UFS_C_PORT_OVER_CURRENT:
                    unit->zz_PortChange &= ~UPSF_C_PORT_OVER_CURRENT;
                    ior->iouh_Actual = 0;
                    ior->iouh_Req.io_Error = 0;
                    return;
                }
            }
            break;
        }
    }

    ior->iouh_Req.io_Error = UHIOERR_STALL;
    ior->iouh_Actual = 0;
}

static void complete_pending_intxfer(struct ZZUSBUnit *unit,
                                      volatile uint8_t *base)
{
    if (unit->zz_PendingIntXfer && unit->zz_PortChange != 0) {
        struct IOUsbHWReq *pending = unit->zz_PendingIntXfer;
        unit->zz_PendingIntXfer = NULL;

        uint8_t change_bitmap[2] = { 0x02, 0x00 };
        uint16_t len = (pending->iouh_Length < 2) ? pending->iouh_Length : 2;
        if (pending->iouh_Data) {
            safe_copy(change_bitmap, pending->iouh_Data, len);
        }
        pending->iouh_Actual = len;
        pending->iouh_Req.io_Error = 0;

        if (!(pending->iouh_Req.io_Flags & IOF_QUICK)) {
            ReplyMsg(&pending->iouh_Req.io_Message);
        }
    }
}

static void handle_roothub_int(struct ZZUSBUnit *unit,
                                struct IOUsbHWReq *ior,
                                volatile uint8_t *base,
                                int *deferred,
                                struct IOUsbHWReq **aborted,
                                int *aborted_count,
                                int aborted_max)
{
    (void)deferred;
    if (ior->iouh_Endpoint == 1 && ior->iouh_Data) {
        /*
         * Re-check port state on every hub-INT poll unless
         * Poseidon hasn't yet acked the previous transition
         * (zz_PortChange != 0). This is how we pick up both
         * hot-plug and hot-unplug — without refreshing here,
         * once the port was known occupied we'd never see the
         * C_PORT_CONNECTION transition going the other way.
         */
        if (unit->zz_PortChange == 0) {
            update_port_state(unit, base, aborted, aborted_count, aborted_max);
        }
        if (unit->zz_PortChange == 0) {
            /* Idle hub INT reply — 0-byte change bitmap. */
            uint8_t no_change[2] = { 0x00, 0x00 };
            uint16_t len = (ior->iouh_Length < 2) ? ior->iouh_Length : 2;
            safe_copy(no_change, ior->iouh_Data, len);
            ior->iouh_Actual = len;
            ior->iouh_Req.io_Error = 0;
            return;
        }
        uint8_t change_bitmap[2] = { 0x02, 0x00 };
        uint16_t len = (ior->iouh_Length < 2) ? ior->iouh_Length : 2;
        safe_copy(change_bitmap, ior->iouh_Data, len);
        ior->iouh_Actual = len;
        ior->iouh_Req.io_Error = 0;
    } else {
        ior->iouh_Actual = 0;
        ior->iouh_Req.io_Error = UHIOERR_STALL;
    }
}

static void __attribute__((used)) begin_io(struct Library *dev asm("a6"), struct IOUsbHWReq *ior asm("a1"))
{
    struct ZZUSBBase* ZZBase = (struct ZZUSBBase*)dev;
    struct ZZUSBUnit* unit;
    int deferred = 0;
    /*
     * IORs that we need to ReplyMsg AFTER releasing zz_Lock.
     * Calling ReplyMsg while holding a semaphore can cause
     * scheduling issues if the receiving task tries to re-enter
     * our driver. These are collected inside the switch while
     * the lock is held and replied once the lock is released.
     */
    struct IOUsbHWReq *deferred_old_ior = NULL;
    struct IOUsbHWReq *aborted_replies[18];  /* 16 int slots + 1 root hub + 1 slack */
    int aborted_count = 0;
    for (int _i = 0; _i < 18; _i++) aborted_replies[_i] = NULL;

    if (!ZZBase || !ior) return;

    unit = (struct ZZUSBUnit*)ior->iouh_Req.io_Unit;
    if (!unit) {
        ior->iouh_Req.io_Error = IOERR_NOCMD;
        if (!(ior->iouh_Req.io_Flags & IOF_QUICK)) {
            ReplyMsg(&ior->iouh_Req.io_Message);
        }
        return;
    }



    /*
     * Lazy poll-task creation.
     *
     * AddTask cannot safely be called from init_device: that runs
     * during library AutoInit, before MakeLibrary has sealed
     * lib_Sum via SumLibrary, and touching the scheduler's ready
     * list in that window corrupts exec state enough to guru with
     * 80000004. By the time begin_io is first called, Poseidon has
     * already OpenDevice'd us, MakeLibrary has returned, and
     * lib_Sum is stable — AddTask is safe here.
     *
     * Forbid/Permit protects the one-shot init against re-entrant
     * begin_io calls that race to create the task. AddTask inside
     * a Forbid section is legal; the new task is installed on the
     * ready list but doesn't start running until Permit returns.
     */
    /*
     * Poll-task creation moved OUT of the generic begin_io path.
     * We now only create it on the first downstream INTXFER (the
     * actual path that needs it). In the empty-port bring-online
     * scenario we never reach that path — no poll task, no crash.
     */

    volatile uint8_t* base = (volatile uint8_t*)unit->zz_Registers;

    ObtainSemaphore(&ZZBase->zz_Lock);

    switch (ior->iouh_Req.io_Command) {
    case UHCMD_QUERYDEVICE:
        {
            /*
             * QUERYDEVICE returns driver info. iouh_Data is a caller-
             * provided TagItem array terminated by TAG_DONE. Per
             * the standard Amiga TagItem convention used by every
             * usbhardware.device Poseidon talks to (Deneb, Subway,
             * RapidRoad, Highway), ti_Data is a *direct value slot*:
             * write the ULONG value (or STRPTR for string tags)
             * straight into ti_Data. The autodoc's "pointer to a
             * longword (ULONG *)" phrasing is about the field type,
             * not the semantics.
             *
             * v1.54 temporarily treated ti_Data as a pointer-to-
             * longword (like psdGetAttrs), dereferenced it, and wrote
             * values into whatever addresses Poseidon had pre-filled.
             * That corrupted Poseidon's internal state and crashed
             * Trident with guru 80000004. Revert to direct-slot
             * writes — matches all working drivers.
             *
             * Control-tag handling (TAG_IGNORE / TAG_MORE / TAG_SKIP)
             * is preserved so a valid TagItem taglist of any shape
             * is walked safely.
             *
             * Poseidon also queries an extra tag (UHA_Dummy + 0x21
             * = 0x80004732) not declared in our NDK usbhardware.h.
             * Empirically it sits next to UHA_DriverVersion and is
             * likely a feature-flag tag added in newer Poseidon
             * releases. Leave it alone — we don't know its meaning,
             * and zeroing it could confuse Poseidon.
             */
            /*
             * Simplest possible QUERYDEVICE walker — matches the
             * shape v1.52 had when everything worked. No control-
             * tag (TAG_IGNORE / TAG_MORE / TAG_SKIP) handling; plain
             * linear walk until TAG_DONE. The "full-featured" walker
             * added in v1.53 appears to have tightened timing enough
             * to expose a race in the poll-task first-dispatch path.
             */
            struct TagItem* tags = (struct TagItem*)ior->iouh_Data;
            if (tags) {
                while (tags->ti_Tag != TAG_DONE) {
                    switch (tags->ti_Tag) {
                    case UHA_DriverVersion:
                        tags->ti_Data = 0x0200;
                        break;
                    case UHA_Version:
                        tags->ti_Data = DEVICE_VERSION;
                        break;
                    case UHA_Revision:
                        tags->ti_Data = DEVICE_REVISION;
                        break;
                    case UHA_State:
                        tags->ti_Data = UHSF_OPERATIONAL;
                        break;
                    case UHA_Manufacturer:
                        tags->ti_Data = (ULONG)(uintptr_t)"MNT Research GmbH";
                        break;
                    case UHA_ProductName:
                        tags->ti_Data = (ULONG)(uintptr_t)
                            "ZZ9000 USB Host Controller";
                        break;
                    case UHA_Description:
                        tags->ti_Data = (ULONG)(uintptr_t)
                            "Poseidon USB hardware driver for the ZZ9000 "
                            "Zorro card (Zynq ChipIdea EHCI)";
                        break;
                    case UHA_Copyright:
                        tags->ti_Data = (ULONG)(uintptr_t)
                            "(C) Copyright 2026 Dimitris Panokostas. "
                            "Licensed under GNU GPL v3 or later.";
                        break;
                    default:
                        break;
                    }
                    tags++;
                }
            }
            ior->iouh_Actual = 0;
            ior->iouh_Req.io_Error = 0;
        }
        break;

    case UHCMD_USBRESET:
        {
            struct ZZUSBCommand cmd;
            memset(&cmd, 0, sizeof(cmd));
            cmd.cmd = ZZUSB_CMD_RESET_PORT;
            cmd.timeout_ms = 5000;

            uint16_t status = send_usb_cmd(base, &cmd, NULL, 0);

            if (status == ZZUSB_STATUS_OK) {
                volatile struct ZZUSBCommand *result =
                    (volatile struct ZZUSBCommand*)(base + 0xa000);
                /*
                 * Only flag POWER + CONNECTION + speed here.
                 * Poseidon's hub class drives the enable / C_RESET
                 * transitions via the subsequent hub SET_FEATURE
                 * and GET_STATUS control transfers handled in
                 * handle_roothub_control. Setting PE or C_RESET at
                 * this point triggers a Poseidon recovery path
                 * that can reboot the Amiga on device-online.
                 */
                UWORD port_status = UPSF_PORT_POWER | UPSF_PORT_CONNECTION;
                if (result->speed == ZZUSB_SPEED_HIGH) {
                    port_status |= UPSF_PORT_HIGH_SPEED;
                }
                unit->zz_Speed = result->speed;
                unit->zz_PortPresent = TRUE;
                unit->zz_PortStatus = port_status;
                unit->zz_PortChange = UPSF_C_PORT_CONNECTION;
            } else {
                unit->zz_PortPresent = FALSE;
                unit->zz_PortStatus = UPSF_PORT_POWER;
                unit->zz_PortChange = 0;
                unit->zz_Speed = 0;
            }

            /*
             * USBRESET always-OK reporting (matches v1.53 behaviour).
             *
             * v1.58 tried to report UHIOERR_HOSTERROR on firmware
             * error for strictness, but that set up a contradiction
             * (io_Error = HOSTERROR + iouh_State = UHSF_OPERATIONAL)
             * that sent Poseidon down a recovery code path which
             * in turn tripped the poll task — net result: crash.
             *
             * "Empty port" is a normal operational state for a root
             * hub, not a hardware error. Report success; downstream
             * port-status / port-connection bits (set above) tell
             * Poseidon the port is simply unoccupied.
             */
            ior->iouh_Req.io_Error = 0;
            ior->iouh_State = UHSF_OPERATIONAL;

            if (status == ZZUSB_STATUS_OK) {
                complete_pending_intxfer(unit, base);
            }
        }
        break;

    case UHCMD_CONTROLXFER:
        {
            uint16_t rh_addr = unit->zz_RootHubAddr;

            if ((rh_addr == 0 && ior->iouh_DevAddr == 0) || ior->iouh_DevAddr == rh_addr) {
                handle_roothub_control(unit, ior, (void*)base);
            } else {
                struct ZZUSBCommand cmd;
                uint16_t status;

                if (ior->iouh_Length > ZZUSB_MAX_XFER) {
                    ior->iouh_Req.io_Error = UHIOERR_PKTTOOLARGE;
                    break;
                }

                memset(&cmd, 0, sizeof(cmd));
                cmd.cmd = ZZUSB_CMD_CONTROL_XFER;
                cmd.dev_addr = ior->iouh_DevAddr;
                cmd.endpoint = ior->iouh_Endpoint;
                cmd.direction = (ior->iouh_SetupData.bmRequestType & 0x80) ? 0x80 : 0x00;
                cmd.max_pkt_size = ior->iouh_MaxPktSize;
                cmd.speed = unit->zz_Speed;
                cmd.data_length = ior->iouh_Length;
                cmd.timeout_ms = (ior->iouh_Flags & UHFF_NAKTIMEOUT)
                                 ? (ior->iouh_NakTimeout ? ior->iouh_NakTimeout : 5000)
                                 : 0;

                cmd.setup_bRequestType = ior->iouh_SetupData.bmRequestType;
                cmd.setup_bRequest = ior->iouh_SetupData.bRequest;
                cmd.setup_wValue = ior->iouh_SetupData.wValue;
                cmd.setup_wIndex = ior->iouh_SetupData.wIndex;
                cmd.setup_wLength = ior->iouh_SetupData.wLength;

                status = send_usb_cmd(base, &cmd,
                                      (ior->iouh_Dir == UHDIR_OUT) ? ior->iouh_Data : NULL,
                                      (ior->iouh_Dir == UHDIR_OUT) ? ior->iouh_Length : 0);

                if (status == ZZUSB_STATUS_OK) {
                    volatile struct ZZUSBCommand *result =
                        (volatile struct ZZUSBCommand*)(base + 0xa000);
                    uint32_t actual = result->actual_length;

                    /*
                     * Clamp actual against the caller's iouh_Length
                     * in case firmware returns a bogus length after
                     * an EHCI data-buffer or transaction error. A
                     * garbage actual would make safe_copy overflow
                     * iouh_Data and corrupt Poseidon's heap.
                     */
                    if (actual > ior->iouh_Length) actual = ior->iouh_Length;

                    int is_in = (ior->iouh_SetupData.bmRequestType & 0x80) != 0;
                    if (is_in && ior->iouh_Data && actual > 0) {
                        /*
                         * safe_copy not CopyMem — Poseidon's iouh_Data
                         * can be odd-aligned on small descriptor reads,
                         * and CopyMem silently no-ops on this toolchain
                         * with odd endpoints, leaving caller with stale
                         * bytes (e.g. MaxPktSize0=178).
                         */
                        safe_copy((void*)(base + 0xa000 + ZZUSB_DATA_OFFSET),
                                  ior->iouh_Data, actual);
                    }
                    ior->iouh_Actual = actual;
                    ior->iouh_Req.io_Error = 0;
                } else {
                    /*
                     * Control error handling — v1.52 shape. Report
                     * the error honestly; let Poseidon decide on
                     * recovery. The v1.53 "mark port dead + fake
                     * connection-change" recovery was implicated in
                     * HID-bring-up crashes and has been removed.
                     */
                    ior->iouh_Actual = 0;
                    switch (status) {
                    case ZZUSB_STATUS_STALL:
                        ior->iouh_Req.io_Error = UHIOERR_STALL; break;
                    case ZZUSB_STATUS_TIMEOUT:
                    case ZZUSB_STATUS_NAK:
                        ior->iouh_Req.io_Error = UHIOERR_TIMEOUT; break;
                    case ZZUSB_STATUS_OFFLINE:
                        ior->iouh_Req.io_Error = UHIOERR_USBOFFLINE; break;
                    case ZZUSB_STATUS_CRC:
                        ior->iouh_Req.io_Error = UHIOERR_CRCERROR; break;
                    case ZZUSB_STATUS_OVERRUN:
                    case ZZUSB_STATUS_BABBLE:
                        ior->iouh_Req.io_Error = UHIOERR_HOSTERROR; break;
                    default:
                        ior->iouh_Req.io_Error = UHIOERR_HOSTERROR; break;
                    }
                }
            }
        }
        break;

    case UHCMD_USBRESUME:
    case UHCMD_USBSUSPEND:
    case UHCMD_USBOPER:
        ior->iouh_Req.io_Error = 0;
        ior->iouh_State = UHSF_OPERATIONAL;
        break;

    case UHCMD_INTXFER:
        {
            uint16_t rh_addr = unit->zz_RootHubAddr;

            if (((rh_addr == 0 && ior->iouh_DevAddr == 0) || ior->iouh_DevAddr == rh_addr)
                && ior->iouh_Endpoint == 1) {
                handle_roothub_int(unit, ior, base, &deferred,
                                   aborted_replies, &aborted_count,
                                   (int)(sizeof(aborted_replies) /
                                         sizeof(aborted_replies[0])));
            } else {
                /*
                 * Async delivery for downstream interrupt endpoints.
                 * Stash the IOR, Signal the poll task, defer the
                 * reply. Task replies when real data arrives —
                 * same design as Deneb, proven to work for HID in
                 * v1.52 and earlier.
                 */
                if (ior->iouh_Length > ZZUSB_MAX_XFER) {
                    ior->iouh_Req.io_Error = UHIOERR_PKTTOOLARGE;
                    break;
                }

                uint16_t ep = ior->iouh_Endpoint & 0x0f;
                if (ep == 0) {
                    /* EP0 is not valid for interrupt transfers. */
                    ior->iouh_Req.io_Error = UHIOERR_STALL;
                    break;
                }

                /* If Poseidon re-queued on the same endpoint before
                 * we replied to the last one, abort the old one so
                 * the class driver doesn't leak msgs. Reply after
                 * releasing zz_Lock. */
                if (unit->zz_IntPending[ep]) {
                    deferred_old_ior = unit->zz_IntPending[ep];
                    deferred_old_ior->iouh_Actual = 0;
                    deferred_old_ior->iouh_Req.io_Error = IOERR_ABORTED;
                    unit->zz_IntPending[ep] = NULL;
                }

                unit->zz_IntPending[ep] = ior;
                deferred = 1;      /* do NOT ReplyMsg at bottom */

                /* Lazy poll-task creation — on first downstream
                 * INTXFER only. Root-hub INT / USBRESET / bulk /
                 * control are all synchronous and don't need the
                 * task; creating it only here means the empty-port
                 * bring-online path never creates a task. */
                Forbid();
                if (!ZZBase->zz_PollTask) {
                    struct Task *poll = &ZZBase->zz_PollTaskStorage;

                    uint8_t *tp = (uint8_t*)poll;
                    for (unsigned i = 0; i < sizeof(struct Task); i++)
                        tp[i] = 0;

                    poll->tc_Node.ln_Type = NT_TASK;
                    poll->tc_Node.ln_Pri  = -1;
                    poll->tc_Node.ln_Name = (char *)"zzusbhw.poll";

                    poll->tc_SPLower = (APTR)&ZZBase->zz_PollStack[0];
                    poll->tc_SPUpper = (APTR)&ZZBase->zz_PollStack[1024];
                    poll->tc_SPReg   = (APTR)&ZZBase->zz_PollStack[1024];

                    poll->tc_MemEntry.lh_Head =
                        (struct Node *)&poll->tc_MemEntry.lh_Tail;
                    poll->tc_MemEntry.lh_Tail = NULL;
                    poll->tc_MemEntry.lh_TailPred =
                        (struct Node *)&poll->tc_MemEntry.lh_Head;
                    poll->tc_MemEntry.lh_Type = NT_MEMORY;

                    /*
                     * Reserve system signals 0..15 so AllocSignal(-1)
                     * in the task body returns a user bit (16..31)
                     * and doesn't steal signal 0 which exec uses
                     * for port-reply machinery. Also set nest counts
                     * to -1 ("not currently inside Forbid/Disable")
                     * — these are supposed to be fixed up by AddTask
                     * on some exec versions but not all.
                     */
                    poll->tc_SigAlloc  = 0xFFFF;
                    poll->tc_IDNestCnt = -1;
                    poll->tc_TDNestCnt = -1;

                    PollBase = ZZBase;
                    ZZBase->zz_PollTask = poll;

                    /*
                     * AddTask is called via hand-written inline asm
                     * because the NDK's LP3 inline macro
                     * (inline/macros.h) uses an "rf" constraint that
                     * lets gcc allocate initPC and finalPC to d0/d1
                     * instead of the AmigaOS-mandated a2/a3. MuForce
                     * confirmed the bug: dispatched PC = 0 because
                     * AddTask was reading initPC from a garbage a2.
                     * This inline explicitly pins the arguments into
                     * a1/a2/a3 as the ABI requires.
                     *
                     * Offset -0x11A (= -282) is AddTask's LVO.
                     */
                    {
                        struct Task *const _t = poll;
                        const APTR _ipc = (APTR)hotplug_poll_task;
                        const APTR _fpc = NULL;
                        register struct Task *_a1 __asm("a1") = _t;
                        register APTR _a2 __asm("a2") = _ipc;
                        register APTR _a3 __asm("a3") = _fpc;
                        register void *_a6 __asm("a6") = SysBase;
                        __asm volatile ("jsr a6@(-0x11a:W)"
                            : "+r"(_a1), "+r"(_a2), "+r"(_a3)
                            : "r"(_a6)
                            : "d0","d1","a0","cc","memory");
                    }
                }
                Permit();

                /* Wake the task. Skip the Signal if the task hasn't
                 * yet AllocSignal'd its bit — it'll pick up the
                 * pending IOR on its first loop iteration. */
                if (ZZBase->zz_PollTask && ZZBase->zz_PollSignal) {
                    Signal(ZZBase->zz_PollTask, ZZBase->zz_PollSignal);
                }
            }
        }
        break;

    case UHCMD_BULKXFER:
        {
            /*
             * Chunked bulk-transfer loop.
             *
             * Our firmware mailbox is a single 24KB shared buffer
             * (ZZUSB_MAX_XFER ~= 24512 bytes after the header). For
             * efficient large-file transfers, Poseidon's massstorage
             * class submits much larger bulk chunks (32KB / 64KB or
             * more). Earlier revisions rejected anything over that
             * limit with UHIOERR_PKTTOOLARGE, and Poseidon's bulk-
             * error recovery state machine didn't handle that code
             * gracefully — after a few such rejections Poseidon's
             * internal state would shred and hard-lock the Amiga,
             * without ever surfacing a guru (MuForce wasn't seeing
             * anything because it wasn't a trap-catchable fault —
             * it was exec state getting corrupted).
             *
             * Solution: loop, sending the transfer to firmware in
             * ZZUSB_MAX_XFER chunks, and return a single combined
             * reply. Short-read on BULK IN (firmware returns less
             * than requested) signals end-of-transfer per USB spec;
             * we stop the loop there.
             */
            struct ZZUSBCommand cmd;
            uint16_t status = ZZUSB_STATUS_OK;
            uint32_t remaining = ior->iouh_Length;
            uint32_t total_actual = 0;
            uint8_t *user_buf = (uint8_t *)ior->iouh_Data;

            /*
             * Bulk chunk size: 16 KB per EHCI transaction.
             *
             * Sweet spot found empirically. 8 KB (historical default)
             * left ~37% write / ~28% read throughput on the table due
             * to per-chunk fixed overhead (cache ops, EHCI queue setup,
             * mailbox round-trip). 16 KB amortises that overhead while
             * fitting in a single EHCI QTD (5 × 4 KB pages = 20 KB max
             * per QTD). 24 KB forces a 2-QTD chain whose second QTD
             * starves on AXI contention and returns EHCI Data Buffer
             * Errors (status=0x20), wedging the Amiga via Poseidon's
             * recovery path. Raising this again requires Vivado AXI QoS
             * tuning (lever #5) or a larger shared buffer (lever #3).
             */
            enum { BULK_CHUNK = 16384 };

            while (remaining > 0) {
                uint32_t chunk = (remaining > BULK_CHUNK)
                                 ? BULK_CHUNK : remaining;

                memset(&cmd, 0, sizeof(cmd));
                cmd.cmd = ZZUSB_CMD_BULK_XFER;
                cmd.dev_addr = ior->iouh_DevAddr;
                cmd.endpoint = ior->iouh_Endpoint;
                cmd.direction = (ior->iouh_Dir == UHDIR_IN) ? 0x80 : 0x00;
                cmd.max_pkt_size = ior->iouh_MaxPktSize;
                cmd.speed = unit->zz_Speed;
                cmd.data_length = chunk;
                cmd.timeout_ms = (ior->iouh_Flags & UHFF_NAKTIMEOUT)
                                 ? (ior->iouh_NakTimeout ? ior->iouh_NakTimeout : 500)
                                 : 500;

                status = send_usb_cmd(base, &cmd,
                                      (ior->iouh_Dir == UHDIR_OUT && user_buf) ? user_buf : NULL,
                                      (ior->iouh_Dir == UHDIR_OUT) ? chunk : 0);

                if (status != ZZUSB_STATUS_OK) {
                    break;      /* error — fall through to error handling */
                }

                {
                    volatile struct ZZUSBCommand *result =
                        (volatile struct ZZUSBCommand*)(base + 0xa000);
                    uint32_t actual = result->actual_length;

                    if (actual > chunk) actual = chunk;

                    if (ior->iouh_Dir == UHDIR_IN && user_buf && actual > 0) {
                        safe_copy((void*)(base + 0xa000 + ZZUSB_DATA_OFFSET),
                                  user_buf, actual);
                    }

                    total_actual += actual;
                    user_buf += actual;
                    remaining -= actual;

                    /* Short packet on IN signals end-of-transfer
                     * per USB spec — stop looping, don't report an
                     * error. SCSI data-IN transfers routinely end
                     * on a short packet when the device is done. */
                    if (ior->iouh_Dir == UHDIR_IN && actual < chunk) {
                        break;
                    }

                    /* For OUT, if firmware reported 0 or partial
                     * we should also stop — can't push more if the
                     * device isn't accepting. */
                    if (ior->iouh_Dir == UHDIR_OUT && actual < chunk) {
                        break;
                    }
                }
            }

            if (status == ZZUSB_STATUS_OK) {
                ior->iouh_Actual = total_actual;
                unit->zz_BulkErrCount = 0;
                ior->iouh_Req.io_Error = 0;
            } else {
                /*
                 * Error path. Explicitly zero iouh_Actual so the
                 * caller doesn't misread a stale value as valid
                 * transfer length and over-read iouh_Data (which
                 * we did NOT populate on the error path). Observed:
                 * USB ethernet devices that glitch mid-transfer
                 * would return HOSTERROR with stale iouh_Actual,
                 * Poseidon's class driver would walk past the end
                 * of iouh_Data, and AmigaDOS would guru.
                 */
                /*
                 * Bulk error handling — bounded-retry policy.
                 *
                 * Default mapping: non-offline → UHIOERR_TIMEOUT so
                 * the class driver retries normally (avoids specific
                 * error codes that have historically crashed
                 * Poseidon class drivers).
                 *
                 * Escalation: after N consecutive failures on this
                 * unit, report UHIOERR_USBOFFLINE. That forces
                 * Poseidon to tear the device down cleanly instead
                 * of retrying forever. Sustained-retry loops
                 * (4000+ failures over a large file copy) were
                 * observed to eventually shred Poseidon's internal
                 * state and hard-lock the Amiga.
                 *
                 * Counter resets on any successful bulk, so a
                 * transient glitch doesn't cause a spurious
                 * detach.
                 */
                ior->iouh_Actual = 0;
                if (status == ZZUSB_STATUS_OFFLINE) {
                    ior->iouh_Req.io_Error = UHIOERR_USBOFFLINE;
                } else {
                    if (unit->zz_BulkErrCount < 0xFFFF)
                        unit->zz_BulkErrCount++;
                    if (unit->zz_BulkErrCount >= 16) {
                        /* Too many consecutive errors — give up on
                         * this device cleanly. */
                        ior->iouh_Req.io_Error = UHIOERR_USBOFFLINE;
                        unit->zz_PortDead = TRUE;
                    } else {
                        ior->iouh_Req.io_Error = UHIOERR_TIMEOUT;
                    }
                }
            }
        }
        break;

    case CMD_RESET:
    case CMD_FLUSH:
        /*
         * Abort every queued IOR. Collect them here; actual
         * ReplyMsg happens AFTER zz_Lock is released to avoid
         * scheduling issues if a replied task immediately
         * re-enters our driver.
         */
        if (unit->zz_PendingIntXfer) {
            struct IOUsbHWReq *pending = unit->zz_PendingIntXfer;
            unit->zz_PendingIntXfer = NULL;
            pending->iouh_Actual = 0;
            pending->iouh_Req.io_Error = IOERR_ABORTED;
            if (aborted_count < 18)
                aborted_replies[aborted_count++] = pending;
        }
        for (int ep = 0; ep < 16; ep++) {
            struct IOUsbHWReq *pending = unit->zz_IntPending[ep];
            if (!pending) continue;
            unit->zz_IntPending[ep] = NULL;
            pending->iouh_Actual = 0;
            pending->iouh_Req.io_Error = IOERR_ABORTED;
            if (aborted_count < 18)
                aborted_replies[aborted_count++] = pending;
        }
        ior->iouh_Req.io_Error = 0;
        break;
    default:
        ior->iouh_Req.io_Error = IOERR_NOCMD;
        break;
    }

    ReleaseSemaphore(&ZZBase->zz_Lock);

    /* Reply any IORs that were aborted during CMD_RESET/CMD_FLUSH
     * or pre-empted by a re-queue in UHCMD_INTXFER. Done after
     * the lock release to avoid re-entrancy issues. */
    if (deferred_old_ior &&
        !(deferred_old_ior->iouh_Req.io_Flags & IOF_QUICK)) {
        ReplyMsg(&deferred_old_ior->iouh_Req.io_Message);
    }
    for (int _i = 0; _i < aborted_count; _i++) {
        struct IOUsbHWReq *p = aborted_replies[_i];
        if (p && !(p->iouh_Req.io_Flags & IOF_QUICK)) {
            ReplyMsg(&p->iouh_Req.io_Message);
        }
    }

    if (!deferred) {
        if (!(ior->iouh_Req.io_Flags & IOF_QUICK)) {
            ReplyMsg(&ior->iouh_Req.io_Message);
        }
    }
}

static uint32_t __attribute__((used)) abort_io(struct Library *dev asm("a6"), struct IOUsbHWReq *ior asm("a1"))
{
    /*
     * Abort a queued IOR. The two pending slots we track are:
     *  - unit->zz_PendingIntXfer: root-hub hub-status IOR
     *  - unit->zz_IntPending[ep]: async interrupt IOR per endpoint
     * All access is serialised by zz_Lock.
     */
    struct ZZUSBBase *ZZBase = (struct ZZUSBBase*)dev;
    if (!ior || !ZZBase) return IOERR_NOCMD;
    struct ZZUSBUnit *unit = (struct ZZUSBUnit *)ior->iouh_Req.io_Unit;
    if (!unit) {
        ior->iouh_Req.io_Error = IOERR_ABORTED;
        return IOERR_ABORTED;
    }

    ObtainSemaphore(&ZZBase->zz_Lock);
    int found = 0;
    if (unit->zz_PendingIntXfer == ior) {
        unit->zz_PendingIntXfer = NULL;
        found = 1;
    } else {
        for (int ep = 0; ep < 16; ep++) {
            if (unit->zz_IntPending[ep] == ior) {
                unit->zz_IntPending[ep] = NULL;
                found = 1;
                break;
            }
        }
    }
    ReleaseSemaphore(&ZZBase->zz_Lock);

    if (found) {
        ior->iouh_Actual = 0;
        ior->iouh_Req.io_Error = IOERR_ABORTED;
        if (!(ior->iouh_Req.io_Flags & IOF_QUICK)) {
            ReplyMsg(&ior->iouh_Req.io_Message);
        }
        return IOERR_ABORTED;
    }
    ior->iouh_Req.io_Error = IOERR_ABORTED;
    return IOERR_ABORTED;
}

static uint32_t device_vectors[] = {
    (uint32_t)open,
    (uint32_t)close,
    (uint32_t)expunge,
    0,
    (uint32_t)begin_io,
    (uint32_t)abort_io,
    -1
};

const uint32_t auto_init_tables[4] = {
    sizeof(struct ZZUSBBase),
    (uint32_t)device_vectors,
    0,
    (uint32_t)init_device
};
