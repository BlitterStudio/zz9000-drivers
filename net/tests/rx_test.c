/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Host coverage for the RX slot model against a model of the firmware
 * backlog ring (zz9000-firmware ethernet.c) in each generation the driver
 * meets. Build/run:
 * make -C net/tests test
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rx.h"

#define CHECK(expr) do { \
	if (!(expr)) { \
		fprintf(stderr, "%s:%d: check failed: %s\n", \
		        __FILE__, __LINE__, #expr); \
		return EXIT_FAILURE; \
	} \
} while (0)

#define SLOTS 128
#define FRAME 60

/* FW_LEGACY (before 2.1): the backlog restarts at slot 0 once drained,
 *   slots are never cleared, any ack advances, register 0x8C is a debug
 *   counter.
 * FW_21 (2.1, 2.2): ring backlog, slots cleared on consume and when the
 *   backlog drains, any ack advances, RX DMA restarts reset the serial,
 *   0x8C reports the ready count.
 * FW_28 (2.8+): as FW_21, plus serials skip 0 and 1 and the ack must match
 *   the presented serial (0 rejected, 1 a legacy bare advance). */
enum fw_gen { FW_LEGACY, FW_21, FW_28 };

struct fw {
	enum fw_gen gen;
	uint16_t size[SLOTS];
	uint16_t serial[SLOTS];
	uint16_t read, write, backlog, frame_serial;
	uint16_t status_extra;  /* legacy debug counter, or status bits 15:8 */
	/* One-shot: the second header read of a step sees this header. */
	int swap_on_reread;
	uint16_t swap_size, swap_serial;
	unsigned reads;
	/* One-shot: a frame lands between the two word reads of a split
	 * (Zorro II) header read. */
	int publish_between_words;
	unsigned words;
};

static uint16_t fw_version(const struct fw *f)
{
	switch (f->gen) {
	case FW_LEGACY: return 0x0200;
	case FW_21:     return 0x0201;
	default:        return 0x0208;
	}
}

static void fw_init(struct fw *f, enum fw_gen gen)
{
	memset(f, 0, sizeof(*f));
	f->gen = gen;
}

static void fw_restart(struct fw *f)
{
	fw_init(f, f->gen);
}

static uint16_t fw_next_serial(struct fw *f)
{
	f->frame_serial++;
	if (f->gen == FW_28 && f->frame_serial < 2)
		f->frame_serial = 2;
	return f->frame_serial;
}

static void fw_receive(struct fw *f)
{
	f->serial[f->write] = fw_next_serial(f);
	f->size[f->write] = FRAME;
	f->write = (f->write + 1) % SLOTS;
	f->backlog++;
}

/* A frame the EMAC took but the firmware dropped: the serial advances. */
static void fw_drop(struct fw *f)
{
	fw_next_serial(f);
}

static void fw_clear(struct fw *f, uint16_t slot)
{
	f->size[slot] = 0;
	f->serial[slot] = 0;
}

static void fw_ack(struct fw *f, uint16_t acked)
{
	if (f->backlog == 0)
		return;
	if (f->gen == FW_LEGACY) {
		f->read++;
		if (--f->backlog == 0)
			f->read = f->write = 0;
		return;
	}
	if (f->gen == FW_28 &&
	    (acked == 0 || (acked != 1 && acked != f->serial[f->read])))
		return;
	fw_clear(f, f->read);
	f->read = (f->read + 1) % SLOTS;
	f->backlog--;
	if (f->backlog == 0)
		fw_clear(f, f->read);
}

static uint32_t fw_header(void *ctx)
{
	struct fw *f = ctx;

	if (++f->reads == 2 && f->swap_on_reread) {
		f->size[f->read] = f->swap_size;
		f->serial[f->read] = f->swap_serial;
		f->swap_on_reread = 0;
	}
	return ((uint32_t)f->size[f->read] << 16) | f->serial[f->read];
}

/* Zorro II: the longword header read is two word cycles. */
static uint16_t fw_word(void *ctx, unsigned off)
{
	struct fw *f = ctx;
	uint16_t v = off == 0 ? f->size[f->read] : f->serial[f->read];

	if (++f->words == 1 && f->publish_between_words) {
		f->publish_between_words = 0;
		fw_receive(f);
	}
	return v;
}

/* The longword read as the bus splits it: size word, then serial word. */
static uint32_t fw_header_size_first(void *ctx)
{
	uint16_t size = fw_word(ctx, 0);

	return ((uint32_t)size << 16) | fw_word(ctx, 2);
}

static uint32_t fw_header_serial_first(void *ctx)
{
	return zznet_rx_header_split(fw_word, ctx);
}

static uint16_t fw_status(void *ctx)
{
	struct fw *f = ctx;

	if (f->gen == FW_LEGACY)
		return f->status_extra;
	return (uint16_t)(f->status_extra |
	                  (f->backlog > 0xff ? 0xff : f->backlog));
}

struct framer {
	struct zznet_rx_state state;
	struct zznet_rx_io io;
	unsigned delivered, overruns, bad_data;
};

static void framer_init(struct framer *fr, struct fw *f)
{
	memset(fr, 0, sizeof(*fr));
	zznet_rx_reset(&fr->state, fw_version(f));
	fr->io.read_header = fw_header;
	fr->io.rx_status = fw_status;
	fr->io.ctx = f;
}

/* One frame_proc iteration; acks before returning, as frame_proc does. */
static enum zznet_rx_action framer_step(struct framer *fr, struct fw *f)
{
	struct zznet_rx_decision d;

	f->reads = 0;
	f->words = 0;
	d = zznet_rx_next(&fr->state, &fr->io);
	fr->overruns += d.overruns;
	fr->bad_data += d.bad_data;
	if (d.action == ZZNET_RX_DELIVER)
		fr->delivered++;
	if (d.action != ZZNET_RX_WAIT)
		fw_ack(f, d.serial);
	return (enum zznet_rx_action)d.action;
}

/* Run until the framer sleeps. Firmware re-raises the IRQ as soon as the
 * framer re-arms it while frames wait, so a WAIT with backlog repeats until
 * the spin bound: the issue #127 livelock. Returns 0 on a legitimate sleep,
 * -1 on livelock. */
static int framer_run(struct framer *fr, struct fw *f)
{
	int spins;

	for (spins = 0; spins < 4 * SLOTS; spins++) {
		if (framer_step(fr, f) == ZZNET_RX_WAIT && f->backlog == 0)
			return 0;
	}
	return -1;
}

static int test_restart_after_drain_delivers_reused_serial(void)
{
	struct fw f;
	struct framer fr;

	fw_init(&f, FW_28);
	framer_init(&fr, &f);
	fw_receive(&f);                 /* serial 2 */
	CHECK(framer_run(&fr, &f) == 0); /* delivers, then reads an empty slot */
	CHECK(fr.delivered == 1);

	fw_restart(&f);                 /* TX-timeout recovery */
	fw_receive(&f);                 /* serial 2 again */
	CHECK(framer_run(&fr, &f) == 0);
	CHECK(fr.delivered == 2);
	CHECK(f.backlog == 0);
	return EXIT_SUCCESS;
}

static int test_restart_between_ack_and_next_read(void)
{
	struct fw f;
	struct framer fr;

	fw_init(&f, FW_28);
	framer_init(&fr, &f);
	fw_receive(&f);                 /* serial 2 */
	CHECK(framer_step(&fr, &f) == ZZNET_RX_DELIVER);

	/* Restart and the next frame land before the framer reads again, so
	 * it never observes the cleared slot. */
	fw_restart(&f);
	fw_receive(&f);                 /* serial 2 again */
	CHECK(framer_run(&fr, &f) == 0);
	CHECK(fr.delivered == 2);
	CHECK(f.backlog == 0);
	return EXIT_SUCCESS;
}

static int test_firmware_2_1_restart_reuses_serial_1(void)
{
	struct fw f;
	struct framer fr;

	fw_init(&f, FW_21);
	framer_init(&fr, &f);
	fw_receive(&f);                 /* serial 1: no skip on 2.1/2.2 */
	CHECK(framer_step(&fr, &f) == ZZNET_RX_DELIVER);

	fw_restart(&f);
	fw_receive(&f);                 /* serial 1 again */
	CHECK(framer_run(&fr, &f) == 0);
	CHECK(fr.delivered == 2);
	CHECK(f.backlog == 0);
	return EXIT_SUCCESS;
}

static int test_restart_with_high_serial_counts_bad_data(void)
{
	struct fw f;
	struct framer fr;

	fw_init(&f, FW_28);
	framer_init(&fr, &f);
	f.frame_serial = 0x1233;
	fw_receive(&f);                 /* 0x1234 */
	CHECK(framer_run(&fr, &f) == 0);

	fw_restart(&f);
	fw_receive(&f);                 /* 2: a reset, not 0xedcd lost frames */
	CHECK(framer_run(&fr, &f) == 0);
	CHECK(fr.delivered == 2);
	CHECK(fr.bad_data == 1);
	CHECK(fr.overruns == 0);
	return EXIT_SUCCESS;
}

static int test_legacy_stale_slot_is_not_redelivered(void)
{
	struct fw f;
	struct framer fr;

	fw_init(&f, FW_LEGACY);
	f.status_extra = 1;             /* debug counter high half, not frames */
	framer_init(&fr, &f);
	fw_receive(&f);
	fw_receive(&f);
	CHECK(framer_run(&fr, &f) == 0);
	/* Drained: the read cursor is back on slot 0, which still holds frame
	 * 1. Its serial differs from the last acked one, so it is delivered
	 * once more (legacy behavior since rev 2.1); after that it must not
	 * loop. */
	CHECK(fr.delivered == 3);
	CHECK(framer_run(&fr, &f) == 0);
	CHECK(fr.delivered == 3);

	fw_receive(&f);                 /* overwrites slot 0 */
	CHECK(framer_run(&fr, &f) == 0);
	CHECK(fr.delivered == 4);
	return EXIT_SUCCESS;
}

static int test_reread_mismatch_waits_without_ack(void)
{
	struct fw f;
	struct framer fr;

	fw_init(&f, FW_28);
	framer_init(&fr, &f);
	fw_receive(&f);                 /* serial 2 */
	CHECK(framer_step(&fr, &f) == ZZNET_RX_DELIVER);
	fw_restart(&f);
	fw_receive(&f);                 /* serial 2 again */

	/* The slot changes between the two header reads. */
	f.swap_on_reread = 1;
	f.swap_size = FRAME;
	f.swap_serial = 3;
	CHECK(framer_step(&fr, &f) == ZZNET_RX_WAIT);
	CHECK(f.backlog == 1);
	CHECK(fr.delivered == 1);

	CHECK(framer_run(&fr, &f) == 0); /* the IRQ wakes it for serial 3 */
	CHECK(fr.delivered == 2);
	CHECK(f.backlog == 0);
	return EXIT_SUCCESS;
}

static int test_status_high_bits_are_not_ready_frames(void)
{
	struct fw f;
	struct framer fr;

	fw_init(&f, FW_28);
	framer_init(&fr, &f);
	fw_receive(&f);                 /* serial 2 */
	CHECK(framer_step(&fr, &f) == ZZNET_RX_DELIVER);

	/* A stale copy of the consumed header with no frames waiting, while
	 * reserved/backpressure bits are set. */
	f.size[f.read] = FRAME;
	f.serial[f.read] = 2;
	f.status_extra = 0x8100;
	CHECK(framer_step(&fr, &f) == ZZNET_RX_WAIT);
	CHECK(fr.delivered == 1);
	return EXIT_SUCCESS;
}

static int test_repeated_serial_zero_waits(void)
{
	struct fw f;
	struct framer fr;

	fw_init(&f, FW_28);
	framer_init(&fr, &f);
	fw_receive(&f);
	/* Corrupt pending slot: valid size, serial 0, which firmware never
	 * assigns and never accepts as an ack. */
	f.serial[f.read] = 0;
	CHECK(framer_step(&fr, &f) == ZZNET_RX_WAIT);
	CHECK(fr.delivered == 0);
	return EXIT_SUCCESS;
}

static int test_bad_size_is_dropped(void)
{
	static const uint16_t sizes[] = { 13, 1519, 0xffff };
	unsigned i;

	for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		struct fw f;
		struct framer fr;

		fw_init(&f, FW_28);
		framer_init(&fr, &f);
		fw_receive(&f);
		f.size[f.read] = sizes[i];
		CHECK(framer_step(&fr, &f) == ZZNET_RX_DROP);
		CHECK(fr.bad_data == 1);
		CHECK(fr.delivered == 0);
		CHECK(f.backlog == 0);
	}
	return EXIT_SUCCESS;
}

static int test_gap_spans_empty_drain_boundary(void)
{
	struct fw f;
	struct framer fr;

	fw_init(&f, FW_28);
	framer_init(&fr, &f);
	fw_receive(&f);                 /* serial 2 */
	CHECK(framer_run(&fr, &f) == 0);
	fw_drop(&f);                    /* 3 */
	fw_drop(&f);                    /* 4 */
	fw_receive(&f);                 /* 5 */
	CHECK(framer_run(&fr, &f) == 0);
	CHECK(fr.delivered == 2);
	CHECK(fr.overruns == 2);
	CHECK(fr.bad_data == 0);
	return EXIT_SUCCESS;
}

static int test_serial_wrap_is_not_an_overrun(void)
{
	struct fw f;
	struct framer fr;

	fw_init(&f, FW_28);
	framer_init(&fr, &f);
	f.frame_serial = 0xfffe;
	fw_receive(&f);                 /* 0xffff */
	fw_receive(&f);                 /* wraps past 0 and 1 to 2 */
	CHECK(framer_run(&fr, &f) == 0);
	CHECK(fr.delivered == 2);
	CHECK(fr.overruns == 0);
	CHECK(fr.bad_data == 0);
	return EXIT_SUCCESS;
}

/* A frame published between the two word reads of a split header read.
 * Size first reads the empty slot's size 0 beside the new serial: the
 * framer drops it as a torn header and the ack consumes it unread.  Serial
 * first sees the empty slot, and the frame on the next read. */
static int run_split_race(uint32_t (*reader)(void *), unsigned *delivered,
                          unsigned *bad_data)
{
	struct fw f;
	struct framer fr;

	fw_init(&f, FW_28);
	framer_init(&fr, &f);
	fr.io.read_header = reader;
	fw_receive(&f);                  /* serial 2 */
	CHECK(framer_run(&fr, &f) == 0);
	CHECK(fr.delivered == 1);
	f.publish_between_words = 1;     /* serial 3 lands mid-read */
	framer_step(&fr, &f);
	CHECK(framer_run(&fr, &f) == 0);
	*delivered = fr.delivered;
	*bad_data = fr.bad_data;
	return EXIT_SUCCESS;
}

static int test_split_header_read_serial_first(void)
{
	unsigned delivered, bad_data;

	CHECK(run_split_race(fw_header_size_first, &delivered, &bad_data) ==
	      EXIT_SUCCESS);
	CHECK(delivered == 1 && bad_data == 1);  /* the failure this avoids */
	CHECK(run_split_race(fw_header_serial_first, &delivered, &bad_data) ==
	      EXIT_SUCCESS);
	CHECK(delivered == 2 && bad_data == 0);
	return EXIT_SUCCESS;
}

int main(void)
{
	static int (*const tests[])(void) = {
		test_restart_after_drain_delivers_reused_serial,
		test_restart_between_ack_and_next_read,
		test_firmware_2_1_restart_reuses_serial_1,
		test_restart_with_high_serial_counts_bad_data,
		test_legacy_stale_slot_is_not_redelivered,
		test_reread_mismatch_waits_without_ack,
		test_status_high_bits_are_not_ready_frames,
		test_repeated_serial_zero_waits,
		test_bad_size_is_dropped,
		test_gap_spans_empty_drain_boundary,
		test_serial_wrap_is_not_an_overrun,
		test_split_header_read_serial_first,
	};
	unsigned i;

	for (i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
		if (tests[i]() != EXIT_SUCCESS)
			return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}
