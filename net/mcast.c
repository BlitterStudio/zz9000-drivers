/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "mcast.h"

#include <string.h>

uint16_t zznet_mcast_hash(const uint8_t a[ZZNET_MCAST_ADDR_LEN])
{
	return (uint16_t)((a[0] & 0x3f) ^
	                  ((a[0] >> 6) | ((a[1] & 0x0f) << 2)) ^
	                  ((a[1] >> 4) | ((a[2] & 0x03) << 4)) ^
	                  (a[2] >> 2) ^
	                  (a[3] & 0x3f) ^
	                  ((a[3] >> 6) | ((a[4] & 0x0f) << 2)) ^
	                  ((a[4] >> 4) | ((a[5] & 0x03) << 4)) ^
	                  (a[5] >> 2));
}

int zznet_mcast_send_ok(const uint8_t dst[ZZNET_MCAST_ADDR_LEN])
{
	return (dst[0] & 1) != 0;
}

static int zznet_mcast_is_broadcast(const uint8_t *dst)
{
	int i;

	for (i = 0; i < ZZNET_MCAST_ADDR_LEN; i++) {
		if (dst[i] != 0xff)
			return 0;
	}
	return 1;
}

unsigned zznet_mcast_rx_flags(const uint8_t dst[ZZNET_MCAST_ADDR_LEN])
{
	if (zznet_mcast_is_broadcast(dst))
		return ZZNET_RXF_BCAST;
	if (dst[0] & 1)
		return ZZNET_RXF_MCAST;
	return 0;
}

int zznet_mcast_membership_required(const uint8_t dst[ZZNET_MCAST_ADDR_LEN])
{
	if ((dst[0] & 1) == 0)
		return 0;
	return !zznet_mcast_is_broadcast(dst);
}

static int zznet_mcast_same(const uint8_t *a, const uint8_t *b)
{
	return memcmp(a, b, ZZNET_MCAST_ADDR_LEN) == 0;
}

static int zznet_mcast_is_member(const struct zznet_mcast *m, const uint8_t *dst)
{
	int i;

	for (i = 0; i < ZZNET_MCAST_MAX; i++) {
		if (m->entry[i].refs != 0 &&
		    zznet_mcast_same(m->entry[i].addr, dst))
			return 1;
	}
	return 0;
}

struct zznet_rx_plan zznet_mcast_rx_plan(const struct zznet_mcast *m,
                                         const uint8_t dst[ZZNET_MCAST_ADDR_LEN])
{
	struct zznet_rx_plan plan;

	plan.select_reader = 1;
	plan.count_unknown = 0;
	if (!zznet_mcast_membership_required(dst))
		return plan;
	if (m != 0 && zznet_mcast_is_member(m, dst))
		return plan;
	/* Exact miss, including a GEM hash collision. Do not select a
	 * reader and do not count UnknownTypesReceived. */
	plan.select_reader = 0;
	return plan;
}

static int zznet_mcast_bucket_used(const struct zznet_mcast *m, uint16_t bucket)
{
	int i;

	for (i = 0; i < ZZNET_MCAST_MAX; i++) {
		if (m->entry[i].refs != 0 &&
		    zznet_mcast_hash(m->entry[i].addr) == bucket)
			return 1;
	}
	return 0;
}

int zznet_mcast_update(struct zznet_mcast *m, const struct zznet_mcast_io *io,
                       const uint8_t addr[ZZNET_MCAST_ADDR_LEN], int add)
{
	struct zznet_mcast_entry *free_entry = 0;
	struct zznet_mcast_entry *slot = 0;
	int i;

	for (i = 0; i < ZZNET_MCAST_MAX; i++) {
		if (m->entry[i].refs == 0) {
			if (!free_entry)
				free_entry = &m->entry[i];
		} else if (zznet_mcast_same(m->entry[i].addr, addr)) {
			slot = &m->entry[i];
			break;
		}
	}

	if (add) {
		if (slot) {
			if (slot->refs == 0xffffu)
				return ZZNET_S2ERR_NO_RESOURCES;
			slot->refs++;
			return 0;
		}
		if (!free_entry)
			return ZZNET_S2ERR_NO_RESOURCES;
		if (m->count == 0 && !io->capable(io->ctx))
			return ZZNET_S2ERR_NOT_SUPPORTED;
		{
			uint16_t bucket = zznet_mcast_hash(addr);
			int bucket_used = zznet_mcast_bucket_used(m, bucket);

			memcpy(free_entry->addr, addr, ZZNET_MCAST_ADDR_LEN);
			free_entry->refs = 1;
			m->count++;
			if (!bucket_used)
				io->command(io->ctx,
				            (uint16_t)(ZZNET_ETH_CONFIG_HASH_SET | bucket));
		}
		return 0;
	}

	if (!slot)
		return ZZNET_S2ERR_BAD_STATE;
	if (--slot->refs == 0) {
		uint16_t bucket = zznet_mcast_hash(slot->addr);

		memset(slot->addr, 0, ZZNET_MCAST_ADDR_LEN);
		m->count--;
		if (!zznet_mcast_bucket_used(m, bucket))
			io->command(io->ctx,
			            (uint16_t)(ZZNET_ETH_CONFIG_HASH_CLEAR | bucket));
	}
	return 0;
}

void zznet_mcast_reset(struct zznet_mcast *m, const struct zznet_mcast_io *io)
{
	if (io->capable(io->ctx))
		io->command(io->ctx, ZZNET_ETH_CONFIG_HASH_RESET);
	memset(m, 0, sizeof(*m));
}
