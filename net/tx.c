/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "tx.h"

int zznet_tx_reset(struct zznet_tx_state *state, uint16_t status)
{
	state->done = (uint16_t)(status & ZZNET_TX_STATUS_COUNT);
	state->next = 0;
	state->inuse = 0;
	return (status & ZZNET_TX_STATUS_PRESENT) != 0;
}

unsigned zznet_tx_reclaim(struct zznet_tx_state *state, uint16_t status)
{
	uint16_t now = (uint16_t)(status & ZZNET_TX_STATUS_COUNT);
	uint16_t n = (uint16_t)((now - state->done) & ZZNET_TX_STATUS_COUNT);

	/* Nothing of ours outstanding: whatever moved the count was not ours
	 * to free, but the baseline still follows it. */
	state->done = now;
	if (state->inuse == 0)
		return 0;
	if (n > state->inuse)
		n = state->inuse;
	state->inuse = (uint8_t)(state->inuse - n);
	return n;
}

int zznet_tx_slot(const struct zznet_tx_state *state)
{
	if (state->inuse >= ZZNET_TX_SLOTS)
		return -1;
	return state->next & (ZZNET_TX_SLOTS - 1);
}

uint16_t zznet_tx_word(int slot, uint16_t len)
{
	return (uint16_t)(ZZNET_TX_ASYNC |
	                  ((unsigned)slot << ZZNET_TX_SLOT_SHIFT) |
	                  (len & ZZNET_TX_LEN_MASK));
}

void zznet_tx_submitted(struct zznet_tx_state *state)
{
	state->next++;
	state->inuse++;
}
