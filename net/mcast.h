/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Exact SANA-II multicast membership, GEM hash, and register-command
 * decisions. No Amiga headers: the device holds the semaphore and performs
 * MMIO; this model only decides state and commands.
 *
 * Firmware contract (ZZ9000 ethernet config register):
 *   read  bit 0     ZZNET_ETH_CONFIG_CAP_MCAST_HASH
 *   write 0x8000|b  program GEM hash bucket b (0..63)
 *   write 0x4000|b  clear bucket b
 *   write 0x2000    clear every bucket
 * A 64-bucket hash collides, so delivery still requires an exact join.
 */
#ifndef ZZNET_MCAST_H
#define ZZNET_MCAST_H

#include <stdint.h>

#define ZZNET_MCAST_ADDR_LEN 6
#define ZZNET_MCAST_MAX      32
#define ZZNET_MCAST_BUCKETS  64

#define ZZNET_ETH_CONFIG                0x008a
#define ZZNET_ETH_CONFIG_CAP_MCAST_HASH 0x0001
#define ZZNET_ETH_CONFIG_HASH_SET       0x8000
#define ZZNET_ETH_CONFIG_HASH_CLEAR     0x4000
#define ZZNET_ETH_CONFIG_HASH_RESET     0x2000

/* Numeric values of devices/sana2.h. device.c static-asserts the match. */
#define ZZNET_S2ERR_NO_RESOURCES  1
#define ZZNET_S2ERR_BAD_STATE     4
#define ZZNET_S2ERR_BAD_ADDRESS   5
#define ZZNET_S2ERR_NOT_SUPPORTED 8

/* SANA2IOF_MCAST / SANA2IOF_BCAST bit positions. */
#define ZZNET_RXF_MCAST (1u << 5)
#define ZZNET_RXF_BCAST (1u << 6)

struct zznet_mcast_entry {
	uint8_t addr[ZZNET_MCAST_ADDR_LEN];
	uint16_t refs;
};

struct zznet_mcast {
	struct zznet_mcast_entry entry[ZZNET_MCAST_MAX];
	uint16_t count;
};

/* Register window. capable() is non-zero when CAP_MCAST_HASH reads back.
 * command() receives one raw config-register write. Neither is called
 * while holding a lock inside the model; the device serializes callers. */
struct zznet_mcast_io {
	int (*capable)(void *ctx);
	void (*command)(void *ctx, uint16_t command);
	void *ctx;
};

/* Pre-reader RX decision. select_reader == 0 is an exact multicast miss:
 * ack and drop, do not walk packet-type readers, and do not invent an
 * UnknownTypesReceived increment (count_unknown stays 0). Absent-reader
 * and read-failure accounting happen only after a frame is selected. */
struct zznet_rx_plan {
	int select_reader;
	int count_unknown;
};

/* GEM hash: XOR of the eight consecutive 6-bit pieces of the address
 * (Xilinx UG585, GEM network configuration). Result is 0..63. */
uint16_t zznet_mcast_hash(const uint8_t addr[ZZNET_MCAST_ADDR_LEN]);

/* S2_MULTICAST accepts only a group destination (I/G bit set), including
 * the all-ones broadcast address that S2_BROADCAST falls through with. */
int zznet_mcast_send_ok(const uint8_t dst[ZZNET_MCAST_ADDR_LEN]);

/* Receive label for a delivered frame. Broadcast wins over the group bit,
 * so a non-broadcast group is MCAST and not BCAST. */
unsigned zznet_mcast_rx_flags(const uint8_t dst[ZZNET_MCAST_ADDR_LEN]);

/* Non-zero when dst is a group address other than all-ones broadcast.
 * Those frames need the membership table; unicast and broadcast do not. */
int zznet_mcast_membership_required(const uint8_t dst[ZZNET_MCAST_ADDR_LEN]);

struct zznet_rx_plan zznet_mcast_rx_plan(const struct zznet_mcast *m,
                                         const uint8_t dst[ZZNET_MCAST_ADDR_LEN]);

/* add != 0 joins, otherwise leaves. Returns 0 or a ZZNET_S2ERR_* code.
 * Old firmware (capability clear and no current members) returns
 * NOT_SUPPORTED and does not change membership or write a command. */
int zznet_mcast_update(struct zznet_mcast *m, const struct zznet_mcast_io *io,
                       const uint8_t addr[ZZNET_MCAST_ADDR_LEN], int add);

/* First-open / final-close. Clears software state. Writes HASH_RESET
 * only when the capability bit is set. */
void zznet_mcast_reset(struct zznet_mcast *m, const struct zznet_mcast_io *io);

#endif
