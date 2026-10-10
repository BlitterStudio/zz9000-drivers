/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Asynchronous TX slot accounting for the ZZ9000 TX window. No Amiga
 * headers: device.c reads the status register and performs the kick, this
 * model decides which slot is free and when a slot may be reused.
 *
 * Firmware contract (zz9000-firmware, ETH_TX_ASYNC):
 *   ETH_TX write, bit 15 set    asynchronous send of the frame in slot
 *                               bits 12..11 (2 KB each), length bits 10..0;
 *                               the bus cycle returns once the frame is
 *                               queued, not sent
 *   ETH_TX_STATUS (0x68) read   bit 15 = the firmware has the path (older
 *                               firmware reads 0); bits 14..0 = asynchronous
 *                               submissions retired IN ORDER, sent or
 *                               refused. A refused submission is counted
 *                               only after every earlier one, and a DMA
 *                               restart retires everything outstanding, so
 *                               the count is the number of oldest slots the
 *                               driver may reuse.
 *   ETH_TX write, bit 15 clear  the synchronous send, not counted.
 */
#ifndef ZZNET_TX_H
#define ZZNET_TX_H

#include <stdint.h>

#define ZZNET_TX_ASYNC          0x8000
#define ZZNET_TX_SLOT_SHIFT     11
#define ZZNET_TX_LEN_MASK       0x07ff
#define ZZNET_TX_SLOTS          4
#define ZZNET_TX_SLOT_SIZE      2048
#define ZZNET_TX_STATUS_PRESENT 0x8000
#define ZZNET_TX_STATUS_COUNT   0x7fff

struct zznet_tx_state {
	uint16_t done;   /* retired count last read from ETH_TX_STATUS */
	uint8_t  next;   /* submissions so far, modulo 256; slot = next & 3 */
	uint8_t  inuse;  /* submitted and not yet retired */
};

/* status is the ETH_TX_STATUS value; returns 1 when the firmware has the
 * asynchronous path. Only valid with no submission outstanding. */
int zznet_tx_reset(struct zznet_tx_state *state, uint16_t status);

/* Retire what the status count says finished. Returns the slots freed. A
 * count that ran further than what is outstanding (firmware restarted
 * underneath us) frees everything, never more. */
unsigned zznet_tx_reclaim(struct zznet_tx_state *state, uint16_t status);

/* The slot the next submission must use, or -1 while all four are owned
 * by the firmware. */
int zznet_tx_slot(const struct zznet_tx_state *state);

/* The ETH_TX word that submits `len` bytes from `slot`. */
uint16_t zznet_tx_word(int slot, uint16_t len);

/* Record a submission written with zznet_tx_word(zznet_tx_slot(), ...). */
void zznet_tx_submitted(struct zznet_tx_state *state);

#endif /* ZZNET_TX_H */
