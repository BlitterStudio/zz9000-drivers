/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "rx.h"

void zznet_rx_reset(struct zznet_rx_state *state)
{
	state->old_serial = 0;
	state->have_baseline = 0;
	state->saw_empty = 0;
}

struct zznet_rx_decision zznet_rx_classify(struct zznet_rx_state *state,
                                           uint16_t size, uint16_t serial)
{
	struct zznet_rx_decision d = { ZZNET_RX_WAIT, 0, 0, 0 };

	/* issue #29: an all-zero header is an empty slot, not a frame. Acking it
	 * would race a frame landing in the same slot and consume it unread.
	 * old_serial stays put so gap detection spans the drain boundary. */
	if (size == 0 && serial == 0) {
		state->saw_empty = 1;
		d.empty = 1;
		return d;
	}

	/* An unchanged serial means the slot still holds the frame we consumed
	 * last — unless an empty slot was read since. Firmware clears a slot
	 * before the read cursor leaves it, so a consumed frame never reappears
	 * after an empty read; a matching serial there is a new frame after a
	 * firmware DMA restart reset the counter (issue #127). Treating it as
	 * old would never ack it while firmware re-raises the IRQ forever. */
	if (serial == state->old_serial && !state->saw_empty)
		return d;
	state->saw_empty = 0;

	/* Torn reads, cold-boot 0xFFFF and corrupt slots: release the slot
	 * without completing a client read. */
	if (size < ZZNET_RX_MIN_FRAME || size > ZZNET_RX_MAX_FRAME) {
		d.action = ZZNET_RX_DROP;
		d.bad_data = 1;
		state->have_baseline = 1;
		state->old_serial = serial;
		return d;
	}

	/* Gap detection: a reasonable gap is bounded by the backlog depth; a
	 * much larger delta is an artifact (torn header, firmware restart), so
	 * it counts as BadData rather than Overruns. Firmware skips serials 0
	 * and 1 on its u16 wrap, so a clean 0xffff -> 2 step has a raw delta
	 * of 3; discount the two sentinels when the serial wrapped. */
	if (state->have_baseline) {
		uint16_t delta = (uint16_t)(serial - state->old_serial);

		if (serial < state->old_serial)
			delta = (uint16_t)(delta - 2);
		if (delta > 1 && delta <= 128)
			d.overruns = (uint16_t)(delta - 1);
		else if (delta > 128)
			d.bad_data = 1;
	}
	state->have_baseline = 1;
	state->old_serial = serial;
	d.action = ZZNET_RX_DELIVER;
	return d;
}
