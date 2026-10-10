/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "rx.h"

void zznet_rx_reset(struct zznet_rx_state *state, uint16_t fw_version)
{
	state->old_serial = 0;
	state->have_baseline = 0;
	state->ready_valid = fw_version >= ZZNET_RX_STATUS_MIN_FW;
}

uint32_t zznet_rx_header_split(uint16_t (*read16)(void *ctx, unsigned off),
                               void *ctx)
{
	uint16_t serial = read16(ctx, 2);

	if (serial == 0)
		return 0;
	return ((uint32_t)read16(ctx, 0) << 16) | serial;
}

struct zznet_rx_decision zznet_rx_next(struct zznet_rx_state *state,
                                       const struct zznet_rx_io *io)
{
	struct zznet_rx_decision d = { ZZNET_RX_WAIT, 0, 0, 0, 0, 0 };
	uint32_t header = io->read_header(io->ctx);

	d.size = (uint16_t)(header >> 16);
	d.serial = (uint16_t)header;

	/* issue #29: an all-zero header is an empty slot, not a frame. Acking it
	 * would race a frame landing in the same slot and consume it unread.
	 * old_serial stays put so gap detection spans the drain boundary. */
	if (d.size == 0 && d.serial == 0) {
		d.empty = 1;
		return d;
	}

	/* The ack of old_serial completed before this read, so firmware already
	 * consumed that frame. A header still carrying old_serial is either a
	 * stale slot on firmware that never clears slots, or a new frame after
	 * an RX DMA restart reset the counter (issue #127); treating the latter
	 * as old never acks it while firmware re-raises the IRQ forever. Only
	 * the ready count tells them apart, and firmware before 2.1 has no
	 * restarts and no ready count. The re-read rejects a frame that replaced
	 * a stale slot between the two reads; its IRQ wakes the framer again.
	 * Serial 0 never starts a post-restart sequence (2.1/2.2 restart at 1,
	 * later firmware at 2) and handshake firmware rejects an ack of 0, so
	 * delivering a repeated serial 0 would loop on a corrupt slot. */
	if (d.serial == state->old_serial &&
	    (d.serial == 0 || !state->ready_valid ||
	     !(io->rx_status(io->ctx) & ZZNET_ETH_RX_STATUS_READY) ||
	     io->read_header(io->ctx) != header))
		return d;

	/* Torn reads, cold-boot 0xFFFF and corrupt slots: release the slot
	 * without completing a client read. */
	if (d.size < ZZNET_RX_MIN_FRAME || d.size > ZZNET_RX_MAX_FRAME) {
		d.action = ZZNET_RX_DROP;
		d.bad_data = 1;
		state->have_baseline = 1;
		state->old_serial = d.serial;
		return d;
	}

	/* Gap detection: a reasonable gap is bounded by the backlog depth; a
	 * much larger delta is an artifact (torn header, firmware restart), so
	 * it counts as BadData rather than Overruns. Firmware skips serials 0
	 * and 1 on its u16 wrap, so a clean 0xffff -> 2 step has a raw delta
	 * of 3; discount the two sentinels when the serial wrapped. */
	if (state->have_baseline) {
		uint16_t delta = (uint16_t)(d.serial - state->old_serial);

		if (d.serial < state->old_serial)
			delta = (uint16_t)(delta - 2);
		if (delta > 1 && delta <= 128)
			d.overruns = (uint16_t)(delta - 1);
		else if (delta > 128)
			d.bad_data = 1;
	}
	state->have_baseline = 1;
	state->old_serial = d.serial;
	d.action = ZZNET_RX_DELIVER;
	return d;
}
