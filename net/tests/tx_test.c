/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Host coverage for the asynchronous TX slot accounting against a model of
 * the firmware's in-order completion count (zz9000-firmware eth_tx_order.h).
 * Build/run:
 * make -C net/tests test
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tx.h"

#define CHECK(expr) do { \
	if (!(expr)) { \
		fprintf(stderr, "%s:%d: check failed: %s\n", \
		        __FILE__, __LINE__, #expr); \
		return EXIT_FAILURE; \
	} \
} while (0)

/* Firmware side: submissions in order, 1 = reached the GEM, 0 = refused.
 * A refusal retires only once everything ahead of it has. */
struct fw {
	uint16_t done;
	uint8_t q[8];
	unsigned head, len;
	int slot_owned[ZZNET_TX_SLOTS];
	uint8_t slot_of[8];
};

static uint16_t fw_status(const struct fw *f)
{
	return (uint16_t)(ZZNET_TX_STATUS_PRESENT | (f->done & ZZNET_TX_STATUS_COUNT));
}

static void fw_retire_head(struct fw *f)
{
	f->slot_owned[f->slot_of[f->head]] = 0;
	f->head = (f->head + 1) % 8;
	f->len--;
	f->done++;
}

static void fw_retire_refused(struct fw *f)
{
	while (f->len > 0 && f->q[f->head] == 0)
		fw_retire_head(f);
}

/* Returns 0 when the driver submitted into a slot the GEM still owns. */
static int fw_submit(struct fw *f, uint16_t word, int accept)
{
	unsigned slot = (word >> ZZNET_TX_SLOT_SHIFT) & 3;
	unsigned at = (f->head + f->len) % 8;

	if (!(word & ZZNET_TX_ASYNC) || f->slot_owned[slot])
		return 0;
	f->slot_owned[slot] = 1;
	f->slot_of[at] = (uint8_t)slot;
	f->q[at] = (uint8_t)(accept != 0);
	f->len++;
	fw_retire_refused(f);
	return 1;
}

/* The GEM finished the oldest BD. */
static void fw_complete(struct fw *f)
{
	if (f->len == 0 || f->q[f->head] == 0)
		return;
	fw_retire_head(f);
	fw_retire_refused(f);
}

/* DMA restart: everything outstanding retires. */
static void fw_flush(struct fw *f)
{
	while (f->len > 0)
		fw_retire_head(f);
}

static int submit(struct zznet_tx_state *s, struct fw *f, int accept)
{
	int slot = zznet_tx_slot(s);

	if (slot < 0) {
		zznet_tx_reclaim(s, fw_status(f));
		slot = zznet_tx_slot(s);
	}
	if (slot < 0)
		return -1;
	if (!fw_submit(f, zznet_tx_word(slot, 60), accept))
		return -2;
	zznet_tx_submitted(s);
	return slot;
}

int main(void)
{
	struct zznet_tx_state s;
	struct fw f;
	int i;

	/* Old firmware reads 0: no asynchronous path. */
	CHECK(zznet_tx_reset(&s, 0) == 0);

	/* Present, slots taken in order, full after four. */
	memset(&f, 0, sizeof(f));
	CHECK(zznet_tx_reset(&s, fw_status(&f)) == 1);
	for (i = 0; i < 4; i++)
		CHECK(submit(&s, &f, 1) == i);
	CHECK(zznet_tx_slot(&s) < 0);
	CHECK(submit(&s, &f, 1) == -1);

	/* One completion frees exactly the oldest slot. */
	fw_complete(&f);
	CHECK(submit(&s, &f, 1) == 0);
	CHECK(submit(&s, &f, 1) == -1);

	/* A refusal behind DMA-owned frames is not counted before them, so
	 * its slot is not handed back early. */
	fw_complete(&f);           /* slot 1 */
	CHECK(submit(&s, &f, 0) == 1);
	CHECK(f.done == 2);
	CHECK(submit(&s, &f, 1) == -1);
	fw_complete(&f);           /* slot 2 */
	fw_complete(&f);           /* slot 3 */
	CHECK(f.done == 4);
	fw_complete(&f);           /* slot 0, then the refused slot 1 */
	CHECK(f.done == 6);
	CHECK(zznet_tx_reclaim(&s, fw_status(&f)) == 4);
	CHECK(s.inuse == 0);

	/* A refusal with nothing ahead retires at once. */
	CHECK(submit(&s, &f, 0) == 2);
	CHECK(zznet_tx_reclaim(&s, fw_status(&f)) == 1);
	CHECK(s.inuse == 0);

	/* DMA restart retires everything outstanding. */
	CHECK(submit(&s, &f, 1) == 3);
	CHECK(submit(&s, &f, 1) == 0);
	fw_flush(&f);
	CHECK(zznet_tx_reclaim(&s, fw_status(&f)) == 2);
	CHECK(s.inuse == 0);

	/* The 15-bit count wraps. */
	memset(&f, 0, sizeof(f));
	f.done = 0x7ffe;
	zznet_tx_reset(&s, fw_status(&f));
	for (i = 0; i < 4; i++)
		CHECK(submit(&s, &f, 1) == i);
	for (i = 0; i < 3; i++)
		fw_complete(&f);
	CHECK((f.done & ZZNET_TX_STATUS_COUNT) == 1);
	CHECK(zznet_tx_reclaim(&s, fw_status(&f)) == 3);
	CHECK(s.inuse == 1);

	/* Firmware restarted underneath: never free more than outstanding,
	 * and the baseline follows the new count. */
	CHECK(zznet_tx_reclaim(&s, ZZNET_TX_STATUS_PRESENT | 0x1234) == 1);
	CHECK(s.inuse == 0 && s.done == 0x1234);
	CHECK(zznet_tx_reclaim(&s, ZZNET_TX_STATUS_PRESENT | 0x1240) == 0);
	CHECK(s.done == 0x1240);

	/* Length word layout. */
	CHECK(zznet_tx_word(3, 1518) == (0x8000 | (3 << 11) | 1518));
	CHECK((zznet_tx_word(3, 0x7ff) & ZZNET_TX_OFFSET2) == 0);
	CHECK(zznet_tx_word(0, 2046) == (ZZNET_TX_ASYNC | 2046));

	printf("tx_test: ok\n");
	return EXIT_SUCCESS;
}
