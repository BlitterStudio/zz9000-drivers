/*
 * ZZ9000 Poseidon USB Hardware Driver (zzusbhw.device)
 * Copyright (C) 2026, MNT Research GmbH
 * Licensed under the MIT License.
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

#define DEVICE_ID_STRING DEVICE_NAME " " XSTR(DEVICE_VERSION) "." XSTR(DEVICE_REVISION)

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
 * field verification of driver deployment.
 */
static const char __attribute__((used)) version_tag[] =
    "$VER: " DEVICE_NAME " " XSTR(DEVICE_VERSION) "." XSTR(DEVICE_REVISION);

/* Push a NUL-terminated string to the ZZ9000 serial debug channel. */
static void dstr(void* regs, char* str)
{
    while (*str) {
        *((volatile uint16_t*)((uint8_t*)regs + 0xF0)) = *str++;
    }
}

/*
 * Alignment-safe memcpy used in place of AmigaOS CopyMem, which has
 * been observed to silently no-op on this toolchain when the source
 * is at an odd address (GCC 15 can place static const uint8_t[]
 * tables at arbitrary offsets). Poseidon's iouh_Data buffers can
 * also arrive at odd addresses, so neither end is trustworthy.
 *
 * The byte loop at the bottom is the safe fallback. When both src
 * and dst share 4-byte or 2-byte alignment we upgrade to MOVE.L or
 * MOVE.W copies — roughly a 4x / 2x speedup on bulk transfers (up
 * to 24 KB per IOR on mass-storage reads).
 */
static void safe_copy(const void *src, void *dst, uint32_t n)
{
    const uint8_t *s = (const uint8_t *)src;
    uint8_t *d = (uint8_t *)dst;

    /* Long-aligned fast path — both divisible by 4. */
    if (n >= 4 && ((((uintptr_t)s | (uintptr_t)d) & 3) == 0)) {
        const uint32_t *ls = (const uint32_t *)s;
        uint32_t *ld = (uint32_t *)d;
        uint32_t longs = n >> 2;
        while (longs--) *ld++ = *ls++;
        s = (const uint8_t *)ls;
        d = (uint8_t *)ld;
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

    {
        int timeout = 10000000;
        while (timeout-- > 0) {
            if (result->status != ZZUSB_STATUS_PENDING) break;
            delay = 1000;
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

    if ((cd = (struct ConfigDev*)FindConfigDev(cd, 0x6d6e, 0x3))) {
        registers = ((uint8_t*)cd->cd_BoardAddr);
    } else if ((cd = (struct ConfigDev*)FindConfigDev(cd, 0x6d6e, 0x4))) {
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
    unit->zz_RootHubAddr = 0;
    unit->zz_PortChange = 0;
    unit->zz_PortStatus = UPSF_PORT_POWER;
    unit->zz_Speed = 0;
    unit->zz_PendingIntXfer = NULL;
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
    } else if (unit->zz_PortPresent) {
        /*
         * Port transitioned occupied -> empty (hot unplug).
         * Flag C_PORT_CONNECTION so Poseidon's hub class sees
         * the disconnect on its next hub-status read, and
         * sweep any queued downstream interrupt IORs. This is
         * a safety net: poll_int_pending also maps firmware
         * OFFLINE on an outstanding transfer, but we may well
         * detect disconnect here first.
         */
        unit->zz_PortPresent = FALSE;
        unit->zz_Speed = 0;
        unit->zz_PortStatus = UPSF_PORT_POWER;
        unit->zz_PortChange = UPSF_C_PORT_CONNECTION;
        abort_int_iors_offline(unit, aborted, aborted_count, aborted_max);
        complete_pending_intxfer(unit, base);
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
         * send_usb_cmd writes to the shared Zorro command buffer
         * (base + 0xa000) and the firmware writes back into the
         * same region. begin_io uses the same buffer for control
         * and bulk transfers. Without serialization, begin_io and
         * the poll task race the buffer and corrupt each other.
         *
         * ReplyMsg is done AFTER the lock is released so we never
         * call ReplyMsg while holding the semaphore (avoids any
         * risk of deadlock if the reply wakes a task that tries
         * to re-enter our driver).
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

        uint16_t status = send_usb_cmd(base, &cmd,
                                       (ior->iouh_Dir == UHDIR_OUT) ? ior->iouh_Data : NULL,
                                       (ior->iouh_Dir == UHDIR_OUT) ? ior->iouh_Length : 0);

        if (status == ZZUSB_STATUS_OK) {
            volatile struct ZZUSBCommand *result =
                (volatile struct ZZUSBCommand*)(base + 0xa000);
            uint32_t actual = result->actual_length;

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
            /* actual == 0: leave IOR queued. */
        } else if (status != ZZUSB_STATUS_TIMEOUT &&
                   status != ZZUSB_STATUS_NAK) {
            /* Hard error — fail the IOR. */
            ior->iouh_Actual = 0;
            switch (status) {
            case ZZUSB_STATUS_STALL:
                ior->iouh_Req.io_Error = UHIOERR_STALL; break;
            case ZZUSB_STATUS_OFFLINE:
                ior->iouh_Req.io_Error = UHIOERR_USBOFFLINE; break;
            case ZZUSB_STATUS_OVERRUN:
            case ZZUSB_STATUS_BABBLE:
                ior->iouh_Req.io_Error = UHIOERR_OVERFLOW; break;
            case ZZUSB_STATUS_CRC:
                ior->iouh_Req.io_Error = UHIOERR_CRCERROR; break;
            default:
                ior->iouh_Req.io_Error = UHIOERR_HOSTERROR; break;
            }
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
     * Async interrupt-delivery loop — the Deneb-equivalent poll
     * task that lets Poseidon's HID class see only genuine
     * device events instead of a synthetic "valid 0-byte
     * report" every 16 ms.
     *
     * Protocol:
     *   - begin_io UHCMD_INTXFER stashes the IOR in
     *     unit->zz_IntPending[endpoint] and does NOT ReplyMsg.
     *     Signal() wakes us up.
     *   - poll_int_pending issues one firmware poll per stashed
     *     IOR. Real data (actual > 0) -> fill iouh_Data, set
     *     iouh_Actual, ReplyMsg. NAK -> leave IOR queued and
     *     try again. Hard error -> fail with the mapped error.
     *   - When no IORs remain queued we Wait() for the next
     *     Signal from begin_io. Zero CPU when idle.
     *
     * AllocSignal gives us a private bit; the mask is published
     * into PollBase->zz_PollSignal BEFORE the first Wait so any
     * begin_io call that races task startup reads a valid mask.
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

        /* No outstanding IORs — sleep until begin_io wakes us
         * with a new one. If more IORs remain (e.g. NAK), we
         * loop immediately. send_usb_cmd's 16 ms firmware-side
         * poll provides the natural pacing in that case. */
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
                    static const uint8_t mfr_desc[] = {
                        10, 0x03,
                        'M', 0, 'N', 0, 'T', 0
                    };
                    uint16_t len = (ior->iouh_Length < mfr_desc[0]) ? ior->iouh_Length : mfr_desc[0];
                    if (ior->iouh_Data) {
                        safe_copy((void*)mfr_desc, ior->iouh_Data, len);
                    }
                    ior->iouh_Actual = len;
                    ior->iouh_Req.io_Error = 0;
                    return;
                } else if (string_index == 2) {
                    static const uint8_t prod_desc[] = {
                        24, 0x03,
                        'Z', 0, 'Z', 0, '9', 0, '0', 0, '0', 0,
                        '0', 0, ' ', 0, 'U', 0, 'S', 0, 'B', 0
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
    Forbid();
    if (!ZZBase->zz_PollTask) {
        struct Task *poll = &ZZBase->zz_PollTaskStorage;

        /* Clear the full Task struct — AddTask expects ln_Succ /
         * ln_Pred / tc_State and friends to be zero before it
         * inserts the task onto the ready list. memset covers
         * every field; we then set the specific ones we care
         * about. */
        uint8_t *tp = (uint8_t*)poll;
        for (unsigned i = 0; i < sizeof(struct Task); i++) tp[i] = 0;

        poll->tc_Node.ln_Type = NT_TASK;
        poll->tc_Node.ln_Pri = -1;           /* below Poseidon IO tasks */
        poll->tc_Node.ln_Name = (char *)"zzusbhw.poll";

        /* Stack bounds. tc_SPReg is the initial SP; m68k stacks
         * grow downward, so it must point one past the top of
         * the reserved region. */
        poll->tc_SPLower = (APTR)&ZZBase->zz_PollStack[0];
        poll->tc_SPUpper = (APTR)&ZZBase->zz_PollStack[1024];
        poll->tc_SPReg   = (APTR)&ZZBase->zz_PollStack[1024];

        /* tc_MemEntry must be a valid (empty) list. Inline
         * NewList to avoid pulling in amiga.lib. */
        poll->tc_MemEntry.lh_Head     = (struct Node*)&poll->tc_MemEntry.lh_Tail;
        poll->tc_MemEntry.lh_Tail     = NULL;
        poll->tc_MemEntry.lh_TailPred = (struct Node*)&poll->tc_MemEntry.lh_Head;
        poll->tc_MemEntry.lh_Type     = NT_MEMORY;

        PollBase = ZZBase;
        ZZBase->zz_PollTask = poll;     /* published before AddTask so
                                           the task, once running, sees
                                           itself registered */
        AddTask(poll, (APTR)hotplug_poll_task, NULL);
    }
    Permit();

    volatile uint8_t* base = (volatile uint8_t*)unit->zz_Registers;

    ObtainSemaphore(&ZZBase->zz_Lock);

    switch (ior->iouh_Req.io_Command) {
    case UHCMD_QUERYDEVICE:
        {
            struct TagItem* tags = (struct TagItem*)ior->iouh_Data;
            while (tags && tags->ti_Tag != TAG_DONE) {
                switch (tags->ti_Tag) {
                case UHA_DriverVersion: tags->ti_Data = 0x200; break;
                case UHA_Version: tags->ti_Data = DEVICE_VERSION; break;
                case UHA_Revision: tags->ti_Data = DEVICE_REVISION; break;
                case UHA_State: tags->ti_Data = UHSF_OPERATIONAL; break;
                case UHA_Manufacturer: tags->ti_Data = (uint32_t)"MNT Research GmbH"; break;
                case UHA_ProductName: tags->ti_Data = (uint32_t)"ZZ9000 USB (EHCI)"; break;
                case UHA_Description: tags->ti_Data = (uint32_t)"ZZ9000 Poseidon USB Hardware Driver"; break;
                case UHA_Copyright: tags->ti_Data = (uint32_t)"Copyright (C) 2026 MNT Research GmbH"; break;
                default: break;
                }
                tags++;
            }
            ior->iouh_Actual = sizeof(struct TagItem);
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
                    switch (status) {
                    case ZZUSB_STATUS_STALL:
                        ior->iouh_Req.io_Error = UHIOERR_STALL; break;
                    case ZZUSB_STATUS_TIMEOUT:
                    case ZZUSB_STATUS_NAK:
                        ior->iouh_Req.io_Error = UHIOERR_TIMEOUT; break;
                    case ZZUSB_STATUS_OFFLINE:
                        ior->iouh_Req.io_Error = UHIOERR_USBOFFLINE; break;
                    case ZZUSB_STATUS_OVERRUN:
                    case ZZUSB_STATUS_BABBLE:
                        ior->iouh_Req.io_Error = UHIOERR_OVERFLOW; break;
                    case ZZUSB_STATUS_CRC:
                        ior->iouh_Req.io_Error = UHIOERR_CRCERROR; break;
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
                 * Stash the IOR in the per-endpoint pending slot,
                 * signal the poll task, and defer the ReplyMsg. The
                 * task replies this IOR only when the device
                 * actually produces data — matching Deneb's
                 * behaviour and avoiding the stale-report-replay
                 * bugs of sync delivery (cursor slide, stuck
                 * clicks, qualifier pollution).
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

                /*
                 * If Poseidon re-queued on the same endpoint before
                 * we replied to the last one, abort the old one
                 * so Poseidon's HID class doesn't leak msgs. The
                 * old IOR is replied after we release zz_Lock.
                 */
                if (unit->zz_IntPending[ep]) {
                    deferred_old_ior = unit->zz_IntPending[ep];
                    deferred_old_ior->iouh_Actual = 0;
                    deferred_old_ior->iouh_Req.io_Error = IOERR_ABORTED;
                    unit->zz_IntPending[ep] = NULL;
                }

                unit->zz_IntPending[ep] = ior;
                deferred = 1;      /* do NOT ReplyMsg at bottom of begin_io */

                /*
                 * Wake the poll task. If zz_PollSignal is 0, the
                 * task hasn't yet called AllocSignal — skip the
                 * Signal. It'll pick up the IOR on its next loop
                 * iteration anyway (it always scans all units
                 * before Wait()ing).
                 */
                if (ZZBase->zz_PollTask && ZZBase->zz_PollSignal) {
                    Signal(ZZBase->zz_PollTask, ZZBase->zz_PollSignal);
                }
            }
        }
        break;

    case UHCMD_BULKXFER:
        {
            struct ZZUSBCommand cmd;
            uint16_t status;

            if (ior->iouh_Length > ZZUSB_MAX_XFER) {
                ior->iouh_Req.io_Error = UHIOERR_PKTTOOLARGE;
                break;
            }

            memset(&cmd, 0, sizeof(cmd));
            cmd.cmd = ZZUSB_CMD_BULK_XFER;
            cmd.dev_addr = ior->iouh_DevAddr;
            cmd.endpoint = ior->iouh_Endpoint;
            cmd.direction = (ior->iouh_Dir == UHDIR_IN) ? 0x80 : 0x00;
            cmd.max_pkt_size = ior->iouh_MaxPktSize;
            cmd.speed = unit->zz_Speed;
            cmd.data_length = ior->iouh_Length;
            cmd.timeout_ms = (ior->iouh_Flags & UHFF_NAKTIMEOUT)
                             ? (ior->iouh_NakTimeout ? ior->iouh_NakTimeout : 5000)
                             : 0;

            status = send_usb_cmd(base, &cmd,
                                  (ior->iouh_Dir == UHDIR_OUT) ? ior->iouh_Data : NULL,
                                  (ior->iouh_Dir == UHDIR_OUT) ? ior->iouh_Length : 0);

            if (status == ZZUSB_STATUS_OK) {
                volatile struct ZZUSBCommand *result =
                    (volatile struct ZZUSBCommand*)(base + 0xa000);
                uint32_t actual = result->actual_length;

                if (ior->iouh_Dir == UHDIR_IN && ior->iouh_Data && actual > 0) {
                    /* safe_copy — see note in UHCMD_CONTROLXFER. */
                    safe_copy((void*)(base + 0xa000 + ZZUSB_DATA_OFFSET),
                              ior->iouh_Data, actual);
                }
                ior->iouh_Actual = actual;
                if (actual < ior->iouh_Length &&
                    !(ior->iouh_Flags & UHFF_ALLOWRUNTPKTS) &&
                    ior->iouh_Dir == UHDIR_IN) {
                    ior->iouh_Req.io_Error = UHIOERR_RUNTPACKET;
                } else {
                    ior->iouh_Req.io_Error = 0;
                }
            } else {
                switch (status) {
                case ZZUSB_STATUS_STALL:
                    ior->iouh_Req.io_Error = UHIOERR_STALL; break;
                case ZZUSB_STATUS_TIMEOUT:
                case ZZUSB_STATUS_NAK:
                    ior->iouh_Req.io_Error = UHIOERR_TIMEOUT; break;
                case ZZUSB_STATUS_OFFLINE:
                    ior->iouh_Req.io_Error = UHIOERR_USBOFFLINE; break;
                case ZZUSB_STATUS_OVERRUN:
                case ZZUSB_STATUS_BABBLE:
                    ior->iouh_Req.io_Error = UHIOERR_OVERFLOW; break;
                case ZZUSB_STATUS_CRC:
                    ior->iouh_Req.io_Error = UHIOERR_CRCERROR; break;
                default:
                    ior->iouh_Req.io_Error = UHIOERR_HOSTERROR; break;
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
