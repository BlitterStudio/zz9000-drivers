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
#include <exec/interrupts.h>
#include <hardware/intbits.h>

#include <libraries/expansion.h>

#include <devices/timer.h>
#include <devices/usbhardware.h>

#include <proto/exec.h>
#include <proto/expansion.h>
#include <proto/utility.h>

#include <stdint.h>
#include <string.h>

#include "zzusbhw.h"
#include "zzusb_engine.h"
#include "zzusb_interrupt.h"
#include "zzusb_iso.h"
#include "zzcfg_query.h"

/*
 * Older AmigaOS NDKs stop at UHCMD_BULKXFER. Keep the wire-visible
 * Poseidon RT ISO ABI local so this driver still builds with that SDK.
 */
#ifndef UHCMD_ADDISOHANDLER
struct IOUsbHWRTIso
{
    struct Node *urti_Node;
    struct Hook *urti_InReqHook;
    struct Hook *urti_OutReqHook;
    struct Hook *urti_InDoneHook;
    struct Hook *urti_OutDoneHook;
    ULONG urti_OutPrefetch;
    APTR urti_DriverPrivate1;
    APTR urti_DriverPrivate2;
};

struct IOUsbHWBufferReq
{
    UBYTE *ubr_Buffer;
    ULONG ubr_Length;
    UWORD ubr_Frame;
    UWORD ubr_Flags;
};

#define UHCMD_ADDISOHANDLER (CMD_NONSTD + 7)
#define UHCMD_REMISOHANDLER (CMD_NONSTD + 8)
#define UHCMD_STARTRTISO    (CMD_NONSTD + 9)
#define UHCMD_STOPRTISO     (CMD_NONSTD + 10)
#endif

#ifndef UHIOERR_BABBLE
#define UHIOERR_BABBLE 13
#endif

_Static_assert(ZZUSB_STATUS_OK == ZZUSB_ENGINE_STATUS_OK,
               "engine/proxy status drift");
_Static_assert(ZZUSB_STATUS_PENDING == ZZUSB_ENGINE_STATUS_PENDING,
               "engine/proxy status drift");
_Static_assert(ZZUSB_STATUS_ERROR == ZZUSB_ENGINE_STATUS_ERROR,
               "engine/proxy status drift");
_Static_assert(ZZUSB_STATUS_TIMEOUT == ZZUSB_ENGINE_STATUS_TIMEOUT,
               "engine/proxy status drift");
_Static_assert(ZZUSB_STATUS_STALL == ZZUSB_ENGINE_STATUS_STALL,
               "engine/proxy status drift");
_Static_assert(ZZUSB_STATUS_NAK == ZZUSB_ENGINE_STATUS_NAK,
               "engine/proxy status drift");
_Static_assert(ZZUSB_STATUS_CRC == ZZUSB_ENGINE_STATUS_CRC,
               "engine/proxy status drift");
_Static_assert(ZZUSB_STATUS_BABBLE == ZZUSB_ENGINE_STATUS_BABBLE,
               "engine/proxy status drift");
_Static_assert(ZZUSB_STATUS_OVERRUN == ZZUSB_ENGINE_STATUS_OVERRUN,
               "engine/proxy status drift");
_Static_assert(ZZUSB_STATUS_UNDERRUN == ZZUSB_ENGINE_STATUS_UNDERRUN,
               "engine/proxy status drift");
_Static_assert(ZZUSB_STATUS_OFFLINE == ZZUSB_ENGINE_STATUS_OFFLINE,
               "engine/proxy status drift");
_Static_assert(ZZUSB_STATUS_BADPARAM == ZZUSB_ENGINE_STATUS_BADPARAM,
               "engine/proxy status drift");
_Static_assert(ZZUSB_STATUS_UNSUPPORTED == ZZUSB_ENGINE_STATUS_UNSUPPORTED,
               "engine/proxy status drift");
_Static_assert(ZZUSB_STATUS_STALE == ZZUSB_ENGINE_STATUS_STALE,
               "engine/proxy status drift");
_Static_assert(ZZUSB_STATUS_CANCELLED == ZZUSB_ENGINE_STATUS_CANCELLED,
               "engine/proxy status drift");
_Static_assert(ZZUSB_STATUS_HOSTERROR == ZZUSB_ENGINE_STATUS_HOSTERROR,
               "engine/proxy status drift");
_Static_assert(ZZUSB_STATUS_BUSY == ZZUSB_ENGINE_STATUS_BUSY,
               "engine/proxy status drift");
_Static_assert(ZZUSB_STATUS_NOMEM == ZZUSB_ENGINE_STATUS_NOMEM,
               "engine/proxy status drift");

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
    " (31.08.2026) Poseidon USB driver for ZZ9000 " \
    "(C) Copyright 2026 Dimitris Panokostas"

/* USB request constants (from usb.h) */
#define USR_GET_STATUS        0x00
#define USR_CLEAR_FEATURE     0x01
#define USR_SET_FEATURE       0x03
#define USR_GET_DESCRIPTOR    0x06
#define USR_SET_ADDRESS       0x05
#define USR_GET_CONFIGURATION 0x08
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
#define UPSF_PORT_LOW_SPEED   0x0002
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
#define ZZ_RH_POLL_DELAY_TICKS 10
#define ZZ_INT_PENDING_SLOTS 32
#define ZZ_INT_IDLE_REPLY_POLLS 10
#define ZZ_ABORTED_REPLY_SLOTS (ZZ_INT_PENDING_SLOTS + ZZ_NUM_PORTS + 16)
#ifndef UHCF_USB20
#define UHCF_USB20 (1UL << 0)
#endif
#ifndef UHA_Capabilities
#define UHA_Capabilities (UHA_Dummy + 0x21)
#endif
#ifndef UHCF_ISO
#define UHCF_ISO (1UL << 1)
#endif
#ifndef UHCF_RT_ISO
#define UHCF_RT_ISO (1UL << 2)
#endif
#ifndef UBFF_CONTBUFFER
#define UBFF_CONTBUFFER 1U
#endif
#ifndef UHA_RootHubAddr
#define UHA_RootHubAddr (UHA_Dummy + 0x22)
#endif

static struct ZZUSBBase *PollBase;
static struct Interrupt USBEventInterrupt;
static volatile uint8_t USBEventPending;
static uint8_t USBEventIRQInstalled;
static uint8_t USBEventIRQInt2;
static uint16_t UnitGeneration[ZZ_NUM_PORTS];
struct Library *UtilityBase;

/*
 * Saved seglist for expunge. Stored as a static rather than a struct
 * member because struct ZZUSBBase has a frozen v2.0.0 layout — the
 * hand-written AddTask inline asm in begin_io and the firmware-side
 * tooling expect specific field offsets, and inserting a member
 * shifts everything after it. Single-instance driver, so a static
 * is functionally equivalent.
 */
static uint8_t *DeviceSegList;
static struct IOUsbHWReq *RootHubIntPending[ZZ_NUM_PORTS];
static uint8_t RootHubPollDelay[ZZ_NUM_PORTS];

struct ZZIntPendingSlot {
    struct ZZUSBUnit *unit;
    struct IOUsbHWReq *ior;
    uint8_t armed;
    uint8_t abort_requested;
    uint8_t idle_polls;
    uint8_t rearm_required;
};

static struct ZZIntPendingSlot IntPendingSlots[ZZ_INT_PENDING_SLOTS];

enum ZZUSBProtocolMode {
    ZZUSB_PROTOCOL_LEGACY = 0,
    ZZUSB_PROTOCOL_V2 = 2
};

struct ZZUSBProtocolState {
    volatile uint8_t *registers;
    uint32_t next_request_id;
    uint32_t controller_epoch;
    uint32_t capabilities;
    uint8_t mode;
    uint8_t quarantined;
    uint8_t maintenance_quarantined;
};

static struct ZZUSBProtocolState ProtocolStates[ZZ_NUM_PORTS];

#define ZZ_RT_ISO_SLOTS 8

struct ZZRTIsoBatchContext {
    struct IOUsbHWBufferReq requests[ZZUSB_ISO_MAX_PACKETS];
    uint32_t batch_id;
    uint32_t prefetched_bytes;
    uint8_t packet_count;
};

struct ZZRTIsoSlot {
    struct zzusb_rt_lifecycle lifecycle;
    struct ZZRTIsoBatchContext batches[ZZUSB_ISO_PIPELINE_DEPTH];
    struct ZZUSBUnit *unit;
    struct IOUsbHWRTIso *handler;
    uint16_t packet_lengths[ZZUSB_ISO_MAX_PACKETS];
    uint16_t flags;
    uint16_t generation;
    uint16_t address;
    uint16_t endpoint;
    uint16_t max_packet;
    uint16_t interval;
    uint16_t speed;
    uint16_t split_hub_addr;
    uint16_t split_hub_port;
    uint16_t duration_microframes;
    uint8_t direction_in;
    uint8_t packet_count;
};

static struct ZZRTIsoSlot RTIsoSlots[ZZ_RT_ISO_SLOTS];
static uint8_t IsoWire[ZZUSB_V2_DATA_MAX];
static uint8_t IsoPayload[ZZUSB_ISO_DATA_MAX];
static uint32_t SimpleIsoBatchId = 1;

#define ZZ_WORK_SLOTS 32

struct ZZWorkSlot {
    struct zzusb_engine_request lifecycle;
    struct ZZUSBUnit *unit;
    struct IOUsbHWReq *ior;
    uint32_t sequence;
    uint32_t enqueue_seq;
};

static struct ZZWorkSlot WorkSlots[ZZ_WORK_SLOTS];
static struct zzusb_driver_diag_snapshot DriverDiagSnapshot;
static struct ZZWorkSlot *ActiveWorkSlot;
static uint32_t WorkSequence = 1;
static uint32_t EnqueueSequence = 1;
static struct timerequest *WorkerTimerRequest;
static ULONG WorkerTimerMask;
static uint8_t ProtocolNegotiated;
static volatile uint8_t *ForegroundMailboxBase;
static struct ZZUSBUnit *ForegroundMailboxUnit;

static void execute_io(struct Library *dev, struct IOUsbHWReq *ior);
static int process_work_queue(void);
static int rt_iso_pending_for_unit(const struct ZZUSBUnit *unit);
static uint16_t rt_iso_service_limit_ms(const struct ZZUSBUnit *unit);
static void service_rt_iso_during_work(struct ZZUSBBase *base,
                                       struct ZZUSBUnit *unit);

static uint16_t request_speed(struct ZZUSBUnit *unit, struct IOUsbHWReq *ior);
static int write_tag_ulong(struct TagItem *tag, ULONG value);
static int write_tag_str(struct TagItem *tag, const char *value);

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
 * AmigaOS `version DEVS:USBHardware/zzusbhw.device FILE` scans the
 * binary for a `$VER:` tag and prints the version/revision that
 * follows. Without the tag, `version` falls back to a generic "v1.0"
 * which defeats
 * field verification of driver deployment. Include a (date) after
 * the version number per standard Amiga $VER convention — some
 * tools require it to parse the version correctly.
 */
static const char __attribute__((used)) version_tag[] =
    "$VER: " DEVICE_NAME " " XSTR(DEVICE_VERSION) "." XSTR(DEVICE_REVISION)
    " (31.08.2026) Poseidon USB driver for ZZ9000 "
    "(C) Copyright 2026 Dimitris Panokostas";


static struct ExecBase *get_sysbase(void)
{
    struct ExecBase *sysbase;
    __asm volatile ("move.l 4.w,%0" : "=r"(sysbase));
    return sysbase;
}

/* Push a NUL-terminated string to the ZZ9000 serial debug channel. */
static void dstr(void* regs, char* str)
{
    while (*str) {
        *((volatile uint16_t*)((uint8_t*)regs + 0xF0)) = *str++;
    }
}

static uint32_t active_diag_request_id(void)
{
    return ActiveWorkSlot ? ActiveWorkSlot->lifecycle.request_id : 0U;
}

static uint16_t diag_topology(struct IOUsbHWReq *ior)
{
    return (uint16_t)(((uint16_t)ior->iouh_SplitHubAddr << 8) |
                      ((uint16_t)ior->iouh_SplitHubPort & 0xffU));
}

static void trace_port_state(struct ZZUSBUnit *unit, char *tag)
{
    uint32_t detail = ((uint32_t)unit->zz_PortStatus << 16) |
                      unit->zz_PortChange;
    uint16_t schedule = (uint16_t)unit->zz_Speed |
                        (unit->zz_PortPresent ? 0x0100U : 0U) |
                        (unit->zz_PortDead ? 0x0200U : 0U);

    (void)tag;
    zzusb_engine_diag_record(ZZUSB_DRIVER_EVENT_PORT,
                             ZZUSB_ENGINE_STATUS_OK,
                             active_diag_request_id(),
                             ProtocolStates[0].controller_epoch,
                             0, 0, 0, 0, schedule, detail,
                             WorkSequence);
}

static void trace_port_state_status(struct ZZUSBUnit *unit,
                                    char *tag,
                                    uint16_t status,
                                    uint16_t speed)
{
    uint32_t detail = ((uint32_t)unit->zz_PortStatus << 16) |
                      unit->zz_PortChange;

    (void)tag;
    zzusb_engine_diag_record(ZZUSB_DRIVER_EVENT_PORT, status,
                             active_diag_request_id(),
                             ProtocolStates[0].controller_epoch,
                             0, 0, 0, 0, speed, detail,
                             WorkSequence);
}

static void trace_control_status(struct ZZUSBUnit *unit,
                                 char *tag,
                                 struct IOUsbHWReq *ior,
                                 uint16_t status)
{
    uint32_t detail =
        ((uint32_t)ior->iouh_SetupData.bmRequestType << 24) |
        ((uint32_t)ior->iouh_SetupData.bRequest << 16) |
        ior->iouh_Flags;

    (void)tag;
    zzusb_engine_diag_record(ZZUSB_DRIVER_EVENT_CONTROL, status,
                             active_diag_request_id(),
                             ProtocolStates[0].controller_epoch,
                             ior->iouh_DevAddr, ior->iouh_Endpoint,
                             ior->iouh_Dir == UHDIR_IN ? 0x80U : 0U,
                             diag_topology(ior), ior->iouh_Interval,
                             detail | request_speed(unit, ior),
                             WorkSequence);
}

static void trace_int_status(struct ZZUSBUnit *unit, char *tag,
                             struct IOUsbHWReq *ior)
{
    (void)unit;
    (void)tag;
    zzusb_engine_diag_record(ZZUSB_DRIVER_EVENT_INTERRUPT,
                             ZZUSB_ENGINE_STATUS_PENDING,
                             active_diag_request_id(),
                             ProtocolStates[0].controller_epoch,
                             ior->iouh_DevAddr, ior->iouh_Endpoint,
                             ior->iouh_Dir == UHDIR_IN ? 0x80U : 0U,
                             diag_topology(ior), ior->iouh_Interval,
                             ior->iouh_Length, WorkSequence);
}

static void trace_hub_int_data(struct ZZUSBUnit *unit,
                               struct IOUsbHWReq *ior,
                               uint32_t actual,
                               volatile uint8_t *data)
{
    uint32_t detail;

    (void)unit;
    if (ior->iouh_Dir != UHDIR_IN || ior->iouh_Length > 2 || actual == 0)
        return;
    detail = actual << 16;
    for (uint32_t i = 0; i < actual && i < 2; i++)
        detail |= (uint32_t)data[i] << (8U * (1U - i));
    if ((detail & 0xffffU) == 0)
        return;

    zzusb_engine_diag_record(ZZUSB_DRIVER_EVENT_INTERRUPT,
                             ZZUSB_ENGINE_STATUS_OK,
                             active_diag_request_id(),
                             ProtocolStates[0].controller_epoch,
                             ior->iouh_DevAddr, ior->iouh_Endpoint,
                             0x80U, diag_topology(ior),
                             ior->iouh_Interval, detail,
                             WorkSequence);
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

static void safe_zero(void *dst, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    while (n--) *d++ = 0;
}

static uint16_t int_endpoint_key(struct IOUsbHWReq *ior)
{
    return ior->iouh_Endpoint & 0x0f;
}

static int int_slot_matches(struct ZZIntPendingSlot *slot,
                            struct ZZUSBUnit *unit,
                            struct IOUsbHWReq *ior)
{
    struct IOUsbHWReq *pending = slot->ior;

    return pending &&
        slot->unit == unit &&
        pending->iouh_DevAddr == ior->iouh_DevAddr &&
        int_endpoint_key(pending) == int_endpoint_key(ior) &&
        pending->iouh_Dir == ior->iouh_Dir;
}

static void clear_int_slot(struct ZZIntPendingSlot *slot)
{
    slot->unit = NULL;
    slot->ior = NULL;
    slot->armed = 0;
    slot->abort_requested = 0;
    slot->idle_polls = 0;
    slot->rearm_required = 0;
}

static void reset_int_slots(void)
{
    for (int i = 0; i < ZZ_INT_PENDING_SLOTS; i++)
        clear_int_slot(&IntPendingSlots[i]);
}

static struct ZZIntPendingSlot *find_int_slot_for_ior(struct IOUsbHWReq *ior)
{
    for (int i = 0; i < ZZ_INT_PENDING_SLOTS; i++) {
        if (IntPendingSlots[i].ior == ior)
            return &IntPendingSlots[i];
    }
    return NULL;
}

static int queue_int_ior(struct ZZUSBUnit *unit,
                         struct IOUsbHWReq *ior,
                         struct IOUsbHWReq **replaced)
{
    struct ZZIntPendingSlot *free_slot = NULL;

    if (replaced)
        *replaced = NULL;

    for (int i = 0; i < ZZ_INT_PENDING_SLOTS; i++) {
        struct ZZIntPendingSlot *slot = &IntPendingSlots[i];
        if (int_slot_matches(slot, unit, ior)) {
            if (slot->ior && slot->ior != ior) {
                if (replaced)
                    *replaced = slot->ior;
                slot->ior->iouh_Actual = 0;
                slot->ior->iouh_Req.io_Error = IOERR_ABORTED;
                if (zzusb_interrupt_rearm_on_replace(
                        slot->armed, ior->iouh_Dir == UHDIR_IN))
                    slot->rearm_required = 1;
            }
            slot->unit = unit;
            slot->ior = ior;
            slot->abort_requested = 0;
            slot->idle_polls = 0;
            return 1;
        }
        if (!slot->ior && !free_slot)
            free_slot = slot;
    }

    if (!free_slot)
        return 0;

    free_slot->unit = unit;
    free_slot->ior = ior;
    free_slot->armed = 0;
    free_slot->abort_requested = 0;
    free_slot->idle_polls = 0;
    return 1;
}

static int int_pending_for_unit(struct ZZUSBUnit *unit)
{
    for (int i = 0; i < ZZ_INT_PENDING_SLOTS; i++) {
        if (IntPendingSlots[i].ior && IntPendingSlots[i].unit == unit)
            return 1;
    }
    return 0;
}

static uint16_t generation_for_unit(struct ZZUSBUnit *unit)
{
    if (PollBase) {
        for (int i = 0; i < ZZ_NUM_PORTS; i++)
            if (&PollBase->zz_Units[i] == unit)
                return UnitGeneration[i] ? UnitGeneration[i] : 1;
    }
    return 1;
}

static void bump_unit_generation(struct ZZUSBUnit *unit)
{
    if (!PollBase)
        return;
    for (int i = 0; i < ZZ_NUM_PORTS; i++) {
        if (&PollBase->zz_Units[i] != unit)
            continue;
        UnitGeneration[i]++;
        if (!UnitGeneration[i])
            UnitGeneration[i] = 1;
        for (int slot = 0; slot < ZZ_INT_PENDING_SLOTS; slot++)
            if (IntPendingSlots[slot].unit == unit)
                IntPendingSlots[slot].armed = 0;
        return;
    }
}

static int is_addr0_ep0(struct IOUsbHWReq *ior)
{
    return ior->iouh_DevAddr == 0 && ior->iouh_Endpoint == 0;
}

static int is_direct_root_request(struct ZZUSBUnit *unit)
{
    /*
     * Address zero is also used while enumerating devices behind an
     * external high-speed hub. In that case Poseidon supplies valid split
     * information and the root port itself remains high-speed. Only force
     * root-port handling when the physical root port is already known to be
     * FS/LS; this applies to every endpoint, not just address-zero EP0.
     */
    return unit->zz_PortPresent &&
           unit->zz_Speed != ZZUSB_SPEED_HIGH;
}

static int is_direct_root_addr0(struct ZZUSBUnit *unit, struct IOUsbHWReq *ior)
{
    return is_addr0_ep0(ior) && is_direct_root_request(unit);
}

static int is_addr0_get_device_desc(struct IOUsbHWReq *ior)
{
    return is_addr0_ep0(ior) &&
           ior->iouh_SetupData.bmRequestType == 0x80 &&
           ior->iouh_SetupData.bRequest == 0x06 &&
           SWAP16(ior->iouh_SetupData.wValue) == 0x0100;
}

static uint16_t request_speed(struct ZZUSBUnit *unit, struct IOUsbHWReq *ior)
{
    if (is_direct_root_request(unit))
        return unit->zz_Speed;
    if (ior->iouh_Flags & UHFF_SPLITTRANS) {
        return (ior->iouh_Flags & UHFF_LOWSPEED)
               ? ZZUSB_SPEED_LOW : ZZUSB_SPEED_FULL;
    }
    if (ior->iouh_Flags & UHFF_LOWSPEED)
        return ZZUSB_SPEED_LOW;
    if (ior->iouh_Flags & UHFF_HIGHSPEED)
        return ZZUSB_SPEED_HIGH;
    return unit->zz_Speed;
}

static void fill_split_fields(struct ZZUSBCommand *cmd,
                              struct ZZUSBUnit *unit,
                              struct IOUsbHWReq *ior)
{
    if (is_direct_root_request(unit))
        return;

    if ((ior->iouh_Flags & UHFF_SPLITTRANS) &&
        ior->iouh_SplitHubAddr != 0 &&
        ior->iouh_SplitHubPort != 0) {
        cmd->split_hub_addr = ior->iouh_SplitHubAddr;
        cmd->split_hub_port = ior->iouh_SplitHubPort;
        cmd->flags |= ZZUSB_FLAG_SPLIT;
    }
}

static void fill_root_reset_hint(struct ZZUSBCommand *cmd,
                                 struct ZZUSBUnit *unit)
{
    cmd->speed = unit->zz_Speed;
    /*
     * Full-speed and high-speed devices both present J before reset. Forcing
     * PFSC on a full-speed pre-reset hint prevents high-speed chirp, so only
     * force the FS/LS reset path for a confirmed low-speed root attach.
     */
    if (unit->zz_PortPresent && unit->zz_Speed == ZZUSB_SPEED_LOW)
        cmd->flags |= ZZUSB_FLAG_RESET_FSLS;
}

static void mark_direct_low_speed_unsupported(struct ZZUSBUnit *unit,
                                              char *tag)
{
    unit->zz_PortDead = TRUE;
    unit->zz_PortPresent = FALSE;
    unit->zz_Speed = ZZUSB_SPEED_LOW;
    unit->zz_PortStatus = UPSF_PORT_POWER;
    unit->zz_PortChange = UPSF_C_PORT_CONNECTION;
    unit->zz_BulkErrCount = 0;
    trace_port_state(unit, tag);
}

static int is_zero_report(volatile uint8_t *buf, uint32_t len)
{
    while (len--) {
        if (*buf++ != 0)
            return 0;
    }
    return 1;
}

static struct ZZUSBProtocolState *protocol_state_for(
    volatile uint8_t *base)
{
    for (int i = 0; i < ZZ_NUM_PORTS; ++i) {
        if (ProtocolStates[i].registers == base)
            return &ProtocolStates[i];
    }
    return NULL;
}

static uint32_t next_request_id(struct ZZUSBProtocolState *state)
{
    uint32_t id = state->next_request_id++;

    if (id == 0) {
        id = 1;
        state->next_request_id = 2;
    }
    return id;
}

static int worker_wait_us(uint32_t microseconds)
{
    struct timerequest *request = WorkerTimerRequest;

    if (!request || !WorkerTimerMask)
        return 0;
    SetSignal(0, WorkerTimerMask);
    request->tr_node.io_Command = TR_ADDREQUEST;
    request->tr_time.tv_secs = microseconds / 1000000UL;
    request->tr_time.tv_micro = microseconds % 1000000UL;
    SendIO((struct IORequest *)request);
    Wait(WorkerTimerMask);
    WaitIO((struct IORequest *)request);
    return 1;
}

static int worker_wait_iso_event(uint32_t microseconds)
{
    struct timerequest *request = WorkerTimerRequest;
    ULONG event_mask = PollBase ? PollBase->zz_PollSignal : 0;
    int timer_completed;

    if (!request || !WorkerTimerMask)
        return -1;
    SetSignal(0, WorkerTimerMask);
    request->tr_node.io_Command = TR_ADDREQUEST;
    request->tr_time.tv_secs = microseconds / 1000000UL;
    request->tr_time.tv_micro = microseconds % 1000000UL;
    SendIO((struct IORequest *)request);
    Wait(WorkerTimerMask | event_mask);
    timer_completed = CheckIO((struct IORequest *)request) != NULL;
    if (!timer_completed)
        AbortIO((struct IORequest *)request);
    WaitIO((struct IORequest *)request);
    return timer_completed;
}

static int active_work_aborted(void)
{
    return ActiveWorkSlot &&
           ActiveWorkSlot->lifecycle.abort_requested;
}

static int send_usb_cmd_wire(volatile uint8_t *base,
                             struct ZZUSBCommand *cmd,
                             void *data_out, uint32_t data_out_len,
                             struct ZZUSBProtocolState *state,
                             int use_v2, int is_query, int honor_abort)
{
    volatile struct ZZUSBCommand *result =
        (volatile struct ZZUSBCommand*)(base + 0xa000);
    volatile struct ZZUSBProtocolExtension *result_ext =
        (volatile struct ZZUSBProtocolExtension*)
            (base + 0xa000 + ZZUSB_CMD_SIZE);
    struct ZZUSBProtocolExtension ext;
    uint32_t request_id = 0;
    uint32_t max_data = use_v2 ? ZZUSB_V2_DATA_MAX : ZZUSB_MAX_XFER;
    uint32_t elapsed_ms;
    uint32_t outer_limit_ms;
    uint16_t service_elapsed_ms = 0;

    if (data_out_len > max_data)
        return ZZUSB_STATUS_BADPARAM;
    if (!WorkerTimerRequest || !WorkerTimerMask)
        return ZZUSB_STATUS_HOSTERROR;

    switch (cmd->cmd) {
    case ZZUSB_CMD_CONTROL_XFER:
        cmd->xfer_type = ZZUSB_XFER_CONTROL;
        break;
    case ZZUSB_CMD_BULK_XFER:
        cmd->xfer_type = ZZUSB_XFER_BULK;
        break;
    case ZZUSB_CMD_INT_XFER:
        cmd->xfer_type = ZZUSB_XFER_INTERRUPT;
        break;
    case ZZUSB_CMD_PERIODIC_ARM:
    case ZZUSB_CMD_PERIODIC_REAP:
    case ZZUSB_CMD_PERIODIC_STOP:
        cmd->xfer_type = ZZUSB_XFER_INTERRUPT;
        break;
    case ZZUSB_CMD_ISO_XFER:
    case ZZUSB_CMD_ISO_QUEUE:
    case ZZUSB_CMD_ISO_REAP:
    case ZZUSB_CMD_ISO_STOP:
        cmd->xfer_type = ZZUSB_XFER_ISO;
        break;
    default:
        break;
    }

    cmd->status = ZZUSB_STATUS_PENDING;
    safe_copy(cmd, (void*)(base + 0xa000), ZZUSB_CMD_SIZE);

    memset(&ext, 0, sizeof(ext));
    if (use_v2) {
        request_id = next_request_id(state);
        ext.version = ZZUSB_PROTOCOL_VERSION;
        ext.header_size = ZZUSB_V2_HEADER_SIZE;
        ext.request_id = request_id;
        ext.controller_epoch = is_query ? 0 : state->controller_epoch;
        ext.capabilities = state->capabilities;
        safe_copy(&ext, (void*)(base + 0xa000 + ZZUSB_CMD_SIZE),
                  sizeof(ext));
    }

    if (data_out && data_out_len > 0) {
        safe_copy(data_out,
                  (void*)(base + 0xa000 + ZZUSB_DATA_OFFSET),
                  data_out_len);
    }

    CacheClearU();
    *((volatile uint16_t*)(base + ZZ_REG_USB_PROXY_CMD)) =
        cmd->cmd | (use_v2 ? ZZUSB_DOORBELL_V2 : 0);

    {
        uint32_t firmware_ms = cmd->timeout_ms
                               ? (uint32_t)cmd->timeout_ms
                               : ZZUSB_PROXY_MAX_TIMEOUT_MS;
        outer_limit_ms = firmware_ms + 150U;

        for (elapsed_ms = 0; elapsed_ms < outer_limit_ms; elapsed_ms++) {
            CacheClearE((APTR)result,
                        use_v2 ? ZZUSB_V2_HEADER_SIZE : ZZUSB_CMD_SIZE,
                        CACRF_ClearD);
            if (result->status != ZZUSB_STATUS_PENDING)
                break;
            if (honor_abort && active_work_aborted())
                break;
            if (!worker_wait_us(1000U))
                break;
            if (ForegroundMailboxBase == base && ForegroundMailboxUnit &&
                rt_iso_pending_for_unit(ForegroundMailboxUnit) &&
                ++service_elapsed_ms >=
                    rt_iso_service_limit_ms(ForegroundMailboxUnit)) {
                service_rt_iso_during_work(PollBase,
                                           ForegroundMailboxUnit);
                service_elapsed_ms = 0;
            }
        }
    }

    CacheClearE((APTR)result,
                use_v2 ? ZZUSB_V2_HEADER_SIZE : ZZUSB_CMD_SIZE,
                CACRF_ClearD);
    if (result->status == ZZUSB_STATUS_PENDING) {
        if (honor_abort && active_work_aborted()) {
            while (result->status == ZZUSB_STATUS_PENDING &&
                   elapsed_ms < outer_limit_ms) {
                if (!worker_wait_us(1000U))
                    break;
                elapsed_ms++;
                if (ForegroundMailboxBase == base &&
                    ForegroundMailboxUnit &&
                    rt_iso_pending_for_unit(ForegroundMailboxUnit) &&
                    ++service_elapsed_ms >=
                        rt_iso_service_limit_ms(ForegroundMailboxUnit)) {
                    service_rt_iso_during_work(PollBase,
                                               ForegroundMailboxUnit);
                    service_elapsed_ms = 0;
                }
                CacheClearE((APTR)result,
                            use_v2 ? ZZUSB_V2_HEADER_SIZE : ZZUSB_CMD_SIZE,
                            CACRF_ClearD);
            }
            if (result->status == ZZUSB_STATUS_PENDING && state)
                state->quarantined = 1;
            zzusb_engine_diag_count(ZZUSB_DRIVER_COUNT_CANCELLATION);
            zzusb_engine_diag_record(ZZUSB_DRIVER_EVENT_MAILBOX,
                                     ZZUSB_ENGINE_STATUS_CANCELLED,
                                     request_id, state->controller_epoch,
                                     (uint16_t)cmd->dev_addr,
                                     (uint8_t)cmd->endpoint,
                                     (uint8_t)cmd->direction,
                                     (uint16_t)((cmd->split_hub_addr << 8) |
                                                (cmd->split_hub_port & 0xffU)),
                                     cmd->flags, cmd->timeout_ms,
                                     WorkSequence);
            return ZZUSB_STATUS_CANCELLED;
        }
        if (state)
            state->quarantined = 1;
        zzusb_engine_diag_count(ZZUSB_DRIVER_COUNT_TIMEOUT);
        zzusb_engine_diag_record(ZZUSB_DRIVER_EVENT_MAILBOX,
                                 ZZUSB_ENGINE_STATUS_TIMEOUT,
                                 request_id, state->controller_epoch,
                                 (uint16_t)cmd->dev_addr,
                                 (uint8_t)cmd->endpoint,
                                 (uint8_t)cmd->direction,
                                 (uint16_t)((cmd->split_hub_addr << 8) |
                                            (cmd->split_hub_port & 0xffU)),
                                 cmd->flags, cmd->timeout_ms,
                                 WorkSequence);
        return ZZUSB_STATUS_TIMEOUT;
    }
    {
        uint32_t response_length = result->actual_length;

        if (response_length > max_data)
            response_length = max_data;
        if (response_length)
            CacheClearE((APTR)(base + 0xa000 + ZZUSB_DATA_OFFSET),
                        response_length, CACRF_ClearD);
    }

    if (use_v2) {
        uint32_t response_id = result_ext->request_id;
        uint32_t response_epoch = result_ext->controller_epoch;

        if (response_id != request_id) {
            state->quarantined = 1;
            zzusb_engine_diag_count(ZZUSB_DRIVER_COUNT_LATE_COMPLETION);
            zzusb_engine_diag_record(ZZUSB_DRIVER_EVENT_LATE_COMPLETION,
                                     ZZUSB_ENGINE_STATUS_STALE,
                                     response_id, response_epoch,
                                     (uint16_t)cmd->dev_addr,
                                     (uint8_t)cmd->endpoint,
                                     (uint8_t)cmd->direction,
                                     0, cmd->flags, request_id,
                                     WorkSequence);
            return ZZUSB_STATUS_STALE;
        }
        if (!is_query && response_epoch != state->controller_epoch) {
            if (cmd->cmd == ZZUSB_CMD_RESET_PORT ||
                result->status == ZZUSB_STATUS_STALE) {
                state->controller_epoch = response_epoch;
            } else {
                state->quarantined = 1;
                zzusb_engine_diag_count(ZZUSB_DRIVER_COUNT_STALE);
                zzusb_engine_diag_record(ZZUSB_DRIVER_EVENT_STALE,
                                         ZZUSB_ENGINE_STATUS_STALE,
                                         response_id, response_epoch,
                                         (uint16_t)cmd->dev_addr,
                                         (uint8_t)cmd->endpoint,
                                         (uint8_t)cmd->direction,
                                         0, cmd->flags,
                                         state->controller_epoch,
                                         WorkSequence);
                return ZZUSB_STATUS_STALE;
            }
        }
        if (is_query)
            state->controller_epoch = response_epoch;
        state->capabilities = result_ext->capabilities;
        if (result->status == ZZUSB_STATUS_STALE) {
            zzusb_engine_diag_count(ZZUSB_DRIVER_COUNT_STALE);
            zzusb_engine_diag_record(ZZUSB_DRIVER_EVENT_STALE,
                                     ZZUSB_ENGINE_STATUS_STALE,
                                     response_id, response_epoch,
                                     (uint16_t)cmd->dev_addr,
                                     (uint8_t)cmd->endpoint,
                                     (uint8_t)cmd->direction,
                                     0, cmd->flags, 0, WorkSequence);
        }
    }

    return result->status;
}

static int send_usb_cmd_sideband(
    volatile uint8_t *base, struct ZZUSBCommand *cmd,
    void *data_out, uint32_t data_out_len,
    void *data_in, uint32_t data_in_capacity,
    struct ZZUSBProtocolState *state)
{
    volatile struct ZZUSBCommand *result =
        (volatile struct ZZUSBCommand *)
            (base + 0xa000 + ZZUSB_MAINT_HEADER_OFFSET);
    volatile struct ZZUSBProtocolExtension *result_ext =
        (volatile struct ZZUSBProtocolExtension *)
            (base + 0xa000 + ZZUSB_MAINT_HEADER_OFFSET + ZZUSB_CMD_SIZE);
    struct ZZUSBProtocolExtension ext;
    uint32_t request_id;
    uint32_t elapsed_ms;
    uint32_t outer_limit_ms;
    uint32_t response_length;
    uint16_t status;

    if (!state || state->mode != ZZUSB_PROTOCOL_V2 ||
        !(state->capabilities & ZZUSB_CAP_MAINTENANCE) ||
        state->quarantined || !WorkerTimerRequest || !WorkerTimerMask)
        return ZZUSB_STATUS_HOSTERROR;
    if (data_out_len > ZZUSB_MAINT_DATA_MAX ||
        cmd->data_length > ZZUSB_MAINT_DATA_MAX)
        return ZZUSB_STATUS_BADPARAM;

    /*
     * The maintenance mailbox has no doorbell. Publish its payload and
     * identity while status is terminal, then publish PENDING last so the
     * firmware cannot consume a partially written command.
     */
    cmd->xfer_type = ZZUSB_XFER_ISO;
    cmd->status = ZZUSB_STATUS_OK;
    safe_copy(cmd, (void *)result, ZZUSB_CMD_SIZE);
    memset(&ext, 0, sizeof(ext));
    request_id = next_request_id(state);
    ext.version = ZZUSB_PROTOCOL_VERSION;
    ext.header_size = ZZUSB_V2_HEADER_SIZE;
    ext.request_id = request_id;
    ext.controller_epoch = state->controller_epoch;
    ext.capabilities = state->capabilities;
    safe_copy(&ext, (void *)result_ext, sizeof(ext));
    if (data_out && data_out_len)
        safe_copy(data_out,
                  (void *)(base + 0xa000 + ZZUSB_MAINT_DATA_OFFSET),
                  data_out_len);
    CacheClearU();
    result->status = ZZUSB_STATUS_PENDING;
    cmd->status = ZZUSB_STATUS_PENDING;
    CacheClearU();

    outer_limit_ms = (cmd->timeout_ms ? cmd->timeout_ms :
                      ZZUSB_PROXY_MAX_TIMEOUT_MS) + 150U;
    for (elapsed_ms = 0; elapsed_ms < outer_limit_ms; elapsed_ms++) {
        CacheClearE((APTR)result, ZZUSB_V2_HEADER_SIZE, CACRF_ClearD);
        if (result->status != ZZUSB_STATUS_PENDING)
            break;
        if (!worker_wait_us(1000U))
            break;
    }
    CacheClearE((APTR)result, ZZUSB_V2_HEADER_SIZE, CACRF_ClearD);
    if (result->status == ZZUSB_STATUS_PENDING) {
        state->maintenance_quarantined = 1;
        state->quarantined = 1;
        zzusb_engine_diag_count(ZZUSB_DRIVER_COUNT_TIMEOUT);
        zzusb_engine_diag_record(
            ZZUSB_DRIVER_EVENT_MAILBOX, ZZUSB_ENGINE_STATUS_TIMEOUT,
            request_id, state->controller_epoch, (uint16_t)cmd->dev_addr,
            (uint8_t)cmd->endpoint, (uint8_t)cmd->direction,
            (uint16_t)((cmd->split_hub_addr << 8) |
                       (cmd->split_hub_port & 0xffU)),
            cmd->flags, cmd->timeout_ms, WorkSequence);
        return ZZUSB_STATUS_TIMEOUT;
    }
    if (result_ext->request_id != request_id ||
        result_ext->controller_epoch != state->controller_epoch) {
        state->maintenance_quarantined = 1;
        state->quarantined = 1;
        zzusb_engine_diag_count(ZZUSB_DRIVER_COUNT_LATE_COMPLETION);
        return ZZUSB_STATUS_STALE;
    }

    response_length = result->actual_length;
    if (response_length > ZZUSB_MAINT_DATA_MAX ||
        response_length > data_in_capacity ||
        (response_length && !data_in)) {
        state->maintenance_quarantined = 1;
        state->quarantined = 1;
        return ZZUSB_STATUS_HOSTERROR;
    }
    if (response_length) {
        CacheClearE((APTR)(base + 0xa000 + ZZUSB_MAINT_DATA_OFFSET),
                    response_length, CACRF_ClearD);
        safe_copy((void *)(base + 0xa000 + ZZUSB_MAINT_DATA_OFFSET),
                  data_in, response_length);
    }
    state->capabilities = result_ext->capabilities;
    status = result->status;
    safe_copy((void *)result, cmd, ZZUSB_CMD_SIZE);
    return status;
}


static int negotiate_usb_proxy(volatile uint8_t *base,
                               struct ZZUSBProtocolState *state,
                               int allow_legacy)
{
    struct ZZUSBCommand query;
    int status;
    int unsafe;

    memset(state, 0, sizeof(*state));
    state->registers = base;
    state->next_request_id = 1;
    state->mode = ZZUSB_PROTOCOL_LEGACY;

    memset(&query, 0, sizeof(query));
    query.cmd = ZZUSB_CMD_QUERY_CAPS;
    query.timeout_ms = 100;
    status = send_usb_cmd_wire(base, &query, NULL, 0, state, 1, 1, 0);
    unsafe = state->quarantined;
    if (status == ZZUSB_STATUS_OK &&
        state->controller_epoch != 0 &&
        (state->capabilities & ZZUSB_CAP_BASE) == ZZUSB_CAP_BASE) {
        state->mode = ZZUSB_PROTOCOL_V2;
        state->quarantined = 0;
        return 1;
    }

    state->next_request_id = 1;
    state->controller_epoch = 0;
    state->capabilities = 0;
    state->mode = ZZUSB_PROTOCOL_LEGACY;
    state->quarantined = unsafe || !allow_legacy;
    return 0;
}

/*
 * Never overwrite a quarantined primary or maintenance mailbox. Once every
 * in-flight command is terminal, a v2 QUERY_CAPS establishes a fresh
 * request-ID/epoch fence. Recovery never falls back to unfenced legacy
 * traffic.
 */
static void recover_quarantined_proxy(volatile uint8_t *base)
{
    struct ZZUSBProtocolState *state = protocol_state_for(base);
    volatile struct ZZUSBCommand *result =
        (volatile struct ZZUSBCommand *)(base + 0xa000);
    volatile struct ZZUSBCommand *maintenance =
        (volatile struct ZZUSBCommand *)
            (base + 0xa000 + ZZUSB_MAINT_HEADER_OFFSET);

    if (!state || !state->quarantined)
        return;
    if (state->maintenance_quarantined) {
        CacheClearE((APTR)maintenance, ZZUSB_V2_HEADER_SIZE,
                    CACRF_ClearD);
        if (maintenance->status == ZZUSB_STATUS_PENDING)
            return;
        state->maintenance_quarantined = 0;
    }
    CacheClearE((APTR)result, ZZUSB_V2_HEADER_SIZE, CACRF_ClearD);
    if (result->status == ZZUSB_STATUS_PENDING)
        return;
    negotiate_usb_proxy(base, state, 0);
}

static int send_usb_cmd_scoped(volatile uint8_t *base,
                               struct ZZUSBCommand *cmd,
                               void *data_out, uint32_t data_out_len,
                               int honor_abort)
{
    struct ZZUSBProtocolState *state = protocol_state_for(base);

    if (state && state->quarantined)
        return ZZUSB_STATUS_HOSTERROR;
    return send_usb_cmd_wire(
        base, cmd, data_out, data_out_len, state,
        state && state->mode == ZZUSB_PROTOCOL_V2, 0, honor_abort);
}

static int send_usb_cmd(volatile uint8_t *base, struct ZZUSBCommand *cmd,
                        void *data_out, uint32_t data_out_len)
{
    return send_usb_cmd_scoped(base, cmd, data_out, data_out_len, 1);
}

static int send_usb_cmd_maintenance(
    volatile uint8_t *base, struct ZZUSBCommand *cmd,
    void *data_out, uint32_t data_out_len,
    void *data_in, uint32_t data_in_capacity)
{
    struct ZZUSBProtocolState *state = protocol_state_for(base);
    volatile struct ZZUSBCommand *result;
    uint32_t response_length;
    int status;

    if (ForegroundMailboxBase == base)
        return send_usb_cmd_sideband(
            base, cmd, data_out, data_out_len,
            data_in, data_in_capacity, state);

    status = send_usb_cmd_scoped(base, cmd, data_out, data_out_len, 0);
    if (status != ZZUSB_STATUS_OK)
        return status;
    result = (volatile struct ZZUSBCommand *)(base + 0xa000);
    response_length = result->actual_length;
    if (response_length > data_in_capacity ||
        (response_length && !data_in)) {
        if (state)
            state->quarantined = 1;
        return ZZUSB_STATUS_HOSTERROR;
    }
    if (response_length)
        safe_copy((void *)(base + 0xa000 + ZZUSB_DATA_OFFSET),
                  data_in, response_length);
    safe_copy((void *)result, cmd, ZZUSB_CMD_SIZE);
    return status;
}

static int periodic_stop_retired(uint16_t status)
{
    return status == ZZUSB_STATUS_OK ||
           status == ZZUSB_STATUS_NAK ||
           status == ZZUSB_STATUS_OFFLINE ||
           status == ZZUSB_STATUS_STALE;
}

static uint16_t stop_periodic_slot(struct ZZIntPendingSlot *slot)
{
    struct ZZUSBCommand cmd;
    struct ZZUSBProtocolState *state;
    volatile uint8_t *base;
    struct IOUsbHWReq *ior;
    uint16_t status;

    if (!slot || !slot->armed)
        return ZZUSB_STATUS_OK;
    if (!slot->unit || !slot->ior)
        return ZZUSB_STATUS_BADPARAM;

    ior = slot->ior;
    base = (volatile uint8_t *)slot->unit->zz_Registers;
    memset(&cmd, 0, sizeof(cmd));
    cmd.cmd = ZZUSB_CMD_PERIODIC_STOP;
    cmd.dev_addr = ior->iouh_DevAddr;
    cmd.endpoint = ior->iouh_Endpoint;
    cmd.direction = (ior->iouh_Dir == UHDIR_IN) ? 0x80 : 0x00;
    cmd.max_pkt_size = ior->iouh_MaxPktSize;
    cmd.speed = request_speed(slot->unit, ior);
    cmd.interval = ior->iouh_Interval;
    cmd.reserved = generation_for_unit(slot->unit);
    cmd.timeout_ms = 100;
    fill_split_fields(&cmd, slot->unit, ior);
    status = send_usb_cmd(base, &cmd, NULL, 0);
    slot->armed = 0;
    if (!periodic_stop_retired(status)) {
        state = protocol_state_for(base);
        if (state)
            state->quarantined = 1;
    }
    return status;
}

static BYTE map_proxy_status(uint16_t status)
{
    switch (zzusb_engine_classify_status(status)) {
    case ZZUSB_ERROR_NONE:          return UHIOERR_NO_ERROR;
    case ZZUSB_ERROR_NAK:           return UHIOERR_NAK;
    case ZZUSB_ERROR_STALL:         return UHIOERR_STALL;
    case ZZUSB_ERROR_TIMEOUT:       return UHIOERR_TIMEOUT;
    case ZZUSB_ERROR_OFFLINE:       return UHIOERR_USBOFFLINE;
    case ZZUSB_ERROR_CRC:           return UHIOERR_CRCERROR;
    case ZZUSB_ERROR_BABBLE:        return UHIOERR_BABBLE;
    case ZZUSB_ERROR_OVERFLOW:      return UHIOERR_OVERFLOW;
    case ZZUSB_ERROR_UNDERFLOW:     return UHIOERR_RUNTPACKET;
    case ZZUSB_ERROR_BAD_PARAMETER:
    case ZZUSB_ERROR_UNSUPPORTED:   return UHIOERR_BADPARAMS;
    case ZZUSB_ERROR_NO_MEMORY:     return UHIOERR_OUTOFMEMORY;
    case ZZUSB_ERROR_CANCELLED:     return IOERR_ABORTED;
    case ZZUSB_ERROR_HOST:
    default:                        return UHIOERR_HOSTERROR;
    }
}

static ULONG iso_public_capabilities(struct ZZUSBUnit *unit)
{
    struct ZZUSBProtocolState *state;
    ULONG capabilities = UHCF_USB20;

    if (!unit)
        return capabilities;
    state = protocol_state_for((volatile uint8_t *)unit->zz_Registers);
    if (!state || state->mode != ZZUSB_PROTOCOL_V2 || state->quarantined)
        return capabilities;
    if (state->capabilities & ZZUSB_CAP_ISO_SIMPLE)
        capabilities |= UHCF_ISO;
    if (UtilityBase &&
        (state->capabilities & ZZUSB_CAP_ISO_REALTIME))
        capabilities |= UHCF_RT_ISO;
    return capabilities;
}

static uint16_t iso_encoded_max_packet(const struct IOUsbHWReq *ior)
{
    uint16_t encoded = ior->iouh_MaxPktSize & 0x07ffU;

    if (!(ior->iouh_Flags & UHFF_HIGHSPEED))
        return encoded;
    if ((ior->iouh_Flags & UHFF_MULTI_3) == UHFF_MULTI_3)
        encoded |= 2U << 11;
    else if (ior->iouh_Flags & UHFF_MULTI_2)
        encoded |= 1U << 11;
    return encoded;
}

static void fill_iso_command(struct ZZUSBCommand *cmd,
                             struct ZZUSBUnit *unit,
                             const struct IOUsbHWReq *ior,
                             uint16_t command, uint32_t data_length)
{
    memset(cmd, 0, sizeof(*cmd));
    cmd->cmd = command;
    cmd->dev_addr = ior->iouh_DevAddr;
    cmd->endpoint = ior->iouh_Endpoint;
    cmd->direction = ior->iouh_Dir == UHDIR_IN ? 0x80 : 0;
    cmd->max_pkt_size = iso_encoded_max_packet(ior);
    cmd->data_length = data_length;
    cmd->timeout_ms = 100;
    cmd->speed = request_speed(unit, (struct IOUsbHWReq *)ior);
    cmd->interval = ior->iouh_Interval;
    cmd->reserved = generation_for_unit(unit);
    fill_split_fields(cmd, unit, (struct IOUsbHWReq *)ior);
}

static BYTE map_iso_packet_status(uint8_t status, UWORD request_flags)
{
    switch (status) {
    case ZZUSB_ISO_PACKET_OK:
        return 0;
    case ZZUSB_ISO_PACKET_SHORT:
        return (request_flags & UHFF_ALLOWRUNTPKTS) ?
               0 : UHIOERR_RUNTPACKET;
    case ZZUSB_ISO_PACKET_MISSED:
        return UHIOERR_TIMEOUT;
    case ZZUSB_ISO_PACKET_UNDERRUN:
        return UHIOERR_RUNTPACKET;
    case ZZUSB_ISO_PACKET_OVERRUN:
        return UHIOERR_OVERFLOW;
    case ZZUSB_ISO_PACKET_CANCELLED:
        return IOERR_ABORTED;
    case ZZUSB_ISO_PACKET_OFFLINE:
        return UHIOERR_USBOFFLINE;
    case ZZUSB_ISO_PACKET_BABBLE:
        return UHIOERR_BABBLE;
    default:
        return UHIOERR_HOSTERROR;
    }
}

static uint16_t stop_iso_request(volatile uint8_t *base,
                                 struct ZZUSBUnit *unit,
                                 const struct IOUsbHWReq *ior)
{
    struct ZZUSBCommand cmd;

    fill_iso_command(&cmd, unit, ior, ZZUSB_CMD_ISO_STOP, 0);
    return send_usb_cmd(base, &cmd, NULL, 0);
}

static void execute_simple_iso(struct ZZUSBUnit *unit,
                               struct IOUsbHWReq *ior,
                               volatile uint8_t *base)
{
    struct ZZUSBCommand cmd;
    struct zzusb_iso_batch_result batch;
    struct zzusb_iso_packet_result packets[ZZUSB_ISO_MAX_PACKETS];
    uint16_t packet_lengths[ZZUSB_ISO_MAX_PACKETS];
    uint32_t batch_id;
    uint16_t status;
    uint32_t total_actual = 0;
    uint32_t timeout_ms;
    uint32_t elapsed_ms = 0;
    unsigned packet_count;
    unsigned wire_length;
    int queued = 0;

    ior->iouh_Actual = 0;
    ior->iouh_ExtError = 0;
    if (!(iso_public_capabilities(unit) & UHCF_ISO) ||
        (ior->iouh_Flags & UHFF_LOWSPEED) ||
        !ior->iouh_Endpoint || !ior->iouh_Interval ||
        (ior->iouh_Length && !ior->iouh_Data)) {
        ior->iouh_Req.io_Error = UHIOERR_BADPARAMS;
        return;
    }

    packet_count = zzusb_iso_plan_simple(
        ior->iouh_Length, iso_encoded_max_packet(ior),
        packet_lengths, ZZUSB_ISO_MAX_PACKETS);
    if (!packet_count) {
        ior->iouh_Req.io_Error =
            ior->iouh_Length > ZZUSB_ISO_DATA_MAX ?
            UHIOERR_PKTTOOLARGE : UHIOERR_BADPARAMS;
        return;
    }
    batch_id = SimpleIsoBatchId++;
    if (!batch_id) {
        batch_id = 1;
        SimpleIsoBatchId = 2;
    }
    wire_length = zzusb_iso_build_queue(
        IsoWire, sizeof(IsoWire), batch_id,
        ior->iouh_Frame ? 0 : ZZUSB_ISO_FLAG_ASAP,
        ior->iouh_Frame, 0, packet_lengths, packet_count,
        (const uint8_t *)ior->iouh_Data,
        ior->iouh_Dir == UHDIR_IN);
    if (!wire_length) {
        ior->iouh_Req.io_Error = UHIOERR_BADPARAMS;
        return;
    }

    fill_iso_command(&cmd, unit, ior, ZZUSB_CMD_ISO_QUEUE, wire_length);
    status = send_usb_cmd(base, &cmd, IsoWire, wire_length);
    if (status != ZZUSB_STATUS_OK) {
        if (status == ZZUSB_STATUS_HOSTERROR)
            stop_iso_request(base, unit, ior);
        ior->iouh_Req.io_Error = map_proxy_status(status);
        return;
    }
    queued = 1;
    timeout_ms = (ior->iouh_Flags & UHFF_NAKTIMEOUT) ?
                 (ior->iouh_NakTimeout ? ior->iouh_NakTimeout : 1000U) :
                 1000U;

    for (;;) {
        volatile struct ZZUSBCommand *result;
        uint32_t actual_length;

        if (active_work_aborted()) {
            status = ZZUSB_STATUS_CANCELLED;
            break;
        }
        fill_iso_command(&cmd, unit, ior, ZZUSB_CMD_ISO_REAP,
                         ZZUSB_V2_DATA_MAX);
        status = send_usb_cmd(base, &cmd, NULL, 0);
        if (status == ZZUSB_STATUS_OK) {
            result = (volatile struct ZZUSBCommand *)(base + 0xa000);
            actual_length = result->actual_length;
            if (actual_length > sizeof(IsoWire)) {
                status = ZZUSB_STATUS_HOSTERROR;
                break;
            }
            safe_copy((void *)(base + 0xa000 + ZZUSB_DATA_OFFSET),
                      IsoWire, actual_length);
            if (!zzusb_iso_parse_reap(
                    IsoWire, actual_length, ior->iouh_Dir == UHDIR_IN,
                    &batch, packets, ZZUSB_ISO_MAX_PACKETS) ||
                batch.batch_id != batch_id) {
                status = ZZUSB_STATUS_HOSTERROR;
                break;
            }
            queued = 0;
            break;
        }
        if (status != ZZUSB_STATUS_NAK)
            break;
        if (elapsed_ms >= timeout_ms) {
            status = ZZUSB_STATUS_TIMEOUT;
            break;
        }
        {
            int wait_result = worker_wait_iso_event(10000U);

            if (wait_result < 0) {
                status = ZZUSB_STATUS_HOSTERROR;
                break;
            }
            if (wait_result > 0)
                elapsed_ms += 10U;
        }
    }

    if (queued)
        stop_iso_request(base, unit, ior);
    if (status != ZZUSB_STATUS_OK) {
        ior->iouh_Req.io_Error = map_proxy_status(status);
        return;
    }

    ior->iouh_Frame = packets[0].frame;
    ior->iouh_Req.io_Error = 0;
    for (unsigned index = 0; index < batch.packet_count; index++) {
        BYTE packet_error = map_iso_packet_status(
            packets[index].status, ior->iouh_Flags);

        if (!ior->iouh_Req.io_Error && packet_error) {
            ior->iouh_Req.io_Error = packet_error;
            ior->iouh_ExtError = packets[index].status;
        }
        if (ior->iouh_Dir == UHDIR_IN && ior->iouh_Data &&
            packets[index].actual) {
            safe_copy(IsoWire + batch.metadata_size +
                      packets[index].offset,
                      (uint8_t *)ior->iouh_Data + packets[index].offset,
                      packets[index].actual);
        }
        total_actual += packets[index].actual;
    }
    ior->iouh_Actual = total_actual;
}

static void fill_rt_iso_command(struct ZZUSBCommand *cmd,
                                const struct ZZRTIsoSlot *slot,
                                uint16_t command, uint32_t data_length)
{
    memset(cmd, 0, sizeof(*cmd));
    cmd->cmd = command;
    cmd->dev_addr = slot->address;
    cmd->endpoint = slot->endpoint;
    cmd->direction = slot->direction_in ? 0x80 : 0;
    cmd->max_pkt_size = slot->max_packet;
    cmd->data_length = data_length;
    cmd->timeout_ms = 100;
    cmd->speed = slot->speed;
    cmd->interval = slot->interval;
    cmd->split_hub_addr = slot->split_hub_addr;
    cmd->split_hub_port = slot->split_hub_port;
    cmd->flags = slot->flags;
    cmd->reserved = slot->generation;
}

static struct ZZRTIsoSlot *find_rt_iso_slot(
    const struct IOUsbHWRTIso *handler)
{
    for (unsigned index = 0; index < ZZ_RT_ISO_SLOTS; index++)
        if (RTIsoSlots[index].handler == handler &&
            RTIsoSlots[index].lifecycle.state != ZZUSB_RT_FREE)
            return &RTIsoSlots[index];
    return NULL;
}

static struct ZZRTIsoBatchContext *find_rt_batch_context(
    struct ZZRTIsoSlot *slot, uint32_t batch_id)
{
    for (unsigned index = 0; index < ZZUSB_ISO_PIPELINE_DEPTH; index++)
        if (slot->batches[index].batch_id == batch_id)
            return &slot->batches[index];
    return NULL;
}

static struct ZZRTIsoBatchContext *free_rt_batch_context(
    struct ZZRTIsoSlot *slot)
{
    for (unsigned index = 0; index < ZZUSB_ISO_PIPELINE_DEPTH; index++)
        if (!slot->batches[index].batch_id)
            return &slot->batches[index];
    return NULL;
}

static uint32_t rt_prefetched_bytes(const struct ZZRTIsoSlot *slot)
{
    uint32_t total = 0;

    for (unsigned index = 0; index < ZZUSB_ISO_PIPELINE_DEPTH; index++)
        if (slot->batches[index].batch_id)
            total += slot->batches[index].prefetched_bytes;
    return total;
}

static void rt_finish_out_context(struct ZZRTIsoSlot *slot,
                                  struct ZZRTIsoBatchContext *context,
                                  const struct zzusb_iso_packet_result *packets,
                                  uint16_t forced_flags)
{
    if (!slot->handler || !slot->handler->urti_OutDoneHook)
        return;
    for (unsigned index = 0; index < context->packet_count; index++) {
        struct IOUsbHWBufferReq *request = &context->requests[index];

        request->ubr_Length = packets ? packets[index].actual : 0;
        if (packets) {
            request->ubr_Frame = packets[index].frame;
            request->ubr_Flags |= zzusb_iso_status_flags(
                packets[index].status);
        }
        request->ubr_Flags |= forced_flags;
        CallHookPkt(slot->handler->urti_OutDoneHook,
                    (APTR)slot, request);
    }
}

static void rt_cancel_contexts(struct ZZRTIsoSlot *slot,
                               uint16_t flags)
{
    for (unsigned index = 0; index < ZZUSB_ISO_PIPELINE_DEPTH; index++) {
        struct ZZRTIsoBatchContext *context = &slot->batches[index];

        if (!context->batch_id)
            continue;
        if (!slot->direction_in)
            rt_finish_out_context(slot, context, NULL, flags);
        memset(context, 0, sizeof(*context));
    }
}

static uint16_t queue_rt_iso_batch(struct ZZRTIsoSlot *slot)
{
    struct ZZRTIsoBatchContext *context;
    struct ZZUSBCommand cmd;
    uint16_t lengths[ZZUSB_ISO_MAX_PACKETS];
    uint32_t batch_id;
    uint32_t total_data = 0;
    unsigned packet_count = slot->packet_count;
    unsigned wire_length;
    uint16_t status;

    context = free_rt_batch_context(slot);
    if (!context)
        return ZZUSB_STATUS_BUSY;
    memcpy(lengths, slot->packet_lengths,
           packet_count * sizeof(lengths[0]));

    if (!slot->direction_in && slot->handler->urti_OutPrefetch) {
        uint32_t available = slot->handler->urti_OutPrefetch;
        uint32_t outstanding = rt_prefetched_bytes(slot);
        uint16_t packet_size = zzusb_iso_payload_size(slot->max_packet);
        unsigned allowed;

        if (outstanding >= available)
            return ZZUSB_STATUS_BUSY;
        available -= outstanding;
        allowed = packet_size ? (unsigned)(available / packet_size) : 0;
        if (!allowed)
            return ZZUSB_STATUS_BUSY;
        if (packet_count > allowed)
            packet_count = allowed;
    }

    batch_id = zzusb_rt_queue(&slot->lifecycle);
    if (!batch_id)
        return ZZUSB_STATUS_BUSY;
    memset(context, 0, sizeof(*context));
    context->batch_id = batch_id;
    context->packet_count = (uint8_t)packet_count;

    if (!slot->direction_in) {
        memset(IsoPayload, 0, sizeof(IsoPayload));
        for (unsigned index = 0; index < packet_count; index++) {
            struct IOUsbHWBufferReq *request =
                &context->requests[index];
            uint16_t requested = lengths[index];

            request->ubr_Buffer = NULL;
            request->ubr_Length = requested;
            request->ubr_Frame = 0;
            request->ubr_Flags = 0;
            if (slot->handler->urti_OutReqHook)
                CallHookPkt(slot->handler->urti_OutReqHook,
                            (APTR)slot, request);
            if (request->ubr_Length > requested) {
                request->ubr_Length = requested;
                request->ubr_Flags |=
                    ZZUSB_RT_FLAG_UNDERRUN |
                    ZZUSB_RT_FLAG_PACKET_ERROR;
            }
            lengths[index] = (uint16_t)request->ubr_Length;
            if (request->ubr_Buffer && lengths[index])
                safe_copy(request->ubr_Buffer,
                          IsoPayload + total_data, lengths[index]);
            else if (lengths[index])
                request->ubr_Flags |=
                    ZZUSB_RT_FLAG_UNDERRUN |
                    ZZUSB_RT_FLAG_PACKET_ERROR;
            total_data += lengths[index];
        }
    } else {
        for (unsigned index = 0; index < packet_count; index++)
            total_data += lengths[index];
    }
    context->prefetched_bytes = total_data;

    wire_length = zzusb_iso_build_queue(
        IsoWire, ZZUSB_MAINT_DATA_MAX, batch_id, ZZUSB_ISO_FLAG_ASAP,
        0, 0, lengths, packet_count, IsoPayload, slot->direction_in);
    if (!wire_length) {
        zzusb_rt_complete(&slot->lifecycle, batch_id);
        if (!slot->direction_in)
            rt_finish_out_context(
                slot, context, NULL, ZZUSB_RT_FLAG_PACKET_ERROR);
        memset(context, 0, sizeof(*context));
        return ZZUSB_STATUS_BADPARAM;
    }
    fill_rt_iso_command(&cmd, slot, ZZUSB_CMD_ISO_QUEUE, wire_length);
    status = send_usb_cmd_maintenance(
        (volatile uint8_t *)slot->unit->zz_Registers,
        &cmd, IsoWire, wire_length, NULL, 0);
    if (status != ZZUSB_STATUS_OK) {
        zzusb_rt_complete(&slot->lifecycle, batch_id);
        if (!slot->direction_in)
            rt_finish_out_context(
                slot, context, NULL, ZZUSB_RT_FLAG_PACKET_ERROR);
        memset(context, 0, sizeof(*context));
        return status;
    }
    zzusb_engine_diag_count(ZZUSB_DRIVER_COUNT_ISO_QUEUE);
    return ZZUSB_STATUS_OK;
}

static uint16_t fill_rt_iso_pipeline(struct ZZRTIsoSlot *slot)
{
    uint16_t status = ZZUSB_STATUS_OK;

    while (slot->lifecycle.state == ZZUSB_RT_RUNNING &&
           slot->lifecycle.in_flight < ZZUSB_ISO_PIPELINE_DEPTH) {
        status = queue_rt_iso_batch(slot);
        if (status != ZZUSB_STATUS_OK)
            break;
    }
    return status;
}

static void rt_deliver_in_packet(
    struct ZZRTIsoSlot *slot,
    const struct zzusb_iso_batch_result *batch,
    const struct zzusb_iso_packet_result *packet)
{
    struct IOUsbHWBufferReq request;
    const uint8_t *source = IsoWire + batch->metadata_size + packet->offset;
    uint32_t remaining = packet->actual;
    uint32_t copied = 0;
    uint16_t status_flags = zzusb_iso_status_flags(packet->status);
    unsigned segments = 0;

    memset(&request, 0, sizeof(request));
    request.ubr_Frame = packet->frame;
    request.ubr_Flags = status_flags;
    if (!slot->handler->urti_InReqHook) {
        request.ubr_Buffer = (UBYTE *)source;
        request.ubr_Length = remaining;
        if (slot->handler->urti_InDoneHook)
            CallHookPkt(slot->handler->urti_InDoneHook,
                        (APTR)slot, &request);
        return;
    }

    do {
        uint32_t segment;

        request.ubr_Buffer = NULL;
        request.ubr_Length = remaining;
        request.ubr_Frame = packet->frame;
        request.ubr_Flags = status_flags;
        CallHookPkt(slot->handler->urti_InReqHook,
                    (APTR)slot, &request);
        request.ubr_Flags |= status_flags;
        segment = request.ubr_Length;
        if (segment > remaining) {
            segment = remaining;
            request.ubr_Flags |=
                ZZUSB_RT_FLAG_OVERRUN |
                ZZUSB_RT_FLAG_PACKET_ERROR;
        }
        if (segment && request.ubr_Buffer)
            safe_copy(source + copied, request.ubr_Buffer, segment);
        else if (segment)
            request.ubr_Flags |=
                ZZUSB_RT_FLAG_OVERRUN |
                ZZUSB_RT_FLAG_PACKET_ERROR;
        request.ubr_Length = segment;
        if (slot->handler->urti_InDoneHook)
            CallHookPkt(slot->handler->urti_InDoneHook,
                        (APTR)slot, &request);
        copied += segment;
        remaining -= segment;
        segments++;
    } while (remaining && (request.ubr_Flags & UBFF_CONTBUFFER) &&
             segments < ZZUSB_ISO_MAX_PACKETS && request.ubr_Length);
}

static uint16_t reap_rt_iso_batch(struct ZZRTIsoSlot *slot)
{
    struct ZZUSBCommand cmd;
    struct zzusb_iso_batch_result batch;
    struct zzusb_iso_packet_result packets[ZZUSB_ISO_MAX_PACKETS];
    struct ZZRTIsoBatchContext *context;
    uint32_t actual_length;
    uint16_t status;

    fill_rt_iso_command(&cmd, slot, ZZUSB_CMD_ISO_REAP,
                        ZZUSB_MAINT_DATA_MAX);
    status = send_usb_cmd_maintenance(
        (volatile uint8_t *)slot->unit->zz_Registers,
        &cmd, NULL, 0, IsoWire, sizeof(IsoWire));
    if (status != ZZUSB_STATUS_OK)
        return status;
    actual_length = cmd.actual_length;
    if (actual_length > sizeof(IsoWire))
        return ZZUSB_STATUS_HOSTERROR;
    if (!zzusb_iso_parse_reap(
            IsoWire, actual_length, slot->direction_in,
            &batch, packets, ZZUSB_ISO_MAX_PACKETS))
        return ZZUSB_STATUS_HOSTERROR;
    context = find_rt_batch_context(slot, batch.batch_id);
    if (!context || context->packet_count != batch.packet_count ||
        !zzusb_rt_complete(&slot->lifecycle, batch.batch_id))
        return ZZUSB_STATUS_HOSTERROR;

    if (slot->direction_in) {
        for (unsigned index = 0; index < batch.packet_count; index++)
            rt_deliver_in_packet(slot, &batch, &packets[index]);
    } else {
        rt_finish_out_context(slot, context, packets, 0);
    }
    memset(context, 0, sizeof(*context));
    zzusb_engine_diag_count(ZZUSB_DRIVER_COUNT_ISO_REAP);
    zzusb_engine_diag_record(
        ZZUSB_DRIVER_EVENT_ISO, ZZUSB_ENGINE_STATUS_OK,
        batch.batch_id, ProtocolStates[0].controller_epoch,
        slot->address, (uint8_t)slot->endpoint,
        slot->direction_in ? 0x80 : 0,
        (uint16_t)((slot->split_hub_addr << 8) |
                   (slot->split_hub_port & 0xffU)),
        slot->flags, batch.total_data, WorkSequence);
    return ZZUSB_STATUS_OK;
}

static uint16_t stop_rt_iso_slot(struct ZZRTIsoSlot *slot)
{
    struct ZZUSBCommand cmd;
    uint16_t status;

    if (slot->lifecycle.state == ZZUSB_RT_RUNNING) {
        if (!zzusb_rt_begin_stop(&slot->lifecycle))
            return ZZUSB_STATUS_BADPARAM;
    } else if (slot->lifecycle.state != ZZUSB_RT_STOPPING) {
        return ZZUSB_STATUS_BADPARAM;
    }
    fill_rt_iso_command(&cmd, slot, ZZUSB_CMD_ISO_STOP, 0);
    status = send_usb_cmd_maintenance(
        (volatile uint8_t *)slot->unit->zz_Registers,
        &cmd, NULL, 0, NULL, 0);
    if (status != ZZUSB_STATUS_OK && status != ZZUSB_STATUS_NAK)
        return status;
    rt_cancel_contexts(slot, ZZUSB_RT_FLAG_PACKET_ERROR);
    zzusb_rt_finish_stop(&slot->lifecycle);
    return ZZUSB_STATUS_OK;
}

static void stop_rt_iso_for_unit(struct ZZUSBUnit *unit)
{
    for (unsigned index = 0; index < ZZ_RT_ISO_SLOTS; index++) {
        struct ZZRTIsoSlot *slot = &RTIsoSlots[index];

        if (slot->unit == unit &&
            (slot->lifecycle.state == ZZUSB_RT_RUNNING ||
             slot->lifecycle.state == ZZUSB_RT_STOPPING))
            stop_rt_iso_slot(slot);
    }
}

static void finish_reset_rt_iso_for_unit(struct ZZUSBUnit *unit)
{
    for (unsigned index = 0; index < ZZ_RT_ISO_SLOTS; index++) {
        struct ZZRTIsoSlot *slot = &RTIsoSlots[index];

        if (slot->unit != unit)
            continue;
        if (slot->lifecycle.state == ZZUSB_RT_RUNNING)
            zzusb_rt_begin_stop(&slot->lifecycle);
        if (slot->lifecycle.state == ZZUSB_RT_STOPPING) {
            rt_cancel_contexts(slot, ZZUSB_RT_FLAG_PACKET_ERROR);
            zzusb_rt_finish_stop(&slot->lifecycle);
        }
        slot->generation = generation_for_unit(unit);
    }
}

static BYTE add_rt_iso_handler(struct ZZUSBUnit *unit,
                               struct IOUsbHWReq *ior)
{
    struct IOUsbHWRTIso *handler =
        (struct IOUsbHWRTIso *)ior->iouh_Data;
    struct ZZRTIsoSlot *slot = NULL;
    struct ZZUSBCommand command;
    unsigned packet_count;

    if (!(iso_public_capabilities(unit) & UHCF_RT_ISO) ||
        !handler || (ior->iouh_Flags & UHFF_LOWSPEED) ||
        !ior->iouh_Endpoint || !ior->iouh_Interval ||
        find_rt_iso_slot(handler))
        return UHIOERR_BADPARAMS;
    for (unsigned index = 0; index < ZZ_RT_ISO_SLOTS; index++)
        if (RTIsoSlots[index].lifecycle.state == ZZUSB_RT_FREE) {
            slot = &RTIsoSlots[index];
            break;
        }
    if (!slot)
        return UHIOERR_OUTOFMEMORY;

    memset(slot, 0, sizeof(*slot));
    fill_iso_command(&command, unit, ior, ZZUSB_CMD_ISO_QUEUE, 0);
    slot->unit = unit;
    slot->handler = handler;
    slot->generation = command.reserved;
    slot->address = command.dev_addr;
    slot->endpoint = command.endpoint;
    slot->max_packet = command.max_pkt_size;
    slot->interval = command.interval;
    slot->speed = command.speed;
    slot->split_hub_addr = command.split_hub_addr;
    slot->split_hub_port = command.split_hub_port;
    slot->flags = command.flags;
    slot->direction_in = command.direction == 0x80;
    packet_count = zzusb_iso_plan_realtime(
        slot->max_packet, slot->interval,
        slot->speed == ZZUSB_SPEED_HIGH,
        slot->packet_lengths, ZZUSB_ISO_MAX_PACKETS,
        &slot->duration_microframes);
    if (!packet_count) {
        memset(slot, 0, sizeof(*slot));
        return UHIOERR_BADPARAMS;
    }
    {
        unsigned planned_count = packet_count;

        packet_count = zzusb_iso_limit_packet_count(
            slot->packet_lengths, packet_count, ZZUSB_MAINT_DATA_MAX);
        if (!packet_count) {
            memset(slot, 0, sizeof(*slot));
            return UHIOERR_BADPARAMS;
        }
        slot->duration_microframes =
            (uint16_t)(((uint32_t)slot->duration_microframes *
                        packet_count) / planned_count);
    }
    slot->packet_count = (uint8_t)packet_count;
    zzusb_rt_init(&slot->lifecycle);
    if (!zzusb_rt_add(&slot->lifecycle)) {
        memset(slot, 0, sizeof(*slot));
        return UHIOERR_HOSTERROR;
    }
    handler->urti_DriverPrivate1 = slot;
    ior->iouh_Actual = 0;
    return 0;
}

static BYTE start_rt_iso_handler(struct ZZUSBUnit *unit,
                                 struct IOUsbHWReq *ior)
{
    struct ZZRTIsoSlot *slot = find_rt_iso_slot(
        (struct IOUsbHWRTIso *)ior->iouh_Data);
    uint16_t status;

    if (!slot || slot->unit != unit ||
        !(iso_public_capabilities(unit) & UHCF_RT_ISO) ||
        !zzusb_rt_start(&slot->lifecycle))
        return UHIOERR_BADPARAMS;
    status = fill_rt_iso_pipeline(slot);
    if (!slot->lifecycle.in_flight) {
        zzusb_rt_begin_stop(&slot->lifecycle);
        zzusb_rt_finish_stop(&slot->lifecycle);
        return map_proxy_status(status);
    }
    if (PollBase && PollBase->zz_PollTask && PollBase->zz_PollSignal)
        Signal(PollBase->zz_PollTask, PollBase->zz_PollSignal);
    return 0;
}

static BYTE stop_rt_iso_handler(struct ZZUSBUnit *unit,
                                struct IOUsbHWReq *ior)
{
    struct ZZRTIsoSlot *slot = find_rt_iso_slot(
        (struct IOUsbHWRTIso *)ior->iouh_Data);

    if (!slot || slot->unit != unit)
        return UHIOERR_BADPARAMS;
    return map_proxy_status(stop_rt_iso_slot(slot));
}

static BYTE remove_rt_iso_handler(struct ZZUSBUnit *unit,
                                  struct IOUsbHWReq *ior)
{
    struct IOUsbHWRTIso *handler =
        (struct IOUsbHWRTIso *)ior->iouh_Data;
    struct ZZRTIsoSlot *slot = find_rt_iso_slot(handler);
    uint16_t status;

    if (!slot || slot->unit != unit)
        return UHIOERR_BADPARAMS;
    if (slot->lifecycle.state == ZZUSB_RT_RUNNING ||
        slot->lifecycle.state == ZZUSB_RT_STOPPING) {
        status = stop_rt_iso_slot(slot);
        if (status != ZZUSB_STATUS_OK)
            return map_proxy_status(status);
    }
    if (!zzusb_rt_remove(&slot->lifecycle))
        return UHIOERR_BADPARAMS;
    handler->urti_DriverPrivate1 = NULL;
    memset(slot, 0, sizeof(*slot));
    return 0;
}

static int rt_iso_pending_for_unit(const struct ZZUSBUnit *unit)
{
    for (unsigned index = 0; index < ZZ_RT_ISO_SLOTS; index++)
        if (RTIsoSlots[index].unit == unit &&
            RTIsoSlots[index].lifecycle.state == ZZUSB_RT_RUNNING)
            return 1;
    return 0;
}

static uint16_t rt_iso_service_limit_ms(const struct ZZUSBUnit *unit)
{
    uint16_t shortest_ms = 8;

    for (unsigned index = 0; index < ZZ_RT_ISO_SLOTS; index++) {
        const struct ZZRTIsoSlot *slot = &RTIsoSlots[index];
        uint32_t safe_microframes;
        uint32_t candidate_ms;

        if (slot->unit != unit ||
            slot->lifecycle.state != ZZUSB_RT_RUNNING)
            continue;
        safe_microframes = slot->duration_microframes *
            (slot->lifecycle.in_flight > 1 ?
             slot->lifecycle.in_flight - 1U : 1U);
        candidate_ms = safe_microframes / 8U;
        if (!candidate_ms)
            candidate_ms = 1;
        if (candidate_ms < shortest_ms)
            shortest_ms = (uint16_t)candidate_ms;
    }
    return shortest_ms;
}

static void poll_rt_iso(struct ZZUSBUnit *unit)
{
    for (unsigned index = 0; index < ZZ_RT_ISO_SLOTS; index++) {
        struct ZZRTIsoSlot *slot = &RTIsoSlots[index];
        uint16_t status;

        if (slot->unit != unit ||
            slot->lifecycle.state != ZZUSB_RT_RUNNING)
            continue;
        if (!unit->zz_PortPresent) {
            zzusb_rt_begin_stop(&slot->lifecycle);
            rt_cancel_contexts(slot, ZZUSB_RT_FLAG_PACKET_ERROR);
            zzusb_rt_finish_stop(&slot->lifecycle);
            continue;
        }
        if (!slot->lifecycle.in_flight) {
            fill_rt_iso_pipeline(slot);
            continue;
        }
        while (slot->lifecycle.in_flight) {
            status = reap_rt_iso_batch(slot);
            if (status == ZZUSB_STATUS_OK) {
                fill_rt_iso_pipeline(slot);
                continue;
            }
            if (status != ZZUSB_STATUS_NAK &&
                status != ZZUSB_STATUS_BUSY)
                stop_rt_iso_slot(slot);
            break;
        }
    }
}

static ULONG usb_event_isr(struct ZZUSBBase *base __asm("a1"))
{
    volatile UWORD *status_reg;
    UWORD status;

    if (!base || !base->zz_Units[0].zz_Registers)
        return 0;
    status_reg = (volatile UWORD *)
        ((uint8_t *)base->zz_Units[0].zz_Registers + 0x04);
    status = *status_reg;
    if (!zzusb_event_interrupt_pending(status))
        return 0;

    *status_reg = ZZUSB_EVENT_ACK_VALUE;
    USBEventPending = 1;
    if (base->zz_PollTask && base->zz_PollSignal)
        Signal(base->zz_PollTask, base->zz_PollSignal);
    return 1;
}

static void install_usb_event_isr(struct ZZUSBBase *base)
{
    if (USBEventIRQInstalled)
        return;
    memset(&USBEventInterrupt, 0, sizeof(USBEventInterrupt));
    USBEventInterrupt.is_Node.ln_Type = NT_INTERRUPT;
    USBEventInterrupt.is_Node.ln_Pri = 0;
    USBEventInterrupt.is_Node.ln_Name = (char *)"zzusbhw.usb";
    USBEventInterrupt.is_Data = base;
    USBEventInterrupt.is_Code = (VOID (*)())usb_event_isr;
    Disable();
    AddIntServer(USBEventIRQInt2 ? INTB_PORTS : INTB_EXTER,
                 &USBEventInterrupt);
    Enable();
    USBEventIRQInstalled = 1;
}

static void ensure_poll_task(struct ZZUSBBase *ZZBase)
{
    if (ZZBase->zz_PollTask)
        return;

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
         * Reserve system signals 0..15 so AllocSignal(-1) in the task
         * body returns a user bit (16..31), and initialize nest counts
         * to the normal task state for exec variants that do not fix
         * them up inside AddTask().
         */
        poll->tc_SigAlloc  = 0xFFFF;
        poll->tc_IDNestCnt = -1;
        poll->tc_TDNestCnt = -1;

        PollBase = ZZBase;
        ZZBase->zz_PollTask = poll;

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
        install_usb_event_isr(ZZBase);
    }
    Permit();
}

static struct Library* __attribute__((used)) init_device(uint8_t *seg_list asm("a0"), struct Library *dev asm("d0"))
{
    struct Library* ExpansionBase;
    struct ConfigDev* cd = NULL;
    uint8_t* registers = NULL;

    SysBase = get_sysbase();

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

    UtilityBase = (struct Library *)OpenLibrary(
        (CONST_STRPTR)"utility.library", 0L);

    /* Saved for expunge so the loader can release our segments. */
    DeviceSegList = seg_list;

    dev->lib_Node.ln_Type = NT_DEVICE;
    dev->lib_Node.ln_Name = (char *)device_name;
    dev->lib_Version = DEVICE_VERSION;
    dev->lib_Revision = DEVICE_REVISION;
    dev->lib_IdString = (char *)device_id_string;

    InitSemaphore(&ZZBase->zz_Lock);
    {
        UWORD present = 0;
        UWORD value = zzcfg_query((ULONG)registers, ZZ_CFG_KEY_INT2,
                                  &present);
        USBEventIRQInt2 = (present && value) ? 1 : 0;
    }
    USBEventIRQInstalled = 0;
    USBEventPending = 0;
    memset(UnitGeneration, 0, sizeof(UnitGeneration));
    UnitGeneration[0] = 1;

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
    unit->zz_BulkErrCount = 0;
    for (int ep = 0; ep < 16; ep++)
        unit->zz_IntPending[ep] = NULL;
    RootHubIntPending[0] = NULL;
    RootHubPollDelay[0] = 0;
    reset_int_slots();
    memset(ProtocolStates, 0, sizeof(ProtocolStates));
    ProtocolStates[0].registers = registers;
    ProtocolStates[0].next_request_id = 1;
    ProtocolStates[0].mode = ZZUSB_PROTOCOL_LEGACY;
    ProtocolNegotiated = 0;
    memset(WorkSlots, 0, sizeof(WorkSlots));
    memset(RTIsoSlots, 0, sizeof(RTIsoSlots));
    memset(IsoWire, 0, sizeof(IsoWire));
    memset(IsoPayload, 0, sizeof(IsoPayload));
    SimpleIsoBatchId = 1;
    zzusb_engine_diag_reset();

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

/*
 * Unload helper, callable from both expunge and close. Caller must
 * ensure lib_OpenCnt is zero before invoking. Returns 0 (and re-asserts
 * LIBF_DELEXP) if the poll task is still alive — its Task struct and
 * stack live inside ZZBase, so freeing the base out from under it
 * would crash on the next wake-up. The poll task is created lazily
 * on the first INT transfer and never torn down in current builds,
 * so once it exists the driver effectively becomes non-unloadable;
 * acceptable for a hardware driver normally only unloaded at reboot.
 */
static uint8_t *unload_device(struct Library *dev)
{
    struct ZZUSBBase *ZZBase = (struct ZZUSBBase *)dev;

    if (ZZBase->zz_PollTask) {
        dev->lib_Flags |= LIBF_DELEXP;
        return 0;
    }

    uint8_t *seg = DeviceSegList;
    if (UtilityBase) {
        CloseLibrary(UtilityBase);
        UtilityBase = NULL;
    }

    /*
     * Forbid() prevents another task from FindDevice'ing us between
     * the Remove and FreeMem and dereferencing a half-freed base.
     */
    Forbid();
    Remove(&dev->lib_Node);
    FreeMem((char *)dev - dev->lib_NegSize,
            dev->lib_NegSize + dev->lib_PosSize);
    Permit();
    return seg;
}

static uint8_t* __attribute__((used)) expunge(struct Library *dev asm("a6"))
{
    /*
     * Defer the unload if any unit is still open. Without this guard
     * the caller would FreeMem our base while live IORequests still
     * point at it, crashing on the next BeginIO.
     */
    if (dev->lib_OpenCnt) {
        dev->lib_Flags |= LIBF_DELEXP;
        return 0;
    }
    return unload_device(dev);
}

static void __attribute__((used)) open(struct Library *dev asm("a6"), struct IOUsbHWReq *ior asm("a1"),
                 uint32_t unitnum asm("d0"), uint32_t flags asm("d1"))
{
    struct ZZUSBBase* ZZBase = (struct ZZUSBBase*)dev;
    int io_err = IOERR_OPENFAIL;

    if (!ior) {
        return;
    }

    if (unitnum < ZZ_NUM_PORTS) {
        struct ZZUSBUnit* unit = &ZZBase->zz_Units[unitnum];
        if (unit->zz_Enabled) {
            io_err = 0;
            ior->iouh_Req.io_Unit = (struct Unit*)unit;
            ior->iouh_Req.io_Unit->unit_flags = UNITF_ACTIVE;
            ior->iouh_Req.io_Unit->unit_OpenCnt++;
            dev->lib_OpenCnt++;
            /*
             * A pending expunge is being cancelled by this open — clear
             * only on success, otherwise a failed open would silently
             * lose a previous deferred-expunge request.
             */
            dev->lib_Flags &= ~LIBF_DELEXP;
        }
    }

    ior->iouh_Req.io_Error = io_err;
}

static uint8_t* __attribute__((used)) close(struct Library *dev asm("a6"), struct IOUsbHWReq *ior asm("a1"))
{
    if (ior) {
        struct Unit *unit = ior->iouh_Req.io_Unit;
        if (unit && unit != (struct Unit *)-1 && unit->unit_OpenCnt) {
            unit->unit_OpenCnt--;
        }
        /* Sentinel values let exec catch use-after-close on this IOR. */
        ior->iouh_Req.io_Unit = (struct Unit *)-1;
        ior->iouh_Req.io_Device = (struct Device *)-1;
    }

    if (dev->lib_OpenCnt) {
        dev->lib_OpenCnt--;
    }

    if (dev->lib_OpenCnt == 0 && (dev->lib_Flags & LIBF_DELEXP)) {
        return unload_device(dev);
    }
    return 0;
}

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
    for (int i = 0; i < ZZ_INT_PENDING_SLOTS; i++) {
        struct ZZIntPendingSlot *slot = &IntPendingSlots[i];
        struct IOUsbHWReq *p = slot->ior;
        if (!p || slot->unit != unit) continue;
        clear_int_slot(slot);
        p->iouh_Actual = 0;
        p->iouh_Req.io_Error = UHIOERR_USBOFFLINE;
        if (aborted && aborted_count && *aborted_count < aborted_max) {
            aborted[(*aborted_count)++] = p;
        }
    }

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
        if (r->speed == ZZUSB_SPEED_LOW) {
            /*
             * The Zynq/ChipIdea root port does not complete direct
             * low-speed EP0. Hide these devices from Poseidon before it
             * starts enumeration; low-speed devices behind a high-speed
             * hub are still handled through split transactions because
             * the root port speed remains HIGH in that topology.
             */
            if (!unit->zz_PortDead)
                mark_direct_low_speed_unsupported(unit, "LS_ROOT_IGNORE");
            return;
        }

        UWORD port_status = UPSF_PORT_POWER | UPSF_PORT_CONNECTION;
        if (unit->zz_PortPresent && unit->zz_Speed == r->speed) {
            port_status |= unit->zz_PortStatus &
                           (UPSF_PORT_ENABLE | UPSF_PORT_SUSPEND);
        }
        if (r->speed == ZZUSB_SPEED_HIGH) {
            port_status |= UPSF_PORT_HIGH_SPEED;
        } else if (r->speed == ZZUSB_SPEED_LOW) {
            port_status |= UPSF_PORT_LOW_SPEED;
        }
        if (!unit->zz_PortPresent || unit->zz_Speed != r->speed) {
            bump_unit_generation(unit);
            unit->zz_PortPresent = TRUE;
            unit->zz_Speed = r->speed;
            unit->zz_PortStatus = port_status;
            unit->zz_PortChange = UPSF_C_PORT_CONNECTION;
            trace_port_state(unit, "PORT_CONNECT");
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
            bump_unit_generation(unit);
            unit->zz_PortPresent = FALSE;
            unit->zz_Speed = 0;
            unit->zz_PortStatus = UPSF_PORT_POWER;
            unit->zz_PortChange = UPSF_C_PORT_CONNECTION;
            unit->zz_BulkErrCount = 0;
            abort_int_iors_offline(unit, aborted, aborted_count, aborted_max);
            trace_port_state(unit, "PORT_DISCONNECT");
        }
    }
}

static void poll_int_pending_legacy(struct ZZUSBBase *base_dev,
                                    struct ZZUSBUnit *unit)
{
    volatile uint8_t *base = (volatile uint8_t *)unit->zz_Registers;

    for (int slot_index = 0; slot_index < ZZ_INT_PENDING_SLOTS;
         slot_index++) {
        struct ZZIntPendingSlot *slot = &IntPendingSlots[slot_index];
        struct IOUsbHWReq *reply_now = NULL;
        struct IOUsbHWReq *ior;
        struct ZZUSBCommand cmd;
        uint16_t status;
        uint32_t actual = 0;
        int idle_in = 0;

        ObtainSemaphore(&base_dev->zz_Lock);
        if (!slot->ior || slot->unit != unit) {
            ReleaseSemaphore(&base_dev->zz_Lock);
            continue;
        }
        ior = slot->ior;
        if (slot->abort_requested) {
            ior->iouh_Actual = 0;
            ior->iouh_Req.io_Error = IOERR_ABORTED;
            clear_int_slot(slot);
            reply_now = ior;
            goto legacy_done;
        }

        memset(&cmd, 0, sizeof(cmd));
        cmd.cmd = ZZUSB_CMD_INT_XFER;
        cmd.dev_addr = ior->iouh_DevAddr;
        cmd.endpoint = ior->iouh_Endpoint;
        cmd.direction = ior->iouh_Dir == UHDIR_IN ? 0x80 : 0;
        cmd.max_pkt_size = ior->iouh_MaxPktSize;
        cmd.speed = request_speed(unit, ior);
        cmd.data_length = ior->iouh_Length;
        cmd.interval = ior->iouh_Interval;
        cmd.timeout_ms = 1000;
        fill_split_fields(&cmd, unit, ior);
        status = send_usb_cmd(
            base, &cmd,
            ior->iouh_Dir == UHDIR_OUT ? ior->iouh_Data : NULL,
            ior->iouh_Dir == UHDIR_OUT ? ior->iouh_Length : 0);

        if (status == ZZUSB_STATUS_OK) {
            volatile struct ZZUSBCommand *result =
                (volatile struct ZZUSBCommand *)(base + 0xa000);
            actual = result->actual_length;
            if (actual > ior->iouh_Length)
                actual = ior->iouh_Length;
            trace_hub_int_data(
                unit, ior, actual,
                (volatile uint8_t *)(base + 0xa000 + ZZUSB_DATA_OFFSET));
            idle_in = ior->iouh_Dir == UHDIR_IN && actual == 0;
            if (ior->iouh_Dir == UHDIR_IN && actual > 0 && actual <= 2 &&
                is_zero_report(
                    (volatile uint8_t *)(base + 0xa000 +
                                         ZZUSB_DATA_OFFSET),
                    actual))
                idle_in = 1;
        } else if (ior->iouh_Dir == UHDIR_IN &&
                   status != ZZUSB_STATUS_OFFLINE) {
            idle_in = 1;
        }

        if (idle_in) {
            if (slot->idle_polls < 0xff)
                slot->idle_polls++;
            if (slot->idle_polls >= ZZ_INT_IDLE_REPLY_POLLS) {
                if (ior->iouh_Data && ior->iouh_Length) {
                    if (actual)
                        safe_copy(
                            (void *)(base + 0xa000 + ZZUSB_DATA_OFFSET),
                            ior->iouh_Data, actual);
                    else
                        safe_zero(ior->iouh_Data, ior->iouh_Length);
                }
                ior->iouh_Actual = actual;
                ior->iouh_Req.io_Error = 0;
                clear_int_slot(slot);
                reply_now = ior;
            }
        } else if (status == ZZUSB_STATUS_OK) {
            if (ior->iouh_Dir == UHDIR_IN && ior->iouh_Data && actual)
                safe_copy((void *)(base + 0xa000 + ZZUSB_DATA_OFFSET),
                          ior->iouh_Data, actual);
            ior->iouh_Actual = actual;
            ior->iouh_Req.io_Error = 0;
            clear_int_slot(slot);
            reply_now = ior;
        } else {
            ior->iouh_Actual = 0;
            ior->iouh_Req.io_Error = map_proxy_status(status);
            clear_int_slot(slot);
            reply_now = ior;
        }

legacy_done:
        ReleaseSemaphore(&base_dev->zz_Lock);
        if (reply_now && !(reply_now->iouh_Req.io_Flags & IOF_QUICK))
            ReplyMsg(&reply_now->iouh_Req.io_Message);
        return;
    }
}

/*
 * Arm each pending interrupt IOR once, then reap only after the firmware
 * raises the coalesced USB event interrupt. NAK and no-data responses remain
 * non-terminal; terminal completion stops the firmware endpoint before the
 * Poseidon IOR and its slot are released.
 */
static void poll_int_pending(struct ZZUSBBase *base_dev,
                             struct ZZUSBUnit *unit,
                             int reap_events)
{
    volatile uint8_t *base = (volatile uint8_t*)unit->zz_Registers;
    struct ZZUSBProtocolState *state = protocol_state_for(base);

    if (!state || !(state->capabilities & ZZUSB_CAP_PERIODIC)) {
        poll_int_pending_legacy(base_dev, unit);
        return;
    }

    for (int slot_index = 0; slot_index < ZZ_INT_PENDING_SLOTS; slot_index++) {
        struct ZZIntPendingSlot *slot = &IntPendingSlots[slot_index];
        struct IOUsbHWReq *reply_now = NULL;
        struct IOUsbHWReq *ior;
        struct ZZUSBCommand cmd;
        uint16_t status;
        uint16_t stop_status;
        ObtainSemaphore(&base_dev->zz_Lock);
        if (!slot->ior || slot->unit != unit) {
            ReleaseSemaphore(&base_dev->zz_Lock);
            continue;
        }
        ior = slot->ior;

        memset(&cmd, 0, sizeof(cmd));
        cmd.dev_addr = ior->iouh_DevAddr;
        cmd.endpoint = ior->iouh_Endpoint;
        cmd.direction = (ior->iouh_Dir == UHDIR_IN) ? 0x80 : 0x00;
        cmd.max_pkt_size = ior->iouh_MaxPktSize;
        cmd.speed = request_speed(unit, ior);
        cmd.data_length = ior->iouh_Length;
        cmd.interval = ior->iouh_Interval;
        cmd.reserved = generation_for_unit(unit);
        cmd.timeout_ms = 100;
        fill_split_fields(&cmd, unit, ior);

        if (slot->abort_requested) {
            stop_periodic_slot(slot);
            ior->iouh_Actual = 0;
            ior->iouh_Req.io_Error = IOERR_ABORTED;
            clear_int_slot(slot);
            reply_now = ior;
        }

        if (!reply_now && slot->rearm_required) {
            stop_status = stop_periodic_slot(slot);
            slot->rearm_required = 0;
            if (!periodic_stop_retired(stop_status)) {
                ior->iouh_Actual = 0;
                ior->iouh_Req.io_Error = map_proxy_status(stop_status);
                clear_int_slot(slot);
                reply_now = ior;
            }
        }

        if (!reply_now && !slot->armed) {
            cmd.cmd = ZZUSB_CMD_PERIODIC_ARM;
            status = send_usb_cmd(
                base, &cmd,
                (ior->iouh_Dir == UHDIR_OUT) ? ior->iouh_Data : NULL,
                (ior->iouh_Dir == UHDIR_OUT) ? ior->iouh_Length : 0);
            if (status != ZZUSB_STATUS_OK) {
                ior->iouh_Actual = 0;
                ior->iouh_Req.io_Error = map_proxy_status(status);
                clear_int_slot(slot);
                reply_now = ior;
            } else {
                slot->armed = 1;
            }
        }

        if (!reply_now && slot->armed && reap_events) {
            volatile struct ZZUSBCommand *result;
            uint32_t actual;
            int zero_report = 0;
            enum zzusb_interrupt_action action;

            cmd.cmd = ZZUSB_CMD_PERIODIC_REAP;
            status = send_usb_cmd(base, &cmd, NULL, 0);
            result = (volatile struct ZZUSBCommand*)(base + 0xa000);
            actual = status == ZZUSB_STATUS_OK ? result->actual_length : 0;
            if (actual > ior->iouh_Length)
                actual = ior->iouh_Length;

            if (status == ZZUSB_STATUS_OK) {
                trace_hub_int_data(
                    unit, ior, actual,
                    (volatile uint8_t*)(base + 0xa000 +
                                        ZZUSB_DATA_OFFSET));
                if (ior->iouh_Dir == UHDIR_IN &&
                    actual > 0 && actual <= 2)
                    zero_report = is_zero_report(
                        (volatile uint8_t*)(base + 0xa000 +
                                            ZZUSB_DATA_OFFSET),
                        actual);
            }
            action = zzusb_interrupt_classify(
                status, actual, ior->iouh_Dir == UHDIR_IN, zero_report);
            if (action == ZZUSB_INTERRUPT_COMPLETE) {
                if (ior->iouh_Dir == UHDIR_IN && ior->iouh_Data)
                    safe_copy(
                        (void*)(base + 0xa000 + ZZUSB_DATA_OFFSET),
                        ior->iouh_Data, actual);
                ior->iouh_Actual = actual;
                ior->iouh_Req.io_Error = 0;
                stop_status = stop_periodic_slot(slot);
                if (!periodic_stop_retired(stop_status)) {
                    ior->iouh_Actual = 0;
                    ior->iouh_Req.io_Error = map_proxy_status(stop_status);
                }
                clear_int_slot(slot);
                reply_now = ior;
            } else if (action == ZZUSB_INTERRUPT_FAIL) {
                ior->iouh_Actual = 0;
                ior->iouh_Req.io_Error = map_proxy_status(status);
                stop_periodic_slot(slot);
                clear_int_slot(slot);
                reply_now = ior;
            }
        }

        ReleaseSemaphore(&base_dev->zz_Lock);
        if (reply_now && !(reply_now->iouh_Req.io_Flags & IOF_QUICK))
            ReplyMsg(&reply_now->iouh_Req.io_Message);
    }
}

static void service_rt_iso_during_work(struct ZZUSBBase *base,
                                       struct ZZUSBUnit *unit)
{
    ReleaseSemaphore(&base->zz_Lock);
    poll_rt_iso(unit);
    ObtainSemaphore(&base->zz_Lock);
}

static uint16_t send_usb_cmd_with_rt_service(
    volatile uint8_t *base, struct ZZUSBCommand *cmd,
    void *data_out, uint32_t data_out_len, struct ZZUSBUnit *unit)
{
    uint16_t status;

    if (!PollBase || !rt_iso_pending_for_unit(unit))
        return send_usb_cmd(base, cmd, data_out, data_out_len);
    service_rt_iso_during_work(PollBase, unit);
    if (active_work_aborted())
        return ZZUSB_STATUS_CANCELLED;

    ForegroundMailboxBase = base;
    ForegroundMailboxUnit = unit;
    status = send_usb_cmd(base, cmd, data_out, data_out_len);
    ForegroundMailboxUnit = NULL;
    ForegroundMailboxBase = NULL;
    return status;
}

static int poll_roothub_pending(struct ZZUSBBase *base_dev,
                                struct ZZUSBUnit *unit,
                                int unit_index)
{
    volatile uint8_t *base = (volatile uint8_t*)unit->zz_Registers;
    struct IOUsbHWReq *reply_now = NULL;
    struct IOUsbHWReq *aborted_replies[ZZ_ABORTED_REPLY_SLOTS];
    int aborted_count = 0;
    int still_pending;

    for (int i = 0; i < ZZ_ABORTED_REPLY_SLOTS; i++)
        aborted_replies[i] = NULL;

    ObtainSemaphore(&base_dev->zz_Lock);

    struct IOUsbHWReq *ior = RootHubIntPending[unit_index];
    if (!ior) {
        ReleaseSemaphore(&base_dev->zz_Lock);
        return 0;
    }

    if (unit->zz_PortChange == 0) {
        if (RootHubPollDelay[unit_index] > 0) {
            RootHubPollDelay[unit_index]--;
            ReleaseSemaphore(&base_dev->zz_Lock);
            return 1;
        }
        RootHubPollDelay[unit_index] = ZZ_RH_POLL_DELAY_TICKS;
        update_port_state(unit, base, aborted_replies,
                          &aborted_count, ZZ_ABORTED_REPLY_SLOTS);
    }

    if (unit->zz_PortChange != 0) {
        uint8_t change_bitmap[2] = { 0x02, 0x00 };
        uint16_t len = (ior->iouh_Length < 2) ? ior->iouh_Length : 2;

        trace_port_state(unit, "HUB_INT");
        safe_copy(change_bitmap, ior->iouh_Data, len);
        ior->iouh_Actual = len;
        ior->iouh_Req.io_Error = 0;
        RootHubIntPending[unit_index] = NULL;
        RootHubPollDelay[unit_index] = 0;
        reply_now = ior;
    }

    still_pending = RootHubIntPending[unit_index] != NULL;

    ReleaseSemaphore(&base_dev->zz_Lock);

    if (reply_now && !(reply_now->iouh_Req.io_Flags & IOF_QUICK)) {
        ReplyMsg(&reply_now->iouh_Req.io_Message);
    }
    for (int i = 0; i < aborted_count; i++) {
        struct IOUsbHWReq *p = aborted_replies[i];
        if (p && !(p->iouh_Req.io_Flags & IOF_QUICK)) {
            ReplyMsg(&p->iouh_Req.io_Message);
        }
    }

    return still_pending;
}

static void hotplug_poll_task(void)
{
    /*
     * Async interrupt-delivery loop. The v1.52 design:
     *  - begin_io UHCMD_INTXFER stashes downstream IORs in
     *    IntPendingSlots[] and root-hub IORs in RootHubIntPending[].
     *  - Signal() from begin_io wakes us up.
     *  - We scan pending IORs and reply on report data, root-hub
     *    changes, or hard errors.
     *  - Idle loop Wait()s on our signal; zero CPU when no pending.
     */
    BYTE sig = AllocSignal(-1);
    ULONG mask = (sig >= 0) ? (1UL << sig) : (1UL << 16);
    BYTE timer_sig = AllocSignal(-1);
    ULONG timer_mask = (timer_sig >= 0) ? (1UL << timer_sig) : 0;
    struct MsgPort timer_port;
    struct timerequest timer_req;
    BOOL timer_open = FALSE;

    PollBase->zz_PollSignal = mask;

    if (timer_mask) {
        memset(&timer_port, 0, sizeof(timer_port));
        memset(&timer_req, 0, sizeof(timer_req));
        timer_port.mp_Node.ln_Type = NT_MSGPORT;
        timer_port.mp_Flags = PA_SIGNAL;
        timer_port.mp_SigBit = timer_sig;
        timer_port.mp_SigTask = FindTask(NULL);
        timer_port.mp_MsgList.lh_Head =
            (struct Node *)&timer_port.mp_MsgList.lh_Tail;
        timer_port.mp_MsgList.lh_Tail = NULL;
        timer_port.mp_MsgList.lh_TailPred =
            (struct Node *)&timer_port.mp_MsgList.lh_Head;
        timer_port.mp_MsgList.lh_Type = NT_MESSAGE;
        timer_req.tr_node.io_Message.mn_Node.ln_Type = NT_MESSAGE;
        timer_req.tr_node.io_Message.mn_ReplyPort = &timer_port;
        timer_open = (OpenDevice((CONST_STRPTR)"timer.device", UNIT_MICROHZ,
                                 (struct IORequest*)&timer_req, 0) == 0);
    }

    if (timer_open) {
        WorkerTimerRequest = &timer_req;
        WorkerTimerMask = timer_mask;
        if (!ProtocolNegotiated) {
            negotiate_usb_proxy((volatile uint8_t *)
                                PollBase->zz_Units[0].zz_Registers,
                                &ProtocolStates[0], 1);
            ProtocolNegotiated = 1;
        }
    }

    for (;;) {
        int any_pending = 0;
        int reap_events;

        Disable();
        reap_events = USBEventPending != 0;
        USBEventPending = 0;
        Enable();
        for (int u = 0; u < ZZ_NUM_PORTS; u++)
            if (PollBase->zz_Units[u].zz_Enabled)
                poll_rt_iso(&PollBase->zz_Units[u]);
        while (process_work_queue()) {
            for (int u = 0; u < ZZ_NUM_PORTS; u++)
                if (PollBase->zz_Units[u].zz_Enabled)
                    poll_rt_iso(&PollBase->zz_Units[u]);
        }

        for (int u = 0; u < ZZ_NUM_PORTS; u++) {
            struct ZZUSBUnit *unit = &PollBase->zz_Units[u];
            if (!unit->zz_Enabled) continue;

            if (poll_roothub_pending(PollBase, unit, u)) {
                any_pending = 1;
            }

            poll_int_pending(PollBase, unit, reap_events);

            if (int_pending_for_unit(unit))
                any_pending = 1;
            if (rt_iso_pending_for_unit(unit))
                any_pending = 1;
        }

        if (!any_pending) {
            Wait(mask);
        } else if (timer_open) {
            /*
             * Pending interrupt IORs are normal. Do not spin in a CPU
             * delay loop here: that made the whole OS feel stuck while
             * Poseidon held a hub-status request open. Sleep on
             * timer.device and wake early if begin_io signals new work.
             */
            SetSignal(0, timer_mask);
            timer_req.tr_node.io_Command = TR_ADDREQUEST;
            timer_req.tr_time.tv_secs = 0;
            timer_req.tr_time.tv_micro = 100000;
            SendIO((struct IORequest*)&timer_req);
            Wait(mask | timer_mask);
            if (!CheckIO((struct IORequest*)&timer_req)) {
                AbortIO((struct IORequest*)&timer_req);
            }
            WaitIO((struct IORequest*)&timer_req);
        } else {
            Wait(mask);
        }
    }
}

static void handle_roothub_control(struct ZZUSBUnit *unit,
                                   struct IOUsbHWReq *ior)
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
                trace_port_state(unit, "GET_STATUS");
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
                        if (unit->zz_PortDead) {
                            /*
                             * Direct root-port low-speed failed and was
                             * deliberately hidden until physical unplug.
                             * Poseidon may still have a queued hub-reset
                             * request from the failed enumeration path;
                             * answer it locally as an empty powered port.
                             * Do not touch firmware or re-enable speed bits.
                             */
                            unit->zz_PortPresent = FALSE;
                            unit->zz_PortStatus = UPSF_PORT_POWER;
                            unit->zz_PortChange &= ~UPSF_C_PORT_RESET;
                            trace_port_state(unit, "SET_RESET_DEAD");
                            ior->iouh_Actual = 0;
                            ior->iouh_Req.io_Error = 0;
                            return;
                        }

                        unit->zz_PortStatus |= UPSF_PORT_RESET;
                        unit->zz_PortChange &= ~UPSF_C_PORT_RESET;
                        trace_port_state(unit, "SET_RESET_START");
                        stop_rt_iso_for_unit(unit);
                        bump_unit_generation(unit);

                        struct ZZUSBCommand rcmd;
                        volatile uint8_t *rbase = (volatile uint8_t*)unit->zz_Registers;
                        memset(&rcmd, 0, sizeof(rcmd));
                        rcmd.cmd = ZZUSB_CMD_RESET_PORT;
                        rcmd.timeout_ms = 5000;
                        fill_root_reset_hint(&rcmd, unit);
                        recover_quarantined_proxy(rbase);

                        uint16_t rstatus = send_usb_cmd(rbase, &rcmd, NULL, 0);
                        uint16_t fw_speed = 0;
                        volatile struct ZZUSBCommand *rresult =
                            (volatile struct ZZUSBCommand*)(rbase + 0xa000);
                        fw_speed = rresult->speed;
                        if (rstatus == ZZUSB_STATUS_OK ||
                            rstatus == ZZUSB_STATUS_OFFLINE)
                            finish_reset_rt_iso_for_unit(unit);

                        unit->zz_PortStatus &= ~UPSF_PORT_RESET;
                        if ((rstatus == ZZUSB_STATUS_OK ||
                             rstatus == ZZUSB_STATUS_OFFLINE) &&
                            fw_speed == ZZUSB_SPEED_LOW) {
                            mark_direct_low_speed_unsupported(unit,
                                                              "LS_ROOT_IGNORE");
                            trace_port_state_status(unit, "SET_RESET_FW",
                                                    rstatus, fw_speed);
                            trace_port_state(unit, "SET_RESET_DONE");
                            ior->iouh_Actual = 0;
                            ior->iouh_Req.io_Error = 0;
                            return;
                        } else if (rstatus == ZZUSB_STATUS_OK) {
                            unit->zz_Speed = rresult->speed;
                            unit->zz_PortStatus |= UPSF_PORT_ENABLE;
                            unit->zz_PortStatus &= ~(UPSF_PORT_HIGH_SPEED |
                                                     UPSF_PORT_LOW_SPEED);
                            if (unit->zz_Speed == ZZUSB_SPEED_HIGH) {
                                unit->zz_PortStatus |= UPSF_PORT_HIGH_SPEED;
                            } else if (unit->zz_Speed == ZZUSB_SPEED_LOW) {
                                unit->zz_PortStatus |= UPSF_PORT_LOW_SPEED;
                            }
                        } else {
                            unit->zz_PortStatus &= ~(UPSF_PORT_ENABLE |
                                                     UPSF_PORT_HIGH_SPEED |
                                                     UPSF_PORT_LOW_SPEED);
                        }
                        unit->zz_PortChange |= UPSF_C_PORT_RESET;
                        trace_port_state_status(unit, "SET_RESET_FW",
                                                rstatus, fw_speed);
                        trace_port_state(unit, "SET_RESET_DONE");
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
                    trace_port_state(unit, "CLR_ENABLE");
                    ior->iouh_Actual = 0;
                    ior->iouh_Req.io_Error = 0;
                    return;
                case UFS_C_PORT_CONNECTION:
                    unit->zz_PortChange &= ~UPSF_C_PORT_CONNECTION;
                    trace_port_state(unit, "CLR_C_CONN");
                    ior->iouh_Actual = 0;
                    ior->iouh_Req.io_Error = 0;
                    return;
                case UFS_C_PORT_ENABLE:
                    unit->zz_PortChange &= ~UPSF_C_PORT_ENABLE;
                    trace_port_state(unit, "CLR_C_ENABLE");
                    ior->iouh_Actual = 0;
                    ior->iouh_Req.io_Error = 0;
                    return;
                case UFS_C_PORT_RESET:
                    unit->zz_PortChange &= ~UPSF_C_PORT_RESET;
                    trace_port_state(unit, "CLR_C_RESET");
                    ior->iouh_Actual = 0;
                    ior->iouh_Req.io_Error = 0;
                    return;
                case UFS_C_PORT_SUSPEND:
                    unit->zz_PortChange &= ~UPSF_C_PORT_SUSPEND;
                    trace_port_state(unit, "CLR_C_SUSP");
                    ior->iouh_Actual = 0;
                    ior->iouh_Req.io_Error = 0;
                    return;
                case UFS_C_PORT_OVER_CURRENT:
                    unit->zz_PortChange &= ~UPSF_C_PORT_OVER_CURRENT;
                    trace_port_state(unit, "CLR_C_OC");
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

static int handle_roothub_int(struct ZZUSBUnit *unit,
                              int unit_index,
                              struct IOUsbHWReq *ior,
                              struct IOUsbHWReq **aborted,
                              int *aborted_count,
                              int aborted_max)
{
    if (ior->iouh_Endpoint == 1 && ior->iouh_Data) {
        /*
         * Port checks use the poll task's timer request. Queue a quiet
         * root-hub IOR and signal that task instead of waiting from the
         * Poseidon caller; an already-latched change needs no mailbox.
         */
        if (unit->zz_PortChange == 0) {
            struct IOUsbHWReq *old = RootHubIntPending[unit_index];
            if (old && old != ior) {
                old->iouh_Actual = 0;
                old->iouh_Req.io_Error = IOERR_ABORTED;
                if (aborted && aborted_count && *aborted_count < aborted_max) {
                    aborted[(*aborted_count)++] = old;
                }
            }
            ior->iouh_Req.io_Flags &= ~IOF_QUICK;
            RootHubIntPending[unit_index] = ior;
            RootHubPollDelay[unit_index] = 0;
            trace_int_status(unit, "RH_INT_WAIT", ior);
            return 1;
        }
        uint8_t change_bitmap[2] = { 0x02, 0x00 };
        uint16_t len = (ior->iouh_Length < 2) ? ior->iouh_Length : 2;
        trace_port_state(unit, "HUB_INT");
        safe_copy(change_bitmap, ior->iouh_Data, len);
        ior->iouh_Actual = len;
        ior->iouh_Req.io_Error = 0;
    } else {
        ior->iouh_Actual = 0;
        ior->iouh_Req.io_Error = UHIOERR_STALL;
    }
    return 0;
}

/*
 * Returns the number of UHA_* tags actually populated. Caller writes
 * this into iouh_Actual so NSD-aware Poseidon tools can detect tag
 * coverage.
 */
static int fill_querydevice_tags(struct ZZUSBUnit *unit, struct TagItem *tags)
{
    /*
     * Poseidon normally sends a flat taglist, but TagItem control
     * tags are legal API input. Handle them here so a TAG_MORE or
     * TAG_SKIP list cannot make the driver walk unrelated memory.
     * Like Deneb, ti_Data points at the caller's output storage.
     * Leave unknown newer Poseidon tags untouched.
     */
    int guard = 64;
    int count = 0;

    while (tags && guard-- > 0) {
        switch (tags->ti_Tag) {
        case TAG_DONE:
            return count;
        case TAG_IGNORE:
            tags++;
            continue;
        case TAG_MORE:
            tags = (struct TagItem *)tags->ti_Data;
            continue;
        case TAG_SKIP:
            tags += tags->ti_Data + 1;
            continue;
        case UHA_DriverVersion:
            count += write_tag_ulong(tags, 0x0200);
            break;
        case UHA_Version:
            count += write_tag_ulong(tags, DEVICE_VERSION);
            break;
        case UHA_Revision:
            count += write_tag_ulong(tags, DEVICE_REVISION);
            break;
        case UHA_State:
            count += write_tag_ulong(tags, UHSF_OPERATIONAL);
            break;
        case UHA_Manufacturer:
            count += write_tag_str(tags, "MNT Research GmbH");
            break;
        case UHA_ProductName:
            count += write_tag_str(tags, "ZZ9000 USB Host Controller");
            break;
        case UHA_Description:
            count += write_tag_str(tags,
                "Poseidon USB hardware driver for the ZZ9000 "
                "Zorro card (Zynq ChipIdea EHCI)");
            break;
        case UHA_Copyright:
            count += write_tag_str(tags,
                "(C) Copyright 2026 Dimitris Panokostas. "
                "Licensed under GNU GPL v3 or later.");
            break;
        case UHA_Capabilities:
            count += write_tag_ulong(tags,
                                     iso_public_capabilities(unit));
            break;
        case UHA_RootHubAddr:
            count += write_tag_ulong(tags, unit ? unit->zz_RootHubAddr : 0);
            break;
        default:
            break;
        }
        tags++;
    }
    return count;
}

/*
 * NSD (NewStyleDevice) command table. Runtime UHA_Capabilities remains
 * authoritative: ISO commands reject use unless the matched firmware
 * advertises the corresponding transport.
 */
#ifndef NSCMD_DEVICEQUERY
#define NSCMD_DEVICEQUERY 0x4000
#endif
#ifndef NSDEVTYPE_USBHARDWARE
#define NSDEVTYPE_USBHARDWARE 14
#endif
static int write_tag_ulong(struct TagItem *tag, ULONG value)
{
    ULONG *out = (ULONG *)(uintptr_t)tag->ti_Data;
    if (!out)
        return 0;
    *out = value;
    return 1;
}

static int write_tag_str(struct TagItem *tag, const char *value)
{
    STRPTR *out = (STRPTR *)(uintptr_t)tag->ti_Data;
    if (!out)
        return 0;
    *out = (STRPTR)value;
    return 1;
}

static const UWORD NSDSupportedCommands[] = {
    CMD_RESET,
    CMD_FLUSH,
    NSCMD_DEVICEQUERY,
    UHCMD_QUERYDEVICE,
    UHCMD_USBRESET,
    UHCMD_USBRESUME,
    UHCMD_USBSUSPEND,
    UHCMD_USBOPER,
    UHCMD_CONTROLXFER,
    UHCMD_BULKXFER,
    UHCMD_INTXFER,
    UHCMD_ISOXFER,
    UHCMD_ADDISOHANDLER,
    UHCMD_REMISOHANDLER,
    UHCMD_STARTRTISO,
    UHCMD_STOPRTISO,
    0
};

struct ZZNSDeviceQueryResult {
    ULONG  DevQueryFormat;
    ULONG  SizeAvailable;
    UWORD  DeviceType;
    UWORD  DeviceSubType;
    const UWORD *SupportedCommands;
};

static void execute_io(struct Library *dev, struct IOUsbHWReq *ior)
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
    enum { ABORTED_REPLY_MAX = ZZ_ABORTED_REPLY_SLOTS };
    struct IOUsbHWReq *aborted_replies[ABORTED_REPLY_MAX];
    int aborted_count = 0;
    for (int _i = 0; _i < ABORTED_REPLY_MAX; _i++) aborted_replies[_i] = NULL;

    if (!ZZBase || !ior) return;

    unit = (struct ZZUSBUnit*)ior->iouh_Req.io_Unit;
    if (!unit) {
        ior->iouh_Req.io_Error = IOERR_NOCMD;
        if (!(ior->iouh_Req.io_Flags & IOF_QUICK)) {
            ReplyMsg(&ior->iouh_Req.io_Message);
        }
        return;
    }

    volatile uint8_t* base = (volatile uint8_t*)unit->zz_Registers;

    ObtainSemaphore(&ZZBase->zz_Lock);

    switch (ior->iouh_Req.io_Command) {
    case ZZUSB_UHCMD_GET_DIAGNOSTICS:
        if (!ior->iouh_Data ||
            ior->iouh_Length < sizeof(DriverDiagSnapshot)) {
            ior->iouh_Actual = 0;
            ior->iouh_Req.io_Error = UHIOERR_BADPARAMS;
        } else if (!zzusb_engine_diag_snapshot(
                       &DriverDiagSnapshot,
                       ProtocolStates[0].capabilities,
                       ProtocolStates[0].controller_epoch, 4U)) {
            ior->iouh_Actual = 0;
            ior->iouh_Req.io_Error = UHIOERR_HOSTERROR;
        } else {
            safe_copy(&DriverDiagSnapshot, ior->iouh_Data,
                      sizeof(DriverDiagSnapshot));
            ior->iouh_Actual = sizeof(DriverDiagSnapshot);
            ior->iouh_Req.io_Error = 0;
        }
        break;
    case UHCMD_QUERYDEVICE:
        {
            int filled = fill_querydevice_tags(unit,
                (struct TagItem *)ior->iouh_Data);
            ior->iouh_Actual = filled;
            ior->iouh_Req.io_Error = 0;
        }
        break;

    case NSCMD_DEVICEQUERY:
        {
            /*
             * NSD probe. The IOR is only guaranteed to be sized as
             * IOStdReq (callers may not pass an IOUsbHWReq); use the
             * IOStdReq overlay for io_Data / io_Length / io_Actual.
             */
            struct IOStdReq *std = (struct IOStdReq *)ior;
            struct ZZNSDeviceQueryResult *q =
                (struct ZZNSDeviceQueryResult *)std->io_Data;
            /*
             * SizeAvailable is an output field per the NSD spec, but
             * real callers reuse the buffer between probes — strict
             * "must be zero on entry" enforcement (as Deneb does) makes
             * the second probe spuriously fail. We only validate the
             * fields the caller is unambiguously responsible for: a
             * non-null buffer, sufficient length, and DevQueryFormat
             * being the only format we know how to fill (0).
             */
            if (!q ||
                std->io_Length < sizeof(struct ZZNSDeviceQueryResult) ||
                q->DevQueryFormat != 0) {
                std->io_Error = IOERR_NOCMD;
                break;
            }
            q->SizeAvailable     = sizeof(struct ZZNSDeviceQueryResult);
            q->DeviceType        = NSDEVTYPE_USBHARDWARE;
            q->DeviceSubType     = 0;
            q->SupportedCommands = NSDSupportedCommands;
            std->io_Actual       = sizeof(struct ZZNSDeviceQueryResult);
            std->io_Error        = 0;
        }
        break;

    case UHCMD_USBRESET:
        {
            struct ZZUSBCommand cmd;
            stop_rt_iso_for_unit(unit);
            bump_unit_generation(unit);
            memset(&cmd, 0, sizeof(cmd));
            cmd.cmd = ZZUSB_CMD_RESET_PORT;
            cmd.timeout_ms = 5000;
            fill_root_reset_hint(&cmd, unit);
            recover_quarantined_proxy(base);

            uint16_t status = send_usb_cmd(base, &cmd, NULL, 0);
            uint16_t fw_speed = 0;
            volatile struct ZZUSBCommand *result =
                (volatile struct ZZUSBCommand*)(base + 0xa000);
            fw_speed = result->speed;
            if (status == ZZUSB_STATUS_OK ||
                status == ZZUSB_STATUS_OFFLINE)
                finish_reset_rt_iso_for_unit(unit);

            int empty_port = 0;
            if (status == ZZUSB_STATUS_OK) {
                if (result->speed == ZZUSB_SPEED_LOW) {
                    mark_direct_low_speed_unsupported(unit, "LS_ROOT_IGNORE");
                    trace_port_state_status(unit, "USBRESET_FW",
                                            status, fw_speed);
                    trace_port_state(unit, "USBRESET_DONE");
                    ior->iouh_Req.io_Error = 0;
                    ior->iouh_State = UHSF_OPERATIONAL;
                    break;
                }
                /*
                 * Only flag POWER + CONNECTION + speed here.
                 * Poseidon's hub class drives enable/C_RESET through the
                 * subsequent root-hub class requests.
                 */
                UWORD port_status = UPSF_PORT_POWER | UPSF_PORT_CONNECTION;
                if (result->speed == ZZUSB_SPEED_HIGH) {
                    port_status |= UPSF_PORT_HIGH_SPEED;
                } else {
                    port_status |= UPSF_PORT_ENABLE;
                    if (result->speed == ZZUSB_SPEED_LOW)
                        port_status |= UPSF_PORT_LOW_SPEED;
                }
                unit->zz_Speed = result->speed;
                unit->zz_PortPresent = TRUE;
                unit->zz_PortStatus = port_status;
                unit->zz_PortChange = UPSF_C_PORT_CONNECTION;
            } else {
                if (status == ZZUSB_STATUS_OFFLINE &&
                    fw_speed == ZZUSB_SPEED_LOW) {
                    mark_direct_low_speed_unsupported(unit, "LS_ROOT_IGNORE");
                    empty_port = 1;
                } else {
                    struct ZZUSBCommand check;
                    unit->zz_PortPresent = FALSE;
                    unit->zz_PortStatus = UPSF_PORT_POWER;
                    unit->zz_PortChange = 0;
                    unit->zz_Speed = 0;

                    memset(&check, 0, sizeof(check));
                    check.cmd = ZZUSB_CMD_CHECK_PORT;
                    check.timeout_ms = 250;
                    empty_port =
                        send_usb_cmd(base, &check, NULL, 0) ==
                        ZZUSB_STATUS_OFFLINE;
                }
            }
            trace_port_state_status(unit, "USBRESET_FW", status, fw_speed);
            trace_port_state(unit, "USBRESET_DONE");

            if (status == ZZUSB_STATUS_OK || empty_port) {
                ior->iouh_Req.io_Error = 0;
                ior->iouh_State = UHSF_OPERATIONAL;
            } else {
                ior->iouh_Req.io_Error = map_proxy_status(status);
                ior->iouh_State = 0;
            }
        }
        break;

    case UHCMD_CONTROLXFER:
        {
            uint16_t rh_addr = unit->zz_RootHubAddr;

            if ((rh_addr == 0 && ior->iouh_DevAddr == 0) || ior->iouh_DevAddr == rh_addr) {
                handle_roothub_control(unit, ior);
            } else {
                struct ZZUSBCommand cmd;
                uint16_t status;
                int setup_in;

                if (ior->iouh_Length > ZZUSB_MAX_XFER) {
                    ior->iouh_Req.io_Error = UHIOERR_PKTTOOLARGE;
                    break;
                }

                if (unit->zz_PortDead) {
                    ior->iouh_Actual = 0;
                    ior->iouh_Req.io_Error = UHIOERR_USBOFFLINE;
                    break;
                }

                setup_in = (ior->iouh_SetupData.bmRequestType & 0x80) != 0;

                memset(&cmd, 0, sizeof(cmd));
                cmd.cmd = ZZUSB_CMD_CONTROL_XFER;
                cmd.dev_addr = ior->iouh_DevAddr;
                cmd.endpoint = ior->iouh_Endpoint;
                cmd.direction = setup_in ? 0x80 : 0x00;
                cmd.max_pkt_size = ior->iouh_MaxPktSize;
                cmd.speed = request_speed(unit, ior);
                if (is_direct_root_addr0(unit, ior)) {
                    /*
                     * During address-0 enumeration Poseidon may not yet
                     * have reliable per-device speed flags. The root hub
                     * reset result is authoritative here; using a split
                     * or high-speed flag from the IOR can mis-drive a
                     * low-speed mouse as full-speed before it even has
                     * an address.
                     */
                    cmd.speed = unit->zz_Speed;
                }
                cmd.data_length = ior->iouh_Length;
                cmd.timeout_ms = (ior->iouh_Flags & UHFF_NAKTIMEOUT)
                                 ? (ior->iouh_NakTimeout ? ior->iouh_NakTimeout : 5000)
                                 : 0;
                if (is_direct_root_addr0(unit, ior) &&
                    unit->zz_Speed == ZZUSB_SPEED_LOW &&
                    is_addr0_get_device_desc(ior)) {
                    /*
                     * Firmware has its own short internal bound for
                     * this unsupported direct-LS root-port probe. Keep
                     * the Amiga-side mailbox wait comfortably longer so
                     * debug printing and EHCI cleanup cannot race the
                     * next Poseidon command into the shared mailbox.
                     */
                    cmd.timeout_ms = 250;
                }
                fill_split_fields(&cmd, unit, ior);

                cmd.setup_bRequestType = ior->iouh_SetupData.bmRequestType;
                cmd.setup_bRequest = ior->iouh_SetupData.bRequest;
                cmd.setup_wValue = ior->iouh_SetupData.wValue;
                cmd.setup_wIndex = ior->iouh_SetupData.wIndex;
                cmd.setup_wLength = ior->iouh_SetupData.wLength;

                status = send_usb_cmd_with_rt_service(
                    base, &cmd, (!setup_in) ? ior->iouh_Data : NULL,
                    (!setup_in) ? ior->iouh_Length : 0, unit);

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

                    if (setup_in && ior->iouh_Data) {
                        /*
                         * Clear the caller's full destination first. A
                         * short control read must not expose bytes left by
                         * an older request beyond iouh_Actual.
                         */
                        safe_zero(ior->iouh_Data, ior->iouh_Length);
                        if (actual > 0) {
                            safe_copy((void*)(base + 0xa000 + ZZUSB_DATA_OFFSET),
                                      ior->iouh_Data, actual);
                        }
                    }
                    ior->iouh_Actual = actual;
                    ior->iouh_Req.io_Error = 0;
                } else {
                    trace_control_status(unit, "CTRL_FAIL", ior, status);
                    ior->iouh_Actual = 0;
                    ior->iouh_Req.io_Error = map_proxy_status(status);
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

    case UHCMD_ISOXFER:
        execute_simple_iso(unit, ior, base);
        break;

    case UHCMD_ADDISOHANDLER:
        ior->iouh_Actual = 0;
        ior->iouh_Req.io_Error = add_rt_iso_handler(unit, ior);
        break;

    case UHCMD_STARTRTISO:
        ior->iouh_Actual = 0;
        ior->iouh_Req.io_Error = start_rt_iso_handler(unit, ior);
        break;

    case UHCMD_STOPRTISO:
        ior->iouh_Actual = 0;
        ior->iouh_Req.io_Error = stop_rt_iso_handler(unit, ior);
        break;

    case UHCMD_REMISOHANDLER:
        ior->iouh_Actual = 0;
        ior->iouh_Req.io_Error = remove_rt_iso_handler(unit, ior);
        break;

    case UHCMD_INTXFER:
        {
            uint16_t rh_addr = unit->zz_RootHubAddr;

            if (((rh_addr == 0 && ior->iouh_DevAddr == 0) || ior->iouh_DevAddr == rh_addr)
                && ior->iouh_Endpoint == 1) {
                int unit_index = (int)(unit - &ZZBase->zz_Units[0]);
                if (handle_roothub_int(unit, unit_index, ior,
                                       aborted_replies, &aborted_count,
                                       ABORTED_REPLY_MAX)) {
                    deferred = 1;
                    ensure_poll_task(ZZBase);
                    if (ZZBase->zz_PollTask && ZZBase->zz_PollSignal) {
                        Signal(ZZBase->zz_PollTask, ZZBase->zz_PollSignal);
                    }
                }
            } else {
                /*
                 * Async delivery for downstream interrupt endpoints.
                 * Stash the IOR, Signal the poll task, defer the
                 * reply. The task replies on report data, occasional
                 * idle completion, or real offline. This avoids a
                 * tight Poseidon-side interrupt-poll loop while the
                 * firmware-side EHCI poll remains bounded.
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

                ior->iouh_Req.io_Flags &= ~IOF_QUICK;
                if (!queue_int_ior(unit, ior, &deferred_old_ior)) {
                    ior->iouh_Req.io_Error = UHIOERR_OUTOFMEMORY;
                    break;
                }
                deferred = 1;      /* do NOT ReplyMsg at bottom */
                ensure_poll_task(ZZBase);

                if (ZZBase->zz_PollTask && ZZBase->zz_PollSignal) {
                    Signal(ZZBase->zz_PollTask, ZZBase->zz_PollSignal);
                }
            }
        }
        break;

    case UHCMD_BULKXFER:
        {
            /*
             * USB 2.0 forbids low-speed bulk; firmware behaviour is
             * undefined for this combination. Reject up front so a
             * misconfigured class driver gets a clear answer instead
             * of a silent stall later.
             */
            if (ior->iouh_Flags & UHFF_LOWSPEED) {
                ior->iouh_Actual = 0;
                ior->iouh_Req.io_Error = UHIOERR_BADPARAMS;
                break;
            }
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
            int realtime_active = rt_iso_pending_for_unit(unit);

            while (remaining > 0) {
                if (active_work_aborted()) {
                    status = ZZUSB_STATUS_CANCELLED;
                    break;
                }
                /*
                 * A realtime retry must cover at most one USB packet:
                 * a timed-out qTD then has either completed atomically or
                 * transferred nothing, so resubmission cannot duplicate a
                 * partially completed bulk payload.
                 */
                uint32_t rt_packet = ior->iouh_MaxPktSize & 0x07ffU;
                uint32_t limit = realtime_active ?
                    (rt_packet ? rt_packet : 64U) : BULK_CHUNK;
                uint32_t chunk = remaining > limit ? limit : remaining;

                memset(&cmd, 0, sizeof(cmd));
                cmd.cmd = ZZUSB_CMD_BULK_XFER;
                cmd.dev_addr = ior->iouh_DevAddr;
                cmd.endpoint = ior->iouh_Endpoint;
                cmd.direction = (ior->iouh_Dir == UHDIR_IN) ? 0x80 : 0x00;
                cmd.max_pkt_size = ior->iouh_MaxPktSize;
                cmd.speed = request_speed(unit, ior);
                cmd.data_length = chunk;
                cmd.timeout_ms =
                    (ior->iouh_Flags & UHFF_NAKTIMEOUT)
                    ? (ior->iouh_NakTimeout ? ior->iouh_NakTimeout : 500)
                    : 500;
                fill_split_fields(&cmd, unit, ior);

                status = send_usb_cmd_with_rt_service(
                    base, &cmd,
                    (ior->iouh_Dir == UHDIR_OUT && user_buf) ?
                        user_buf : NULL,
                    (ior->iouh_Dir == UHDIR_OUT) ? chunk : 0, unit);

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
                if (realtime_active && !active_work_aborted())
                    service_rt_iso_during_work(ZZBase, unit);
            }

            if (status == ZZUSB_STATUS_OK) {
                ior->iouh_Actual = total_actual;
                unit->zz_BulkErrCount = 0;
                ior->iouh_Req.io_Error = 0;
            } else {
                /*
                 * Actual length is valid only for a matching successful
                 * completion. Preserve the firmware error class exactly;
                 * controller recovery is driven by status/epoch, not by a
                 * retry counter that silently changes the result.
                 */
                ior->iouh_Actual = 0;
                ior->iouh_Req.io_Error = map_proxy_status(status);
            }
        }
        break;

    case CMD_RESET:
    case CMD_FLUSH:
        /*
         * Abort every queued root/downstream interrupt IOR. Collect them
         * here; actual ReplyMsg happens AFTER zz_Lock is released
         * to avoid scheduling issues if a replied task immediately
         * re-enters our driver.
         */
        {
            int unit_index = (int)(unit - &ZZBase->zz_Units[0]);
            struct IOUsbHWReq *pending = RootHubIntPending[unit_index];
            if (pending) {
                RootHubIntPending[unit_index] = NULL;
                RootHubPollDelay[unit_index] = 0;
                pending->iouh_Actual = 0;
                pending->iouh_Req.io_Error = IOERR_ABORTED;
                if (aborted_count < ABORTED_REPLY_MAX)
                    aborted_replies[aborted_count++] = pending;
            }
        }
        for (int i = 0; i < ZZ_INT_PENDING_SLOTS; i++) {
            struct ZZIntPendingSlot *slot = &IntPendingSlots[i];
            struct IOUsbHWReq *pending = slot->ior;
            if (!pending || slot->unit != unit) continue;
            stop_periodic_slot(slot);
            clear_int_slot(slot);
            pending->iouh_Actual = 0;
            pending->iouh_Req.io_Error = IOERR_ABORTED;
            if (aborted_count < ABORTED_REPLY_MAX)
                aborted_replies[aborted_count++] = pending;
        }
        stop_rt_iso_for_unit(unit);
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

static int command_uses_worker(UWORD command)
{
    switch (command) {
    case ZZUSB_UHCMD_GET_DIAGNOSTICS:
    case UHCMD_QUERYDEVICE:
    case UHCMD_USBRESET:
    case UHCMD_USBRESUME:
    case UHCMD_USBSUSPEND:
    case UHCMD_USBOPER:
    case UHCMD_CONTROLXFER:
    case UHCMD_BULKXFER:
    case UHCMD_ISOXFER:
    case UHCMD_ADDISOHANDLER:
    case UHCMD_REMISOHANDLER:
    case UHCMD_STARTRTISO:
    case UHCMD_STOPRTISO:
    case CMD_RESET:
    case CMD_FLUSH:
        return 1;
    default:
        return 0;
    }
}

static int enqueue_work(struct ZZUSBBase *base, struct ZZUSBUnit *unit,
                        struct IOUsbHWReq *ior)
{
    struct ZZWorkSlot *available = NULL;
    uint32_t depth = 0;

    Forbid();
    for (int i = 0; i < ZZ_WORK_SLOTS; i++) {
        if (!WorkSlots[i].ior) {
            available = &WorkSlots[i];
            break;
        }
    }
    if (available) {
        zzusb_engine_init(&available->lifecycle);
        available->unit = unit;
        available->ior = ior;
        available->sequence = 0;
        available->enqueue_seq = EnqueueSequence++;
        zzusb_engine_queue(&available->lifecycle);
        ior->iouh_Actual = 0;
        ior->iouh_Req.io_Error = 0;
        ior->iouh_Req.io_Flags &= ~IOF_QUICK;
        for (int i = 0; i < ZZ_WORK_SLOTS; i++) {
            if (WorkSlots[i].ior)
                depth++;
        }
    }
    Permit();

    if (!available)
        return 0;
    if (zzusb_engine_diag_high_water(depth))
        zzusb_engine_diag_record(ZZUSB_DRIVER_EVENT_HIGH_WATER,
                                 ZZUSB_ENGINE_STATUS_PENDING,
                                 0, ProtocolStates[0].controller_epoch,
                                 0, 0, 0, 0, 0, depth, WorkSequence);
    if (base->zz_PollTask && base->zz_PollSignal)
        Signal(base->zz_PollTask, base->zz_PollSignal);
    return 1;
}

static int work_queue_pending(void)
{
    int pending = 0;

    Forbid();
    for (int i = 0; i < ZZ_WORK_SLOTS; i++) {
        if (WorkSlots[i].ior &&
            WorkSlots[i].lifecycle.state == ZZUSB_REQ_QUEUED) {
            pending = 1;
            break;
        }
    }
    Permit();
    return pending;
}

static int process_work_queue(void)
{
    struct ZZWorkSlot *slot = NULL;
    struct IOUsbHWReq *ior;
    UBYTE saved_flags;
    int reply;

    Forbid();
    for (int i = 0; i < ZZ_WORK_SLOTS; i++) {
        struct ZZWorkSlot *candidate = &WorkSlots[i];

        if (candidate->ior &&
            candidate->lifecycle.state == ZZUSB_REQ_QUEUED &&
            (!slot || candidate->enqueue_seq < slot->enqueue_seq))
            slot = candidate;
    }
    if (!slot) {
        Permit();
        return 0;
    }
    zzusb_engine_dispatch(&slot->lifecycle);
    slot->sequence = WorkSequence++;
    if (slot->sequence == 0)
        slot->sequence = WorkSequence++;
    zzusb_engine_begin(&slot->lifecycle, slot->sequence,
                       ProtocolStates[0].controller_epoch);
    ActiveWorkSlot = slot;
    ior = slot->ior;
    Permit();

    saved_flags = ior->iouh_Req.io_Flags;
    ior->iouh_Req.io_Flags |= IOF_QUICK;
    execute_io((struct Library *)PollBase, ior);
    ior->iouh_Req.io_Flags = saved_flags;

    Forbid();
    if (slot->lifecycle.abort_requested) {
        ior->iouh_Actual = 0;
        ior->iouh_Req.io_Error = IOERR_ABORTED;
    }
    zzusb_engine_complete(&slot->lifecycle, slot->sequence,
                          slot->lifecycle.controller_epoch,
                          ior->iouh_Req.io_Error == 0
                            ? ZZUSB_ENGINE_STATUS_OK
                            : ZZUSB_ENGINE_STATUS_HOSTERROR);
    reply = zzusb_engine_claim_reply(&slot->lifecycle);
    zzusb_engine_release_buffer(&slot->lifecycle);
    ActiveWorkSlot = NULL;
    slot->ior = NULL;
    slot->unit = NULL;
    zzusb_engine_init(&slot->lifecycle);
    Permit();

    if (reply)
        ReplyMsg(&ior->iouh_Req.io_Message);
    return work_queue_pending();
}

static int abort_work_ior(struct IOUsbHWReq *ior, int *reply)
{
    struct ZZWorkSlot *slot = NULL;

    *reply = 0;
    Forbid();
    for (int i = 0; i < ZZ_WORK_SLOTS; i++) {
        if (WorkSlots[i].ior == ior) {
            slot = &WorkSlots[i];
            break;
        }
    }
    if (!slot) {
        Permit();
        return 0;
    }

    zzusb_engine_abort(&slot->lifecycle);
    if (slot->lifecycle.state == ZZUSB_REQ_TERMINAL) {
        ior->iouh_Actual = 0;
        ior->iouh_Req.io_Error = IOERR_ABORTED;
        *reply = zzusb_engine_claim_reply(&slot->lifecycle);
        zzusb_engine_release_buffer(&slot->lifecycle);
        slot->ior = NULL;
        slot->unit = NULL;
        zzusb_engine_init(&slot->lifecycle);
    }
    Permit();

    if (PollBase && PollBase->zz_PollTask && PollBase->zz_PollSignal)
        Signal(PollBase->zz_PollTask, PollBase->zz_PollSignal);
    return 1;
}

static void abort_unit_work(struct ZZUSBUnit *unit)
{
    struct IOUsbHWReq *replies[ZZ_WORK_SLOTS];
    int reply_count = 0;

    Forbid();
    for (int i = 0; i < ZZ_WORK_SLOTS; i++) {
        struct ZZWorkSlot *slot = &WorkSlots[i];
        if (!slot->ior || slot->unit != unit)
            continue;

        zzusb_engine_abort(&slot->lifecycle);
        if (slot->lifecycle.state != ZZUSB_REQ_TERMINAL)
            continue;

        slot->ior->iouh_Actual = 0;
        slot->ior->iouh_Req.io_Error = IOERR_ABORTED;
        if (zzusb_engine_claim_reply(&slot->lifecycle))
            replies[reply_count++] = slot->ior;
        zzusb_engine_release_buffer(&slot->lifecycle);
        slot->ior = NULL;
        slot->unit = NULL;
        zzusb_engine_init(&slot->lifecycle);
    }
    Permit();

    for (int i = 0; i < reply_count; i++)
        ReplyMsg(&replies[i]->iouh_Req.io_Message);
}

static void __attribute__((used)) begin_io(
    struct Library *dev asm("a6"), struct IOUsbHWReq *ior asm("a1"))
{
    struct ZZUSBBase *base = (struct ZZUSBBase *)dev;
    struct ZZUSBUnit *unit;

    if (!base || !ior) {
        execute_io(dev, ior);
        return;
    }
    unit = (struct ZZUSBUnit *)ior->iouh_Req.io_Unit;
    if (unit && (ior->iouh_Req.io_Command == CMD_RESET ||
                 ior->iouh_Req.io_Command == CMD_FLUSH))
        abort_unit_work(unit);
    if (unit && command_uses_worker(ior->iouh_Req.io_Command)) {
        ensure_poll_task(base);
        if (!enqueue_work(base, unit, ior)) {
            ior->iouh_Actual = 0;
            ior->iouh_Req.io_Error = UHIOERR_OUTOFMEMORY;
            if (!(ior->iouh_Req.io_Flags & IOF_QUICK))
                ReplyMsg(&ior->iouh_Req.io_Message);
        }
        return;
    }
    execute_io(dev, ior);
}

static uint32_t __attribute__((used)) abort_io(struct Library *dev asm("a6"), struct IOUsbHWReq *ior asm("a1"))
{
    /*
     * Abort a queued downstream interrupt IOR. All access is
     * serialised by zz_Lock.
     */
    struct ZZUSBBase *ZZBase = (struct ZZUSBBase*)dev;
    if (!ior || !ZZBase) return IOERR_NOCMD;
    struct ZZUSBUnit *unit = (struct ZZUSBUnit *)ior->iouh_Req.io_Unit;
    if (!unit) {
        ior->iouh_Req.io_Error = IOERR_ABORTED;
        return IOERR_ABORTED;
    }
    {
        int work_reply = 0;

        if (abort_work_ior(ior, &work_reply)) {
            if (work_reply)
                ReplyMsg(&ior->iouh_Req.io_Message);
            return IOERR_ABORTED;
        }
    }

    ObtainSemaphore(&ZZBase->zz_Lock);
    int found = 0;
    int unit_index = (int)(unit - &ZZBase->zz_Units[0]);
    if (unit_index >= 0 && unit_index < ZZ_NUM_PORTS &&
        RootHubIntPending[unit_index] == ior) {
        RootHubIntPending[unit_index] = NULL;
        RootHubPollDelay[unit_index] = 0;
        found = 1;
    }
    {
        struct ZZIntPendingSlot *slot = find_int_slot_for_ior(ior);
        if (slot && slot->unit == unit) {
            if (slot->armed) {
                slot->abort_requested = 1;
                found = 2;
            } else {
                clear_int_slot(slot);
                found = 1;
            }
        }
    }
    ReleaseSemaphore(&ZZBase->zz_Lock);
    if (found == 2) {
        if (ZZBase->zz_PollTask && ZZBase->zz_PollSignal)
            Signal(ZZBase->zz_PollTask, ZZBase->zz_PollSignal);
        return IOERR_ABORTED;
    }

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
