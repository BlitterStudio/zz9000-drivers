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

/* Wire-level size bounds; device.c checks them against device.h. */
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
	uint16_t size;
	uint16_t serial;     /* ack value for DROP and DELIVER */
};

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

#endif /* ZZNET_RX_H */
