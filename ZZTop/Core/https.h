/*
 * ZZTop minimal HTTPS GET client over bsdsocket.library and AmiSSL.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * One request at a time, TLS 1.2 or later with certificate and host-name
 * verification against AmiSSL's certificate store, up to five HTTPS
 * redirects, Content-Length, chunked and close-delimited bodies. The
 * socket is non-blocking so the caller's keep_running hook runs at least
 * every 100 ms; it services the window and can cancel the transfer. DOS
 * volume requesters are off for the calling process while it runs.
 */
#ifndef ZZTOP_HTTPS_H
#define ZZTOP_HTTPS_H

#include <exec/types.h>

struct https_sink {
	/* The final response's Content-Length (-1 when unknown), before any
	 * body bytes. Nonzero aborts. May be NULL. */
	int (*begin)(void *ctx, LONG total);
	/* Body bytes: 0 continues, HTTPS_SINK_DONE stops successfully,
	 * anything else aborts with the sink's own error. */
	int (*data)(void *ctx, const UBYTE *data, ULONG len);
	/* 0 cancels. May be NULL. */
	int (*keep_running)(void *ctx);
	void *ctx;
};

#define HTTPS_SINK_DONE 1

enum https_result {
	HTTPS_OK,          /* whole body delivered */
	HTTPS_STOPPED,     /* the sink returned HTTPS_SINK_DONE */
	HTTPS_CANCELLED,   /* keep_running returned 0 */
	HTTPS_SINK_FAILED, /* begin or data aborted; the sink has the reason */
	HTTPS_HTTP_STATUS, /* final status was not 200; see *status */
	HTTPS_FAILED       /* network, TLS or protocol error; see err */
};

/* GET `url` (https:// only) with the given Accept header. `status`
 * receives the final HTTP status (0 when none arrived) and `err` a
 * one-line reason for HTTPS_FAILED. */
enum https_result https_get(const char *url, const char *accept,
	const struct https_sink *sink, int *status, char *err, ULONG err_size);

#endif /* ZZTOP_HTTPS_H */
