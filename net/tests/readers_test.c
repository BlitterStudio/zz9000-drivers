/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Host coverage for the per-opener reader selection frame_proc uses for
 * each received frame (readers.h). Build/run:
 * make -C net/tests test
 */

#include <stdio.h>
#include <stdlib.h>

#include "readers.h"

#define CHECK(expr) do { \
	if (!(expr)) { \
		fprintf(stderr, "%s:%d: check failed: %s\n", \
		        __FILE__, __LINE__, #expr); \
		return EXIT_FAILURE; \
	} \
} while (0)

#define MAX_READERS 8

/* Openers, by address only, as BufferManagement pointers are. */
static int bm[MAX_READERS + 2];

/* A queued request: its opener. */
struct req {
	const void *owner;
};

/* Run the selection over a queue the way take_readers walks the read
 * list: stop at the limit, take a request when the helper says so.
 * taken[i] is set for each request that gets the frame. */
static unsigned select_readers(const struct req *q, unsigned n,
                               unsigned limit, int *taken)
{
	const void *owners[MAX_READERS];
	unsigned count = 0, i;

	for (i = 0; i < n; i++)
		taken[i] = 0;
	for (i = 0; i < n && count < limit; i++)
		taken[i] = zznet_reader_take(owners, &count, limit, q[i].owner);
	return count;
}

static int single_opener_takes_first_request_only(void)
{
	struct req q[] = { { &bm[0] }, { &bm[0] }, { &bm[0] } };
	int taken[3];

	/* Open count 1: one request, the first, as before per-opener reads. */
	CHECK(select_readers(q, 3, 1, taken) == 1);
	CHECK(taken[0] && !taken[1] && !taken[2]);
	/* A larger limit still serves one opener once. */
	CHECK(select_readers(q, 3, MAX_READERS, taken) == 1);
	CHECK(taken[0] && !taken[1] && !taken[2]);
	return EXIT_SUCCESS;
}

static int two_openers_each_get_one_copy(void)
{
	/* A TCP/IP stack and an emulator, interleaved in the queue. */
	struct req q[] = { { &bm[0] }, { &bm[0] }, { &bm[1] }, { &bm[0] }, { &bm[1] } };
	int taken[5];

	CHECK(select_readers(q, 5, 2, taken) == 2);
	CHECK(taken[0] && !taken[1] && taken[2] && !taken[3] && !taken[4]);
	return EXIT_SUCCESS;
}

static int open_count_above_readers_is_an_upper_bound(void)
{
	/* ZZNetStats has the device open but never reads: open count 3,
	 * two readers. Both readers still get the frame. */
	struct req q[] = { { &bm[1] }, { &bm[1] }, { &bm[0] } };
	int taken[3];

	CHECK(select_readers(q, 3, 3, taken) == 2);
	CHECK(taken[0] && !taken[1] && taken[2]);
	return EXIT_SUCCESS;
}

static int cap_stops_at_max_readers(void)
{
	struct req q[MAX_READERS + 2];
	int taken[MAX_READERS + 2];
	unsigned i;

	for (i = 0; i < MAX_READERS + 2; i++)
		q[i].owner = &bm[i];
	CHECK(select_readers(q, MAX_READERS + 2, MAX_READERS, taken) == MAX_READERS);
	for (i = 0; i < MAX_READERS; i++)
		CHECK(taken[i]);
	CHECK(!taken[MAX_READERS] && !taken[MAX_READERS + 1]);
	return EXIT_SUCCESS;
}

static int empty_queue_takes_nothing(void)
{
	int taken[1];

	CHECK(select_readers(NULL, 0, MAX_READERS, taken) == 0);
	return EXIT_SUCCESS;
}

int main(void)
{
	if (single_opener_takes_first_request_only() ||
	    two_openers_each_get_one_copy() ||
	    open_count_above_readers_is_an_upper_bound() ||
	    cap_stops_at_max_readers() ||
	    empty_queue_takes_nothing())
		return EXIT_FAILURE;
	puts("readers_test: ok");
	return EXIT_SUCCESS;
}
