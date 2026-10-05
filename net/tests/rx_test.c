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

/* Firmware side: slots are cleared when consumed and when the backlog
 * drains; serials skip 0 and 1; acks must match the presented serial
 * (1 is the legacy bare advance); a DMA restart clears everything and
 * resets the serial counter. */
struct fw {
	uint16_t size[SLOTS];
	uint16_t serial[SLOTS];
	uint16_t read, write, backlog, frame_serial;
};

static void fw_restart(struct fw *f)
{
	memset(f, 0, sizeof(*f));
}

static uint16_t fw_next_serial(struct fw *f)
{
	f->frame_serial++;
	if (f->frame_serial == 0 || f->frame_serial == 1)
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
	if (f->backlog == 0 || acked == 0)
		return;
	if (acked != 1 && acked != f->serial[f->read])
		return;
	fw_clear(f, f->read);
	f->read = (f->read + 1) % SLOTS;
	f->backlog--;
	if (f->backlog == 0)
		fw_clear(f, f->read);
}

struct framer {
	struct zznet_rx_state state;
	unsigned delivered, overruns, bad_data;
};

/* One frame_proc wake-up. Firmware re-raises the IRQ as soon as the framer
 * re-arms it while backlog > 0, so a WAIT with frames pending repeats until
 * the spin bound: that is the issue #127 livelock. Returns 0 when the
 * framer legitimately sleeps on an empty backlog, -1 on livelock. */
static int framer_run(struct framer *fr, struct fw *f)
{
	int spins;

	for (spins = 0; spins < 4 * SLOTS; spins++) {
		uint16_t size = f->size[f->read];
		uint16_t serial = f->serial[f->read];
		struct zznet_rx_decision d =
			zznet_rx_classify(&fr->state, size, serial);

		fr->overruns += d.overruns;
		fr->bad_data += d.bad_data;
		if (d.action == ZZNET_RX_WAIT) {
			if (f->backlog == 0)
				return 0;
			continue;
		}
		if (d.action == ZZNET_RX_DELIVER)
			fr->delivered++;
		fw_ack(f, serial);
	}
	return -1;
}

static int test_restart_after_one_frame_delivers_next(void)
{
	struct fw f;
	struct framer fr;

	memset(&fr, 0, sizeof(fr));
	zznet_rx_reset(&fr.state);
	fw_restart(&f);

	fw_receive(&f);                 /* serial 2 */
	CHECK(framer_run(&fr, &f) == 0);
	CHECK(fr.delivered == 1);

	fw_restart(&f);                 /* TX-timeout recovery */
	fw_receive(&f);                 /* serial 2 again */
	CHECK(framer_run(&fr, &f) == 0);
	CHECK(fr.delivered == 2);
	CHECK(f.backlog == 0);
	return EXIT_SUCCESS;
}

static int test_unacked_frame_is_not_redelivered(void)
{
	struct zznet_rx_state s;
	struct zznet_rx_decision d;

	zznet_rx_reset(&s);
	d = zznet_rx_classify(&s, FRAME, 7);
	CHECK(d.action == ZZNET_RX_DELIVER);
	/* Re-read before the ack moved the firmware cursor. */
	d = zznet_rx_classify(&s, FRAME, 7);
	CHECK(d.action == ZZNET_RX_WAIT);
	CHECK(!d.empty);
	return EXIT_SUCCESS;
}

static int test_gap_spans_empty_drain_boundary(void)
{
	struct fw f;
	struct framer fr;

	memset(&fr, 0, sizeof(fr));
	zznet_rx_reset(&fr.state);
	fw_restart(&f);

	fw_receive(&f);                 /* serial 2 */
	CHECK(framer_run(&fr, &f) == 0); /* drains, then reads an empty slot */
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

	memset(&fr, 0, sizeof(fr));
	zznet_rx_reset(&fr.state);
	fw_restart(&f);
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
	if (test_restart_after_one_frame_delivers_next() != EXIT_SUCCESS)
		return EXIT_FAILURE;
	if (test_unacked_frame_is_not_redelivered() != EXIT_SUCCESS)
		return EXIT_FAILURE;
	if (test_gap_spans_empty_drain_boundary() != EXIT_SUCCESS)
		return EXIT_FAILURE;
	if (test_serial_wrap_is_not_an_overrun() != EXIT_SUCCESS)
		return EXIT_FAILURE;
	return EXIT_SUCCESS;
}
