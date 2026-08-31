/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * ZZ9000 Poseidon USB Hardware Driver
 *
 * Copyright (C) 2026 Dimitris Panokostas <midwan@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * This driver implements the Poseidon usbhardware.device interface,
 * allowing the Poseidon USB stack to use the ZZ9000's USB port for
 * all USB device types (HID, storage, networking, etc.).
 *
 * Architecture:
 *   The ZZ9000's USB controller (Xilinx Zynq PS7 EHCI) runs on the
 *   ARM core. The m68k Amiga side communicates with it via a
 *   register-based command protocol through Zorro address space.
 *
 *   m68k Poseidon driver <--registers--> ARM firmware <--> EHCI <--> USB devices
 *
 *   The ARM firmware already has a full EHCI/USB stack. This driver
 *   sends USB operation requests through a command mailbox protocol,
 *   and the ARM firmware executes them and returns results via
 *   shared memory.
 */

#ifndef ZZUSBHW_H
#define ZZUSBHW_H

#include <exec/types.h>
#include <exec/lists.h>
#include <exec/semaphores.h>
#include <devices/usbhardware.h>

#define DEVICE_NAME      "zzusbhw.device"
#define DEVICE_VERSION   2
#define DEVICE_REVISION  2

#define ZZ_NUM_PORTS     1
#define ZZUSB_UHCMD_GET_DIAGNOSTICS 0x7a00

/*
 * USB Command Mailbox Protocol
 *
 * The m68k driver writes a command structure to the shared buffer
 * at card_base + 0xa000, then triggers the ARM firmware via a
 * register write. The ARM firmware processes the command and
 * writes the response back to the same buffer.
 *
 * This protocol requires matching ARM firmware support in
 * zz9000-firmware/ZZ9000_proto.sdk/ZZ9000OS/src/main.c
 */

/* Command types */
#define ZZUSB_CMD_CONTROL_XFER  0x01
#define ZZUSB_CMD_BULK_XFER     0x02
#define ZZUSB_CMD_INT_XFER      0x03
#define ZZUSB_CMD_ISO_XFER      0x04
#define ZZUSB_CMD_RESET_PORT    0x05
#define ZZUSB_CMD_RESUME_PORT   0x06
#define ZZUSB_CMD_SUSPEND_PORT  0x07
#define ZZUSB_CMD_ENUMERATE     0x08
#define ZZUSB_CMD_QUERY_DEVICE  0x09
#define ZZUSB_CMD_SET_ADDRESS   0x0A
#define ZZUSB_CMD_CLEAR_STALL   0x0B
#define ZZUSB_CMD_CHECK_PORT    0x0C
#define ZZUSB_CMD_QUERY_CAPS     0x0D
#define ZZUSB_CMD_RETIRE_EP      0x0E
#define ZZUSB_CMD_CANCEL_EP      0x0F
#define ZZUSB_CMD_DIAG_SNAPSHOT  0x10
#define ZZUSB_CMD_PERIODIC_ARM    0x11
#define ZZUSB_CMD_PERIODIC_REAP   0x12
#define ZZUSB_CMD_PERIODIC_STOP   0x13
#define ZZUSB_CMD_ISO_QUEUE       0x14
#define ZZUSB_CMD_ISO_REAP        0x15
#define ZZUSB_CMD_ISO_STOP        0x16

/* Command status codes (returned by ARM firmware) */
#define ZZUSB_STATUS_OK         0x00
#define ZZUSB_STATUS_PENDING    0x01
#define ZZUSB_STATUS_ERROR      0xFF
#define ZZUSB_STATUS_TIMEOUT    0xFE
#define ZZUSB_STATUS_STALL      0xFD
#define ZZUSB_STATUS_NAK        0xFC
#define ZZUSB_STATUS_CRC        0xFB
#define ZZUSB_STATUS_BABBLE     0xFA
#define ZZUSB_STATUS_OVERRUN    0xF9
#define ZZUSB_STATUS_UNDERRUN   0xF8
#define ZZUSB_STATUS_OFFLINE    0xF7
#define ZZUSB_STATUS_BADPARAM   0xF6
#define ZZUSB_STATUS_UNSUPPORTED 0xF5
#define ZZUSB_STATUS_STALE       0xF4
#define ZZUSB_STATUS_CANCELLED   0xF3
#define ZZUSB_STATUS_HOSTERROR   0xF2
#define ZZUSB_STATUS_BUSY        0xF1
#define ZZUSB_STATUS_NOMEM       0xF0

/* USB speed types */
#define ZZUSB_SPEED_LOW        0
#define ZZUSB_SPEED_FULL       1
#define ZZUSB_SPEED_HIGH       2

#define ZZUSB_FLAG_SPLIT          0x0001
#define ZZUSB_FLAG_RESET_FSLS     0x0002
#define ZZUSB_FLAG_MULTI_TT       0x0004
#define ZZUSB_FLAG_TT_THINK_SHIFT 4
#define ZZUSB_FLAG_TT_THINK_MASK  0x0030

#define ZZUSB_XFER_CONTROL        0
#define ZZUSB_XFER_BULK           1
#define ZZUSB_XFER_INTERRUPT      2
#define ZZUSB_XFER_ISO            3

/*
 * Command structure layout in shared buffer.
 * Written at card_base + 0xa000 via Zorro II bus.
 *
 * Most fields are big-endian (m68k native), read by ARM with be16()/be32().
 * Setup packet fields (wValue, wIndex, wLength) are in USB little-endian,
 * read by ARM with le16().
 * uint8_t fields need no conversion.
 */
struct ZZUSBCommand {
    uint16_t cmd;           /* ZZUSB_CMD_* */
    uint16_t status;        /* ZZUSB_STATUS_* (written by ARM on completion) */
    uint32_t dev_addr;      /* USB device address (0-127) */
    uint16_t endpoint;      /* endpoint number (0-15) */
    uint16_t direction;     /* 0=OUT, 0x80=IN */
    uint16_t xfer_type;     /* control/bulk/int/iso */
    uint16_t max_pkt_size;  /* max packet size for endpoint */
    uint32_t data_length;   /* total transfer length */
    uint32_t actual_length; /* actual bytes transferred (written by ARM) */
    uint32_t timeout_ms;    /* timeout in milliseconds */
    uint16_t speed;         /* device speed */
    uint16_t interval;      /* interrupt interval */
    /* Setup data for control transfers (8 bytes) */
    uint8_t  setup_bRequestType;
    uint8_t  setup_bRequest;
    uint16_t setup_wValue;
    uint16_t setup_wIndex;
    uint16_t setup_wLength;
    uint16_t split_hub_addr; /* HS hub address for FS/LS split transactions */
    uint16_t split_hub_port; /* downstream hub port for split transactions */
    uint16_t flags;          /* ZZUSB_FLAG_* */
    uint16_t reserved;
    /* Data follows at ZZUSB_DATA_OFFSET. */
} __attribute__((packed));

struct ZZUSBProtocolExtension {
    uint16_t version;
    uint16_t header_size;
    uint32_t request_id;
    uint32_t controller_epoch;
    uint32_t capabilities;
} __attribute__((packed));

#define ZZUSB_PROTOCOL_VERSION 2
#define ZZUSB_CMD_SIZE         48
#define ZZUSB_V2_HEADER_SIZE   64
#define ZZUSB_DATA_OFFSET      64
#define ZZUSB_APERTURE_SIZE    24576
#define ZZUSB_MAX_XFER         (ZZUSB_APERTURE_SIZE - ZZUSB_DATA_OFFSET)
#define ZZUSB_PROXY_MAX_TIMEOUT_MS 1000
#define ZZUSB_V2_DATA_MAX      16384
#define ZZUSB_DIAG_SIZE        4096
#define ZZUSB_DIAG_OFFSET      (ZZUSB_APERTURE_SIZE - ZZUSB_DIAG_SIZE)
#define ZZUSB_DIAG_PAGE_SIZE    4096
#define ZZUSB_DIAG_MAGIC             0x5a554447UL
#define ZZUSB_DIAG_VERSION           1
#define ZZUSB_DIAG_EVENT_COUNT       64
#define ZZUSB_DIAG_EVENT_SIZE        32
#define ZZUSB_DIAG_COUNTER_COUNT     16
#define ZZUSB_DIAG_OFF_MAGIC         0
#define ZZUSB_DIAG_OFF_GENERATION    4
#define ZZUSB_DIAG_OFF_VERSION       8
#define ZZUSB_DIAG_OFF_HEADER_SIZE   10
#define ZZUSB_DIAG_OFF_TOTAL_SIZE    12
#define ZZUSB_DIAG_OFF_CAPABILITIES  16
#define ZZUSB_DIAG_OFF_EPOCH         20
#define ZZUSB_DIAG_OFF_LAST_ID       24
#define ZZUSB_DIAG_OFF_EVENT_NEXT    28
#define ZZUSB_DIAG_OFF_EVENT_COUNT   32
#define ZZUSB_DIAG_OFF_LOST_EVENTS   36
#define ZZUSB_DIAG_OFF_QUEUE_STATE   40
#define ZZUSB_DIAG_OFF_SCHEDULE_BITS 44
#define ZZUSB_DIAG_OFF_COUNTERS      48
#define ZZUSB_DIAG_OFF_EVENTS        128
#define ZZUSB_DIAG_EVT_OFF_SEQUENCE  0
#define ZZUSB_DIAG_EVT_OFF_REQUEST   4
#define ZZUSB_DIAG_EVT_OFF_EPOCH     8
#define ZZUSB_DIAG_EVT_OFF_DETAIL    12
#define ZZUSB_DIAG_EVT_OFF_TIMESTAMP 16
#define ZZUSB_DIAG_EVT_OFF_TYPE      20
#define ZZUSB_DIAG_EVT_OFF_STATUS    22
#define ZZUSB_DIAG_EVT_OFF_ADDRESS   24
#define ZZUSB_DIAG_EVT_OFF_TOPOLOGY  26
#define ZZUSB_DIAG_EVT_OFF_ENDPOINT  28
#define ZZUSB_DIAG_EVT_OFF_DIRECTION 29
#define ZZUSB_DIAG_EVT_OFF_SCHEDULE  30
#define ZZUSB_DOORBELL_V2      0x8000

#define ZZUSB_CAP_PROTOCOL_V2      (1UL << 0)
#define ZZUSB_CAP_REQUEST_ID       (1UL << 1)
#define ZZUSB_CAP_CONTROLLER_EPOCH (1UL << 2)
#define ZZUSB_CAP_VALIDATION       (1UL << 3)
#define ZZUSB_CAP_DIAGNOSTICS      (1UL << 4)
#define ZZUSB_CAP_PERIODIC         (1UL << 5)
#define ZZUSB_CAP_ISO_SIMPLE       (1UL << 6)
#define ZZUSB_CAP_ISO_REALTIME     (1UL << 7)
#define ZZUSB_CAP_EVENT_IRQ        (1UL << 8)
#define ZZUSB_CAP_PRECISE_ERRORS   (1UL << 9)
#define ZZUSB_CAP_BASE (ZZUSB_CAP_PROTOCOL_V2 | ZZUSB_CAP_REQUEST_ID | \
                        ZZUSB_CAP_CONTROLLER_EPOCH | ZZUSB_CAP_VALIDATION | \
                        ZZUSB_CAP_DIAGNOSTICS | ZZUSB_CAP_PERIODIC | \
                        ZZUSB_CAP_ISO_SIMPLE | ZZUSB_CAP_ISO_REALTIME | \
                        ZZUSB_CAP_EVENT_IRQ | ZZUSB_CAP_PRECISE_ERRORS)

typedef char ZZUSBCommand_size_must_match_protocol[
    (sizeof(struct ZZUSBCommand) == ZZUSB_CMD_SIZE) ? 1 : -1];
typedef char ZZUSBProtocolExtension_size_must_fill_gap[
    (sizeof(struct ZZUSBProtocolExtension) ==
     (ZZUSB_DATA_OFFSET - ZZUSB_CMD_SIZE)) ? 1 : -1];

#define ZZ_REG_USB_PROXY_CMD    0xDE

/*
 * Device base structure (extends struct Device/library)
 */
struct ZZUSBBase {
    struct Device      zz_Device;
    struct SignalSemaphore zz_Lock;
    struct Task       *zz_PollTask;
    ULONG              zz_PollSignal;       /* signal-mask the poll task waits on */
    struct Task        zz_PollTaskStorage;
    ULONG              zz_PollStack[1024];  /* match v2.0.0 layout exactly */
    struct ZZUSBUnit {
        struct Unit    zz_Unit;
        void*          zz_Registers;
        BOOL           zz_Enabled;
        BOOL           zz_PortPresent;
        BOOL           zz_PortDead;    /* device marked unusable after
                                        * unrecoverable error (babble,
                                        * stuck qTD). Sticky until
                                        * physical disconnect — prevents
                                        * infinite re-enumerate loops on
                                        * a device that keeps failing. */
        UWORD          zz_RootHubAddr;
        UWORD          zz_PortChange;
        UWORD          zz_PortStatus;
        UWORD          zz_Speed;
        /*
         * Reserved legacy async interrupt slots. The actual pending
         * table is driver-static and keyed by device address, endpoint,
         * and direction; this field stays here to preserve the frozen
         * device-base layout.
         */
        struct IOUsbHWReq *zz_IntPending[16];
        /*
         * Consecutive bulk-failure counter. Incremented when a
         * UHCMD_BULKXFER comes back non-OK from firmware; reset to
         * 0 on any successful bulk. If it crosses the threshold we
         * report UHIOERR_USBOFFLINE instead of TIMEOUT so Poseidon
         * tears the device down instead of looping forever. A
         * genuinely incompatible USB stick would otherwise keep
         * Poseidon retrying indefinitely, accumulating state until
         * something in the class driver shreds.
         */
        UWORD          zz_BulkErrCount;
    } zz_Units[ZZ_NUM_PORTS];
};

#endif /* ZZUSBHW_H */
