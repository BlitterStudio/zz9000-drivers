/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * RX backlog slot classification for the ZZ9000 framer. No Amiga headers:
 * frame_proc supplies register reads, this model decides, and frame_proc
 * performs the IRQ re-arm or serial ack itself.
 *
 * Firmware contract:
 *   presented slot header   size:16 serial:16, big-endian
 *     size == 0 && serial == 0  empty, firmware-cleared slot
 *     serial                    per-frame counter; firmware 2.1+ resets it
 *                               on every RX DMA restart (Amiga reset, MAC
 *                               update, TX-timeout recovery)
 *   ETH_RX_STATUS bits 7:0  frames waiting on firmware 2.1+; older firmware
 *                           maps that register to a debug counter, so the
 *                           model ignores it below ZZNET_RX_STATUS_MIN_FW
 *   FW_VERSION              major << 8 | minor
 *   RX accept write         acknowledged on the bus only after firmware
 *                           consumed or rejected the named frame
 */
#ifndef ZZNET_RX_H
#define ZZNET_RX_H

#include <stdint.h>

#define ZZNET_ETH_RX_STATUS_READY 0x00ff
#define ZZNET_RX_STATUS_MIN_FW    0x0201
/* Firmware from 2.8 skips serials 0 and 1 on the u16 wrap; older firmware
 * hands serial 0 to a real frame after 0xffff. */
#define ZZNET_RX_SERIAL_SKIP_MIN_FW 0x0208

/* Wire-level size bounds; device.c checks them against device.h. */
/* Length-word flag: the frame starts 2 bytes further into the slot, so the
 * payload behind the 14-byte Ethernet header is longword aligned. Only on
 * firmware that reports ZZNET_ETH_CONFIG_CAP_RX_OFFSET2 and was asked for
 * it (ZZNET_ETH_CONFIG_RX_OFFSET2). */
#define ZZNET_RX_LEN_OFFSET2 0x8000

#define ZZNET_RX_MIN_FRAME 14    /* full Ethernet header, empty payload */
#define ZZNET_RX_MAX_FRAME 1518  /* 802.1Q-tagged frame without FCS */

enum zznet_rx_action {
	ZZNET_RX_WAIT,    /* nothing new: re-arm the IRQ and sleep, no ack */
	ZZNET_RX_DROP,    /* bad hardware size: ack the serial, deliver nothing */
	ZZNET_RX_DELIVER  /* new frame: route it, then ack the serial */
};

/* read_header() returns the presented slot header; rx_status() returns the
 * ETH_RX_STATUS register. */
struct zznet_rx_io {
	uint32_t (*read_header)(void *ctx);
	uint16_t (*rx_status)(void *ctx);
	void *ctx;
};

struct zznet_rx_state {
	uint16_t old_serial;    /* serial of the last acked header */
	uint8_t  have_baseline; /* old_serial is valid for gap detection */
	uint8_t  ready_valid;   /* ETH_RX_STATUS carries the ready count */
};

struct zznet_rx_decision {
	uint8_t  action;     /* enum zznet_rx_action */
	uint8_t  empty;      /* 1: the slot was empty (RxEmptySlot) */
	uint8_t  bad_data;   /* BadData increment, 0 or 1 */
	uint16_t overruns;   /* frames the serial gap says were missed */
	uint16_t size;       /* without ZZNET_RX_LEN_OFFSET2 */
	uint16_t serial;     /* ack value for DROP and DELIVER */
	uint8_t  offset2;    /* the frame starts 2 bytes later in the slot */
};

/* The presented header from two 16-bit reads, for a bus that splits a
 * longword read (Zorro II: two word cycles, size first).  Size first can
 * read the size of the empty slot, then the serial of a frame published in
 * between: size 0 beside a fresh serial, which zznet_rx_next() drops as a
 * torn header, acking the frame unread.  The serial goes first instead, as
 * the publication marker: 0 is an empty slot and the size is not read; any
 * other serial was published together with its size.  read16(ctx, off)
 * reads the header word at off (0 size, 2 serial).
 *
 * Only valid where serial 0 never names a frame: see zznet_rx_serial_first().
 */
uint32_t zznet_rx_header_split(uint16_t (*read16)(void *ctx, unsigned off),
                               void *ctx);

/* 1 if zznet_rx_header_split() may be used with this FW_VERSION. Firmware
 * before 2.8 wraps its serial through 0, so a serial-first read would take
 * the frame after 0xffff for an empty slot, never ack it, and stall
 * reception. Those keep the size-first read, and with it the torn-header
 * race above. */
int zznet_rx_serial_first(uint16_t fw_version);

/* fw_version is the FW_VERSION register value. */
void zznet_rx_reset(struct zznet_rx_state *state, uint16_t fw_version);

/* Caller contract: every DROP or DELIVER is acked with its serial before
 * the next call.
 *
 * Known limit: firmware 2.1+ reuses the first serial after every restart.
 * If a restart and a new frame land while frame_proc is still copying a
 * frame with that same serial, the ack consumes the new frame unread. The
 * header cannot tell the two apart; only a firmware restart generation
 * could. */
struct zznet_rx_decision zznet_rx_next(struct zznet_rx_state *state,
                                       const struct zznet_rx_io *io);

/* Framer drain budget. frame_proc runs above the stack's tasks so the
 * backlog empties as frames arrive, but its drain loop only sleeps once
 * the backlog is empty. Under a flood faster than the 68k drains, it would
 * never sleep and starve every task below it. After ZZNET_RX_DRAIN_BUDGET
 * frames without sleeping, the framer drops to ZZNET_FRAMER_YIELD_PRI for
 * the next budget, where Exec time-slices it with ordinary tasks, then
 * returns to ZZNET_FRAMER_PRI; it is back at ZZNET_FRAMER_PRI whenever it
 * sleeps. The budget is half the 128-slot firmware backlog, above what a
 * TCP receive window keeps in flight, so normal delivery never yields. */
#define ZZNET_FRAMER_PRI        10
#define ZZNET_FRAMER_YIELD_PRI  0
#define ZZNET_RX_DRAIN_BUDGET   64
#define ZZNET_RX_PRI_KEEP       (-128)

struct zznet_rx_drain {
	uint16_t left;     /* frames before the next priority switch */
	uint8_t  lowered;  /* running at ZZNET_FRAMER_YIELD_PRI */
};

/* Start of a wakeup. Returns the priority to set before sleeping, or
 * ZZNET_RX_PRI_KEEP if the framer is already at ZZNET_FRAMER_PRI. */
int zznet_rx_drain_reset(struct zznet_rx_drain *drain);

/* Call once per DROP or DELIVER. Returns the priority to switch to now, or
 * ZZNET_RX_PRI_KEEP. */
int zznet_rx_drain_frame(struct zznet_rx_drain *drain);

#endif /* ZZNET_RX_H */
