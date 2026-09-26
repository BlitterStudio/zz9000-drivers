/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Host coverage for the SANA-II multicast model. Build/run:
 * make -C net/tests test
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mcast.h"

#define CHECK(expr) do { \
	if (!(expr)) { \
		fprintf(stderr, "%s:%d: check failed: %s\n", \
		        __FILE__, __LINE__, #expr); \
		return EXIT_FAILURE; \
	} \
} while (0)

#define REGWIN_LOG 32

/* Mocked ethernet-config window. Reads expose the capability bit;
 * writes are the HASH_SET / HASH_CLEAR / HASH_RESET commands. */
struct regwin {
	uint16_t eth_config;
	uint16_t commands[REGWIN_LOG];
	int ncommands;
	int overflow;
	uint64_t hash_bits;
};

static int reg_capable(void *ctx)
{
	struct regwin *w = ctx;

	return (w->eth_config & ZZNET_ETH_CONFIG_CAP_MCAST_HASH) != 0;
}

static void reg_command(void *ctx, uint16_t command)
{
	struct regwin *w = ctx;

	if (w->ncommands >= REGWIN_LOG) {
		w->overflow = 1;
		w->ncommands++;
		return;
	}
	w->commands[w->ncommands++] = command;
	if (command & ZZNET_ETH_CONFIG_HASH_RESET)
		w->hash_bits = 0;
	else if (command & ZZNET_ETH_CONFIG_HASH_SET)
		w->hash_bits |= (uint64_t)1 << (command & 63);
	else if (command & ZZNET_ETH_CONFIG_HASH_CLEAR)
		w->hash_bits &= ~((uint64_t)1 << (command & 63));
}

static void reg_init(struct regwin *w, struct zznet_mcast_io *io, int capable)
{
	memset(w, 0, sizeof(*w));
	w->eth_config = capable ? ZZNET_ETH_CONFIG_CAP_MCAST_HASH : 0;
	io->capable = reg_capable;
	io->command = reg_command;
	io->ctx = w;
}

static uint16_t refs_of(const struct zznet_mcast *m, const uint8_t *addr)
{
	int i;

	for (i = 0; i < ZZNET_MCAST_MAX; i++) {
		if (m->entry[i].refs != 0 &&
		    memcmp(m->entry[i].addr, addr, ZZNET_MCAST_ADDR_LEN) == 0)
			return m->entry[i].refs;
	}
	return 0;
}

static int occupied(const struct zznet_mcast *m)
{
	int i, n;

	n = 0;
	for (i = 0; i < ZZNET_MCAST_MAX; i++) {
		if (m->entry[i].refs != 0)
			n++;
	}
	return n;
}

/* Group addresses whose only nonzero 6-bit pieces are p0 and p4, so the
 * hash is (addr[0] & 0x3f) XOR addr[3]. Bit 0 of addr[0] stays set. */
static const uint8_t k_bucket0[6]  = {0x01, 0x00, 0x00, 0x01, 0x00, 0x00};
static const uint8_t k_bucket31[6] = {0x01, 0x00, 0x00, 0x1e, 0x00, 0x00};
static const uint8_t k_bucket32[6] = {0x01, 0x00, 0x00, 0x21, 0x00, 0x00};
static const uint8_t k_bucket63[6] = {0x01, 0x00, 0x00, 0x3e, 0x00, 0x00};
/* Distinct groups that share GEM bucket 1. */
static const uint8_t k_group_a[6]  = {0x01, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t k_group_b[6]  = {0x03, 0x00, 0x00, 0x02, 0x00, 0x00};

static const uint8_t k_unicast[6]   = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
static const uint8_t k_broadcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
static const uint8_t k_group[6]     = {0x01, 0x00, 0x5e, 0x00, 0x00, 0x01};
static const uint8_t k_almost_bc[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xfe};

struct pending_read {
	uint16_t packet_type;
	int pending;
};

/* Minimal reader selection. Production walks an Exec list, but it must
 * honor the same plan: an exact miss leaves every reader pending and
 * does not increment unknown types. An accepted frame consumes the first
 * matching packet type, or counts unknown when none matches. */
static int offer_frame(const struct zznet_mcast *m, const uint8_t *dst,
                       uint16_t packet_type, struct pending_read *readers,
                       int nreaders, uint32_t *unknown)
{
	struct zznet_rx_plan plan = zznet_mcast_rx_plan(m, dst);
	int i;

	if (!plan.select_reader) {
		if (plan.count_unknown)
			(*unknown)++;
		return 0;
	}
	for (i = 0; i < nreaders; i++) {
		if (readers[i].pending && readers[i].packet_type == packet_type) {
			readers[i].pending = 0;
			return 1;
		}
	}
	(*unknown)++;
	return 0;
}

static int test_contract_constants(void)
{
	CHECK(ZZNET_ETH_CONFIG == 0x008a);
	CHECK(ZZNET_ETH_CONFIG_CAP_MCAST_HASH == 0x0001);
	CHECK(ZZNET_ETH_CONFIG_HASH_SET == 0x8000);
	CHECK(ZZNET_ETH_CONFIG_HASH_CLEAR == 0x4000);
	CHECK(ZZNET_ETH_CONFIG_HASH_RESET == 0x2000);
	CHECK(ZZNET_S2ERR_NOT_SUPPORTED == 8);
	CHECK(ZZNET_S2ERR_BAD_ADDRESS == 5);
	CHECK(ZZNET_S2ERR_BAD_STATE == 4);
	CHECK(ZZNET_S2ERR_NO_RESOURCES == 1);
	CHECK(ZZNET_RXF_MCAST == (1u << 5));
	CHECK(ZZNET_RXF_BCAST == (1u << 6));
	CHECK(ZZNET_MCAST_BUCKETS == 64);
	return EXIT_SUCCESS;
}

static int test_duplicate_joins_across_openers(void)
{
	struct zznet_mcast m;
	struct regwin win;
	struct zznet_mcast_io io;
	uint16_t bucket;

	memset(&m, 0, sizeof(m));
	reg_init(&win, &io, 1);
	bucket = zznet_mcast_hash(k_group_a);

	/* Two openers join the same group. One hash program, refs == 2. */
	CHECK(zznet_mcast_update(&m, &io, k_group_a, 1) == 0);
	CHECK(zznet_mcast_update(&m, &io, k_group_a, 1) == 0);
	CHECK(refs_of(&m, k_group_a) == 2);
	CHECK(m.count == 1);
	CHECK(win.ncommands == 1);
	CHECK(win.commands[0] == (uint16_t)(ZZNET_ETH_CONFIG_HASH_SET | bucket));
	CHECK(win.hash_bits == ((uint64_t)1 << bucket));

	/* Partial leave keeps the group and the bucket. */
	CHECK(zznet_mcast_update(&m, &io, k_group_a, 0) == 0);
	CHECK(refs_of(&m, k_group_a) == 1);
	CHECK(m.count == 1);
	CHECK(win.ncommands == 1);
	CHECK(zznet_mcast_rx_plan(&m, k_group_a).select_reader == 1);

	/* Final leave clears the bucket once. A further leave is not a member. */
	CHECK(zznet_mcast_update(&m, &io, k_group_a, 0) == 0);
	CHECK(refs_of(&m, k_group_a) == 0);
	CHECK(m.count == 0);
	CHECK(occupied(&m) == 0);
	CHECK(win.ncommands == 2);
	CHECK(win.commands[1] == (uint16_t)(ZZNET_ETH_CONFIG_HASH_CLEAR | bucket));
	CHECK(win.hash_bits == 0);
	CHECK(zznet_mcast_update(&m, &io, k_group_a, 0) == ZZNET_S2ERR_BAD_STATE);
	CHECK(win.ncommands == 2);
	CHECK(!win.overflow);
	return EXIT_SUCCESS;
}

static int test_shared_bucket_partial_and_final_leave(void)
{
	struct zznet_mcast m;
	struct regwin win;
	struct zznet_mcast_io io;
	uint16_t bucket;

	memset(&m, 0, sizeof(m));
	reg_init(&win, &io, 1);
	CHECK(zznet_mcast_hash(k_group_a) == zznet_mcast_hash(k_group_b));
	CHECK(memcmp(k_group_a, k_group_b, ZZNET_MCAST_ADDR_LEN) != 0);
	bucket = zznet_mcast_hash(k_group_a);

	CHECK(zznet_mcast_update(&m, &io, k_group_a, 1) == 0);
	CHECK(zznet_mcast_update(&m, &io, k_group_a, 1) == 0); /* refs 2 */
	CHECK(zznet_mcast_update(&m, &io, k_group_b, 1) == 0);
	CHECK(win.ncommands == 1);
	CHECK(win.commands[0] == (uint16_t)(ZZNET_ETH_CONFIG_HASH_SET | bucket));
	CHECK(m.count == 2);
	CHECK(refs_of(&m, k_group_a) == 2);
	CHECK(refs_of(&m, k_group_b) == 1);

	/* Partial leave of A: address stays, bucket stays, no CLEAR. */
	CHECK(zznet_mcast_update(&m, &io, k_group_a, 0) == 0);
	CHECK(refs_of(&m, k_group_a) == 1);
	CHECK(win.ncommands == 1);
	CHECK(win.hash_bits == ((uint64_t)1 << bucket));
	CHECK(zznet_mcast_rx_plan(&m, k_group_a).select_reader == 1);
	CHECK(zznet_mcast_rx_plan(&m, k_group_b).select_reader == 1);

	/* Final leave of A while B still shares the bucket: no CLEAR.
	 * A is now an exact collision miss; B is still a member. */
	CHECK(zznet_mcast_update(&m, &io, k_group_a, 0) == 0);
	CHECK(refs_of(&m, k_group_a) == 0);
	CHECK(refs_of(&m, k_group_b) == 1);
	CHECK(m.count == 1);
	CHECK(win.ncommands == 1);
	CHECK(win.hash_bits == ((uint64_t)1 << bucket));
	CHECK(zznet_mcast_rx_plan(&m, k_group_a).select_reader == 0);
	CHECK(zznet_mcast_rx_plan(&m, k_group_a).count_unknown == 0);
	CHECK(zznet_mcast_rx_plan(&m, k_group_b).select_reader == 1);

	/* Final leave of the last exact group clears the shared bucket. */
	CHECK(zznet_mcast_update(&m, &io, k_group_b, 0) == 0);
	CHECK(m.count == 0);
	CHECK(occupied(&m) == 0);
	CHECK(win.ncommands == 2);
	CHECK(win.commands[1] == (uint16_t)(ZZNET_ETH_CONFIG_HASH_CLEAR | bucket));
	CHECK(win.hash_bits == 0);
	CHECK(zznet_mcast_rx_plan(&m, k_group_a).select_reader == 0);
	CHECK(zznet_mcast_rx_plan(&m, k_group_b).select_reader == 0);
	CHECK(!win.overflow);
	return EXIT_SUCCESS;
}

static int test_collision_leaves_matching_reader_pending(void)
{
	struct zznet_mcast m;
	struct regwin win;
	struct zznet_mcast_io io;
	struct pending_read reader;
	struct zznet_rx_plan plan;
	uint32_t unknown;

	memset(&m, 0, sizeof(m));
	reg_init(&win, &io, 1);
	CHECK(zznet_mcast_update(&m, &io, k_group_a, 1) == 0);
	CHECK(zznet_mcast_hash(k_group_a) == zznet_mcast_hash(k_group_b));

	reader.packet_type = 0x0800;
	reader.pending = 1;
	unknown = 0;

	plan = zznet_mcast_rx_plan(&m, k_group_b);
	CHECK(plan.select_reader == 0);
	CHECK(plan.count_unknown == 0);
	CHECK(offer_frame(&m, k_group_b, 0x0800, &reader, 1, &unknown) == 0);
	CHECK(reader.pending == 1);
	CHECK(reader.packet_type == 0x0800);
	CHECK(unknown == 0);

	/* The same pending read is still eligible for a real member frame. */
	plan = zznet_mcast_rx_plan(&m, k_group_a);
	CHECK(plan.select_reader == 1);
	CHECK(plan.count_unknown == 0);
	CHECK(offer_frame(&m, k_group_a, 0x0800, &reader, 1, &unknown) == 1);
	CHECK(reader.pending == 0);
	CHECK(unknown == 0);

	/* Accepted frame, no reader: this is the unknown-type path. */
	CHECK(offer_frame(&m, k_group_a, 0x0800, &reader, 1, &unknown) == 0);
	CHECK(unknown == 1);

	/* A later collision of the same packet type must not consume a
	 * replacement read or touch the unknown-type counter. */
	reader.packet_type = 0x0806;
	reader.pending = 1;
	unknown = 4;
	plan = zznet_mcast_rx_plan(&m, k_group_b);
	CHECK(plan.select_reader == 0);
	CHECK(plan.count_unknown == 0);
	CHECK(offer_frame(&m, k_group_b, 0x0806, &reader, 1, &unknown) == 0);
	CHECK(reader.pending == 1);
	CHECK(unknown == 4);

	/* Unicast and broadcast never take the collision path, even with
	 * an empty extra table pointer — they do not consult membership. */
	CHECK(zznet_mcast_membership_required(k_unicast) == 0);
	CHECK(zznet_mcast_membership_required(k_broadcast) == 0);
	CHECK(zznet_mcast_membership_required(k_group_b) == 1);
	plan = zznet_mcast_rx_plan(0, k_unicast);
	CHECK(plan.select_reader == 1);
	CHECK(plan.count_unknown == 0);
	plan = zznet_mcast_rx_plan(0, k_broadcast);
	CHECK(plan.select_reader == 1);
	CHECK(plan.count_unknown == 0);
	return EXIT_SUCCESS;
}

static int test_old_firmware_rejects_without_mutation(void)
{
	struct zznet_mcast m;
	struct zznet_mcast before;
	struct regwin win;
	struct zznet_mcast_io io;

	memset(&m, 0xa5, sizeof(m));
	reg_init(&win, &io, 0);
	zznet_mcast_reset(&m, &io);
	CHECK(win.ncommands == 0);
	CHECK(win.hash_bits == 0);
	CHECK(m.count == 0);
	CHECK(occupied(&m) == 0);

	before = m;
	CHECK(zznet_mcast_update(&m, &io, k_group_a, 1) == ZZNET_S2ERR_NOT_SUPPORTED);
	CHECK(memcmp(&m, &before, sizeof(m)) == 0);
	CHECK(win.ncommands == 0);
	CHECK(win.hash_bits == 0);
	CHECK(refs_of(&m, k_group_a) == 0);

	/* Still unsupported: the failed join must not arm a later one. */
	CHECK(zznet_mcast_update(&m, &io, k_group_b, 1) == ZZNET_S2ERR_NOT_SUPPORTED);
	CHECK(memcmp(&m, &before, sizeof(m)) == 0);
	CHECK(win.ncommands == 0);
	CHECK(!win.overflow);
	return EXIT_SUCCESS;
}

static int test_hash_buckets_0_31_32_63(void)
{
	static const struct {
		const uint8_t *addr;
		uint16_t bucket;
	} cases[] = {
		{k_bucket0, 0},
		{k_bucket31, 31},
		{k_bucket32, 32},
		{k_bucket63, 63},
	};
	struct zznet_mcast m;
	struct regwin win;
	struct zznet_mcast_io io;
	int i;

	memset(&m, 0, sizeof(m));
	reg_init(&win, &io, 1);
	for (i = 0; i < 4; i++) {
		uint16_t bucket = cases[i].bucket;

		CHECK(zznet_mcast_hash(cases[i].addr) == bucket);
		CHECK((cases[i].addr[0] & 1) == 1);
		CHECK(zznet_mcast_update(&m, &io, cases[i].addr, 1) == 0);
		CHECK(win.commands[i] == (uint16_t)(ZZNET_ETH_CONFIG_HASH_SET | bucket));
		CHECK((win.hash_bits & ((uint64_t)1 << bucket)) != 0);
	}
	CHECK(win.ncommands == 4);
	CHECK(m.count == 4);
	CHECK(win.hash_bits == (((uint64_t)1 << 0) | ((uint64_t)1 << 31) |
	                        ((uint64_t)1 << 32) | ((uint64_t)1 << 63)));

	CHECK(zznet_mcast_update(&m, &io, k_bucket0, 0) == 0);
	CHECK(win.ncommands == 5);
	CHECK(win.commands[4] == (uint16_t)(ZZNET_ETH_CONFIG_HASH_CLEAR | 0));
	CHECK((win.hash_bits & ((uint64_t)1 << 0)) == 0);
	CHECK((win.hash_bits & ((uint64_t)1 << 31)) != 0);
	CHECK((win.hash_bits & ((uint64_t)1 << 32)) != 0);
	CHECK((win.hash_bits & ((uint64_t)1 << 63)) != 0);
	CHECK(!win.overflow);
	return EXIT_SUCCESS;
}

static int test_send_and_receive_flags(void)
{
	CHECK(zznet_mcast_send_ok(k_unicast) == 0);
	CHECK(zznet_mcast_send_ok(k_group) == 1);
	CHECK(zznet_mcast_send_ok(k_broadcast) == 1);
	CHECK(zznet_mcast_send_ok(k_almost_bc) == 1);

	CHECK(zznet_mcast_rx_flags(k_unicast) == 0);
	CHECK(zznet_mcast_rx_flags(k_group) == ZZNET_RXF_MCAST);
	CHECK((zznet_mcast_rx_flags(k_group) & ZZNET_RXF_BCAST) == 0);
	CHECK(zznet_mcast_rx_flags(k_almost_bc) == ZZNET_RXF_MCAST);
	CHECK(zznet_mcast_rx_flags(k_broadcast) == ZZNET_RXF_BCAST);
	CHECK((zznet_mcast_rx_flags(k_broadcast) & ZZNET_RXF_MCAST) == 0);
	return EXIT_SUCCESS;
}

static int test_open_close_reset_clears_software_and_register(void)
{
	struct zznet_mcast m;
	struct regwin win;
	struct zznet_mcast_io io;
	uint16_t bucket;

	/* First open on capable firmware: RESET, even if software is dirty. */
	memset(&m, 0xa5, sizeof(m));
	reg_init(&win, &io, 1);
	zznet_mcast_reset(&m, &io);
	CHECK(win.ncommands == 1);
	CHECK(win.commands[0] == ZZNET_ETH_CONFIG_HASH_RESET);
	CHECK(win.hash_bits == 0);
	CHECK(m.count == 0);
	CHECK(occupied(&m) == 0);

	CHECK(zznet_mcast_update(&m, &io, k_group_a, 1) == 0);
	CHECK(zznet_mcast_update(&m, &io, k_bucket63, 1) == 0);
	bucket = zznet_mcast_hash(k_group_a);
	CHECK(win.hash_bits == (((uint64_t)1 << bucket) | ((uint64_t)1 << 63)));
	CHECK(m.count == 2);

	/* Final close: one RESET, not a per-bucket CLEAR, and no leftover join. */
	zznet_mcast_reset(&m, &io);
	CHECK(win.ncommands == 4);
	CHECK(win.commands[3] == ZZNET_ETH_CONFIG_HASH_RESET);
	CHECK(win.hash_bits == 0);
	CHECK(m.count == 0);
	CHECK(occupied(&m) == 0);
	CHECK(refs_of(&m, k_group_a) == 0);
	CHECK(zznet_mcast_rx_plan(&m, k_group_a).select_reader == 0);

	/* The next open's first join programs the bucket again. */
	CHECK(zznet_mcast_update(&m, &io, k_group_a, 1) == 0);
	CHECK(win.ncommands == 5);
	CHECK(win.commands[4] == (uint16_t)(ZZNET_ETH_CONFIG_HASH_SET | bucket));
	CHECK(refs_of(&m, k_group_a) == 1);

	/* Capability gone: reset still clears software and does not write. */
	win.eth_config = 0;
	zznet_mcast_reset(&m, &io);
	CHECK(win.ncommands == 5);
	CHECK(m.count == 0);
	CHECK(occupied(&m) == 0);
	CHECK(win.hash_bits == ((uint64_t)1 << bucket));
	CHECK(!win.overflow);
	return EXIT_SUCCESS;
}

int main(void)
{
	if (test_contract_constants() != EXIT_SUCCESS)
		return EXIT_FAILURE;
	if (test_duplicate_joins_across_openers() != EXIT_SUCCESS)
		return EXIT_FAILURE;
	if (test_shared_bucket_partial_and_final_leave() != EXIT_SUCCESS)
		return EXIT_FAILURE;
	if (test_collision_leaves_matching_reader_pending() != EXIT_SUCCESS)
		return EXIT_FAILURE;
	if (test_old_firmware_rejects_without_mutation() != EXIT_SUCCESS)
		return EXIT_FAILURE;
	if (test_hash_buckets_0_31_32_63() != EXIT_SUCCESS)
		return EXIT_FAILURE;
	if (test_send_and_receive_flags() != EXIT_SUCCESS)
		return EXIT_FAILURE;
	if (test_open_close_reset_clears_software_and_register() != EXIT_SUCCESS)
		return EXIT_FAILURE;
	return EXIT_SUCCESS;
}
