/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Reader selection for one received frame. No Amiga headers: frame_proc
 * walks the read list and removes the requests, this decides which of them
 * get the frame.
 *
 * Every program that has the device open gets its own copy of a frame of a
 * type it reads, one request per opener. An opener is identified by the
 * BufferManagement pointer DevOpen allocates once per OpenDevice. At most
 * `limit` requests are taken for one frame: frame_proc passes the open
 * count, capped at ZZNET_MAX_FRAME_READERS (the size of its array).
 */
#ifndef ZZNET_READERS_H
#define ZZNET_READERS_H

/* Offer one matching request, by its opener, to the frame's readers.
 * Returns 1 when the request is taken: its opener has no request among
 * owners[0..*count) yet and fewer than `limit` are taken. The opener is
 * then appended and *count raised. Returns 0 otherwise, and the request
 * stays queued for a later frame. */
static inline int zznet_reader_take(const void **owners, unsigned *count,
                                    unsigned limit, const void *owner)
{
	unsigned i;

	if (*count >= limit)
		return 0;
	for (i = 0; i < *count; i++) {
		if (owners[i] == owner)
			return 0;
	}
	owners[(*count)++] = owner;
	return 1;
}

#endif /* ZZNET_READERS_H */
