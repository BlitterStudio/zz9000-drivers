/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * RX backlog slot classification for the ZZ9000 framer. No Amiga headers:
 * frame_proc reads the presented slot header, asks this model what to do,
 * and performs the MMIO (IRQ re-arm or serial ack) itself.
 *
 * Firmware contract (presented slot header, big-endian):
 *   size:16 serial:16
 *   size == 0 && serial == 0  empty, firmware-cleared slot
 *   serial                    per-frame counter; reset to 0 by every
 *                             firmware DMA restart (Amiga reset, MAC update,
 *                             TX-timeout recovery), so it is not monotonic
 *                             across a restart.
 */
#ifndef ZZNET_RX_H
#define ZZNET_RX_H

#include <stdint.h>

/* Wire-level size bounds; device.c checks them against device.h. */
#define ZZNET_RX_MIN_FRAME 14    /* full Ethernet header, empty payload */
#define ZZNET_RX_MAX_FRAME 1518  /* 802.1Q-tagged frame without FCS */

enum zznet_rx_action {
	ZZNET_RX_WAIT,    /* nothing new: re-arm the IRQ and sleep, no ack */
	ZZNET_RX_DROP,    /* bad hardware size: ack the serial, deliver nothing */
	ZZNET_RX_DELIVER  /* new frame: route it, then ack the serial */
};

struct zznet_rx_state {
	uint16_t old_serial;    /* serial of the last consumed header */
	uint8_t  have_baseline; /* old_serial is valid for gap detection */
	uint8_t  saw_empty;     /* an empty slot was read since that consume */
};

struct zznet_rx_decision {
	uint8_t  action;     /* enum zznet_rx_action */
	uint8_t  empty;      /* 1: the slot was empty (RxEmptySlot) */
	uint8_t  bad_data;   /* BadData increment, 0 or 1 */
	uint16_t overruns;   /* frames the serial gap says were missed */
};

void zznet_rx_reset(struct zznet_rx_state *state);
struct zznet_rx_decision zznet_rx_classify(struct zznet_rx_state *state,
                                           uint16_t size, uint16_t serial);

#endif /* ZZNET_RX_H */
