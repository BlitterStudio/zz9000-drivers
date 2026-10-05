/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Host coverage for the RX slot model against a model of the firmware
 * backlog ring (zz9000-firmware ethernet.c). Build/run:
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

/* Firmware 2.1+: slots are cleared when consumed and when the backlog
 * drains; serials skip 0 and 1; acks must match the presented serial
 * (1 is the legacy bare advance); a DMA restart clears everything and
 * resets the serial counter; ETH_RX_STATUS reports the ready count.
 *
 * Legacy (before 2.1): the backlog restarts at slot 0 once drained, slots
 * are never cleared, any ack advances, and ETH_RX_STATUS reads 0. */
struct fw {
	int legacy;
	uint16_t size[SLOTS];
	uint16_t serial[SLOTS];
	uint16_t read, write, backlog, frame_serial;
};

static void fw_restart(struct fw *f)
{
	int legacy = f->legacy;

	memset(f, 0, sizeof(*f));
	f->legacy = legacy;
}

static uint16_t fw_next_serial(struct fw *f)
{
	f->frame_serial++;
	if (!f->legacy && (f->frame_serial == 0 || f->frame_serial == 1))
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
	if (f->legacy) {
		f->read++;
		if (--f->backlog == 0)
			f->read = f->write = 0;
		return;
	}
	if (acked == 0 || (acked != 1 && acked != f->serial[f->read]))
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

	return ((uint32_t)f->size[f->read] << 16) | f->serial[f->read];
}

static uint16_t fw_status(void *ctx)
{
	struct fw *f = ctx;

	return f->legacy ? 0 : (uint16_t)(f->backlog > 0xff ? 0xff : f->backlog);
}

struct framer {
	struct zznet_rx_state state;
	struct zznet_rx_io io;
	unsigned delivered, overruns, bad_data;
};

static void framer_init(struct framer *fr, struct fw *f)
{
	memset(fr, 0, sizeof(*fr));
	zznet_rx_reset(&fr->state);
	fr->io.read_header = fw_header;
	fr->io.rx_status = fw_status;
	fr->io.ctx = f;
}

/* One frame_proc iteration; acks before returning, as frame_proc does. */
static enum zznet_rx_action framer_step(struct framer *fr, struct fw *f)
{
	struct zznet_rx_decision d = zznet_rx_next(&fr->state, &fr->io);

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
	struct fw f = { 0 };
	struct framer fr;

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
	struct fw f = { 0 };
	struct framer fr;

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

static int test_legacy_stale_slot_is_not_redelivered(void)
{
	struct fw f = { 0 };
	struct framer fr;

	f.legacy = 1;
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

static int test_gap_spans_empty_drain_boundary(void)
{
	struct fw f = { 0 };
	struct framer fr;

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
	struct fw f = { 0 };
	struct framer fr;

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

int main(void)
{
	if (test_restart_after_drain_delivers_reused_serial() != EXIT_SUCCESS)
		return EXIT_FAILURE;
	if (test_restart_between_ack_and_next_read() != EXIT_SUCCESS)
		return EXIT_FAILURE;
	if (test_legacy_stale_slot_is_not_redelivered() != EXIT_SUCCESS)
		return EXIT_FAILURE;
	if (test_gap_spans_empty_drain_boundary() != EXIT_SUCCESS)
		return EXIT_FAILURE;
	if (test_serial_wrap_is_not_an_overrun() != EXIT_SUCCESS)
		return EXIT_FAILURE;
	return EXIT_SUCCESS;
}
