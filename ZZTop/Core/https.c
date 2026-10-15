/*
 * ZZTop minimal HTTPS GET client over bsdsocket.library and AmiSSL.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <exec/types.h>
#include <utility/tagitem.h>
#include <dos/dosextens.h>
#include <proto/exec.h>

/* AmiSSL's call macros come first: its OpenSSL headers then skip the
 * plain prototypes. */
#include <proto/amissl.h>
#include <proto/amisslmaster.h>
#include <libraries/amisslmaster.h>
#include <libraries/amissl.h>
#include <amissl/amissl.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509.h>

#include <proto/socket.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/filio.h>
#include <netinet/in.h>
#include <netdb.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "fw_release.h"
#include "https.h"
#include "zztop.h"

/* proto/amissl.h and proto/socket.h refer to these bases. */
struct Library *AmiSSLMasterBase;
struct Library *AmiSSLBase;
struct Library *AmiSSLExtBase;
struct Library *SocketBase;

#define HTTPS_MAX_REDIRECTS 5
#define HTTPS_HEAD_MAX      8192
#define HTTPS_IO_BYTES      8192
#define HTTPS_POLL_MICROS   100000UL
#define HTTPS_IDLE_POLLS    300      /* 30 s without progress */

struct conn {
	LONG fd;
	SSL_CTX *ctx;
	SSL *ssl;
	const struct https_sink *sink;
	char *err;
	ULONG err_size;
	BOOL cancelled;
};

static UBYTE io_buf[HTTPS_IO_BYTES];
static char head_buf[HTTPS_HEAD_MAX];

static void set_err(struct conn *c, const char *fmt, const char *arg)
{
	snprintf(c->err, c->err_size, fmt, arg);
}

static BOOL keep_running(struct conn *c)
{
	if (c->sink->keep_running && !c->sink->keep_running(c->sink->ctx))
		c->cancelled = TRUE;
	return !c->cancelled;
}

/* Wait until the socket is readable or writable, servicing the caller
 * between polls. Returns 1 ready, 0 cancelled or timed out (err set for
 * the timeout). */
static int wait_socket(struct conn *c, BOOL for_write)
{
	int polls;

	for (polls = 0; polls < HTTPS_IDLE_POLLS; polls++) {
		fd_set fds;
		struct __timeval tv;
		LONG n;

		if (!keep_running(c))
			return 0;
		FD_ZERO(&fds);
		FD_SET(c->fd, &fds);
		tv.tv_sec = 0;
		tv.tv_usec = HTTPS_POLL_MICROS;
		n = WaitSelect(c->fd + 1, for_write ? NULL : &fds,
			for_write ? &fds : NULL, NULL, &tv, NULL);
		if (n > 0)
			return 1;
		if (n < 0) {
			set_err(c, "%s", "Network wait failed");
			return 0;
		}
	}
	set_err(c, "%s", "The server stopped responding");
	return 0;
}

static void ssl_err(struct conn *c, const char *what)
{
	unsigned long e = ERR_get_error();
	long verify = c->ssl ? SSL_get_verify_result(c->ssl) : X509_V_OK;

	if (verify != X509_V_OK)
		snprintf(c->err, c->err_size, "%s: certificate %s", what,
			X509_verify_cert_error_string(verify));
	else if (e)
		snprintf(c->err, c->err_size, "%s: %s", what,
			ERR_reason_error_string(e) ? ERR_reason_error_string(e) :
			"TLS error");
	else
		snprintf(c->err, c->err_size, "%s", what);
}

/* Drive a non-blocking TLS operation that reported `rc`. Returns 1 to
 * retry after waiting, 0 on a real failure. */
static int ssl_retry(struct conn *c, int rc, const char *what)
{
	int e = SSL_get_error(c->ssl, rc);

	if (e == SSL_ERROR_WANT_READ)
		return wait_socket(c, FALSE);
	if (e == SSL_ERROR_WANT_WRITE)
		return wait_socket(c, TRUE);
	if (!c->cancelled)
		ssl_err(c, what);
	return 0;
}

static void conn_close(struct conn *c)
{
	if (c->ssl) {
		SSL_free(c->ssl);
		c->ssl = NULL;
	}
	if (c->ctx) {
		SSL_CTX_free(c->ctx);
		c->ctx = NULL;
	}
	if (c->fd >= 0) {
		CloseSocket(c->fd);
		c->fd = -1;
	}
}

static int conn_open(struct conn *c, const char *host)
{
	struct hostent *he;
	struct sockaddr_in sa;
	LONG one = 1;
	int rc;

	if (!keep_running(c))
		return 0;
	he = gethostbyname((STRPTR)host);
	if (!he || he->h_addrtype != AF_INET || !he->h_addr_list[0]) {
		set_err(c, "Cannot resolve %s", host);
		return 0;
	}
	memset(&sa, 0, sizeof(sa));
	sa.sin_len = sizeof(sa);
	sa.sin_family = AF_INET;
	sa.sin_port = htons(443);
	memcpy(&sa.sin_addr, he->h_addr_list[0], sizeof(sa.sin_addr));

	c->fd = socket(AF_INET, SOCK_STREAM, 0);
	if (c->fd < 0) {
		set_err(c, "%s", "Cannot create a network socket");
		return 0;
	}
	IoctlSocket(c->fd, FIONBIO, (APTR)&one);
	if (connect(c->fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		LONG soerr = 0;
		socklen_t len = sizeof(soerr);

		if (Errno() != EINPROGRESS) {
			set_err(c, "Cannot connect to %s", host);
			return 0;
		}
		if (!wait_socket(c, TRUE))
			return 0;
		if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &soerr, &len) < 0 ||
		    soerr) {
			set_err(c, "Cannot connect to %s", host);
			return 0;
		}
	}

	c->ctx = SSL_CTX_new(TLS_client_method());
	if (!c->ctx) {
		ssl_err(c, "TLS setup failed");
		return 0;
	}
	SSL_CTX_set_min_proto_version(c->ctx, TLS1_2_VERSION);
	SSL_CTX_set_verify(c->ctx, SSL_VERIFY_PEER, NULL);
	if (SSL_CTX_set_default_verify_paths(c->ctx) != 1) {
		ssl_err(c, "Cannot load AmiSSL certificates");
		return 0;
	}
	c->ssl = SSL_new(c->ctx);
	if (!c->ssl || SSL_set_fd(c->ssl, c->fd) != 1 ||
	    SSL_set_tlsext_host_name(c->ssl, host) != 1 ||
	    SSL_set1_host(c->ssl, host) != 1) {
		ssl_err(c, "TLS setup failed");
		return 0;
	}
	while ((rc = SSL_connect(c->ssl)) != 1)
		if (!ssl_retry(c, rc, "Secure connection failed"))
			return 0;
	return 1;
}

static int conn_write(struct conn *c, const char *data, int len)
{
	while (len > 0) {
		int rc = SSL_write(c->ssl, data, len);
		if (rc > 0) {
			data += rc;
			len -= rc;
		} else if (!ssl_retry(c, rc, "Sending the request failed")) {
			return 0;
		}
	}
	return 1;
}

/* Read some bytes: >0 count, 0 orderly end of stream, -1 failure. */
static int conn_read(struct conn *c, UBYTE *buf, int size)
{
	for (;;) {
		int rc = SSL_read(c->ssl, buf, size);
		int e;

		if (rc > 0)
			return rc;
		e = SSL_get_error(c->ssl, rc);
		if (e == SSL_ERROR_ZERO_RETURN)
			return 0;
		/* Servers that close without close_notify after a
		 * delimited body: treat as end, the framing decides. */
		if (e == SSL_ERROR_SYSCALL && ERR_peek_error() == 0)
			return 0;
		if (!ssl_retry(c, rc, "Receiving data failed"))
			return -1;
		if (!keep_running(c))
			return -1;
	}
}

/* Body delivery state shared with the chunked decoder's sink. */
struct body {
	struct conn *c;
	int sink_rc;
};

static int body_sink(void *ctx, const uint8_t *data, size_t len)
{
	struct body *b = ctx;

	b->sink_rc = b->c->sink->data(b->c->sink->ctx, data, (ULONG)len);
	return b->sink_rc != 0;
}

/* One request on an open connection. On a redirect `next_url` receives
 * the target and the result is HTTPS_OK with *status set to 3xx. */
static enum https_result request(struct conn *c, const char *host,
	const char *path, const char *accept, int *status,
	char *next_url, ULONG next_size)
{
	struct fwrel_http_head head;
	struct fwrel_chunked chunked;
	struct body body;
	ULONG have = 0;
	char *end = NULL;
	ULONG body_start, n;
	ULONG remaining;
	int len;

	len = snprintf(head_buf, sizeof(head_buf),
		"GET %s HTTP/1.1\r\nHost: %s\r\n"
		"User-Agent: ZZTop/%s (AmigaOS)\r\nAccept: %s\r\n"
		"Connection: close\r\n\r\n", path, host, ZZTOP_RELEASE, accept);
	if (len <= 0 || len >= (int)sizeof(head_buf)) {
		set_err(c, "%s", "Request too long");
		return HTTPS_FAILED;
	}
	if (!conn_write(c, head_buf, len))
		return c->cancelled ? HTTPS_CANCELLED : HTTPS_FAILED;

	/* Response head, up to the blank line. */
	while (!end) {
		int rc;

		if (have >= sizeof(head_buf) - 1) {
			set_err(c, "%s", "Response headers too large");
			return HTTPS_FAILED;
		}
		rc = conn_read(c, (UBYTE *)head_buf + have,
			(int)(sizeof(head_buf) - 1 - have));
		if (rc < 0)
			return c->cancelled ? HTTPS_CANCELLED : HTTPS_FAILED;
		if (rc == 0) {
			set_err(c, "%s", "Connection closed before a response");
			return HTTPS_FAILED;
		}
		have += (ULONG)rc;
		head_buf[have] = '\0';
		end = strstr(head_buf, "\r\n\r\n");
	}
	body_start = (ULONG)(end - head_buf) + 4;
	if (!fwrel_http_parse_head(head_buf, (size_t)(end - head_buf), &head)) {
		set_err(c, "%s", "Malformed response from the server");
		return HTTPS_FAILED;
	}
	*status = head.status;
	if (head.status >= 300 && head.status < 400 && head.location[0]) {
		if (head.location[0] == '/')
			snprintf(next_url, next_size, "https://%s%s", host,
				head.location);
		else
			snprintf(next_url, next_size, "%s", head.location);
		return HTTPS_OK;
	}
	if (head.status != 200)
		return HTTPS_HTTP_STATUS;

	if (c->sink->begin && c->sink->begin(c->sink->ctx,
			head.chunked ? -1 : head.content_length))
		return HTTPS_SINK_FAILED;

	body.c = c;
	body.sink_rc = 0;
	fwrel_chunked_init(&chunked);
	remaining = head.content_length >= 0 ? (ULONG)head.content_length : 0;
	n = have - body_start;
	memmove(io_buf, head_buf + body_start, n);
	for (;;) {
		int rc;

		if (n) {
			if (head.chunked) {
				rc = fwrel_chunked_feed(&chunked, io_buf, n,
					body_sink, &body);
				if (body.sink_rc == HTTPS_SINK_DONE)
					return HTTPS_STOPPED;
				if (body.sink_rc)
					return HTTPS_SINK_FAILED;
				if (rc == FWREL_CHUNKED_ERROR) {
					set_err(c, "%s", "Malformed chunked response");
					return HTTPS_FAILED;
				}
				if (rc == FWREL_CHUNKED_DONE)
					return HTTPS_OK;
			} else {
				if (head.content_length >= 0 && n > remaining)
					n = remaining;
				body_sink(&body, io_buf, n);
				if (body.sink_rc == HTTPS_SINK_DONE)
					return HTTPS_STOPPED;
				if (body.sink_rc)
					return HTTPS_SINK_FAILED;
				if (head.content_length >= 0) {
					remaining -= n;
					if (!remaining)
						return HTTPS_OK;
				}
			}
		} else if (head.content_length == 0 && !head.chunked) {
			return HTTPS_OK;
		}
		rc = conn_read(c, io_buf, sizeof(io_buf));
		if (rc < 0)
			return c->cancelled ? HTTPS_CANCELLED : HTTPS_FAILED;
		if (rc == 0) {
			if (!head.chunked && head.content_length < 0)
				return HTTPS_OK;
			set_err(c, "%s", "Connection closed before the end of the data");
			return HTTPS_FAILED;
		}
		n = (ULONG)rc;
	}
}

static BOOL libraries_open(char *err, ULONG err_size)
{
	struct TagItem tags[] = {
		{ AmiSSL_UsesOpenSSLStructs, FALSE },
		{ AmiSSL_GetAmiSSLBase, 0 },
		{ AmiSSL_GetAmiSSLExtBase, 0 },
		{ AmiSSL_SocketBase, 0 },
		{ AmiSSL_ErrNoPtr, 0 },
		{ TAG_DONE, 0 }
	};

	SocketBase = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4);
	if (!SocketBase) {
		snprintf(err, err_size, "%s", "No TCP/IP stack is running");
		return FALSE;
	}
	AmiSSLMasterBase = OpenLibrary((CONST_STRPTR)"amisslmaster.library",
		AMISSLMASTER_MIN_VERSION);
	if (!AmiSSLMasterBase) {
		snprintf(err, err_size, "%s", "AmiSSL 5 is not installed");
		return FALSE;
	}
	tags[1].ti_Data = (ULONG)&AmiSSLBase;
	tags[2].ti_Data = (ULONG)&AmiSSLExtBase;
	tags[3].ti_Data = (ULONG)SocketBase;
	tags[4].ti_Data = (ULONG)&errno;
	/* OpenSSL 3 API: any AmiSSL 5 release provides these calls. */
	if (OpenAmiSSLTagList(AMISSL_V3xx, tags) != 0) {
		AmiSSLBase = NULL;
		snprintf(err, err_size, "%s", "AmiSSL could not be opened");
		return FALSE;
	}
	return TRUE;
}

static void libraries_close(void)
{
	if (AmiSSLBase) {
		CloseAmiSSL();
		AmiSSLBase = NULL;
		AmiSSLExtBase = NULL;
	}
	if (AmiSSLMasterBase) {
		CloseLibrary(AmiSSLMasterBase);
		AmiSSLMasterBase = NULL;
	}
	if (SocketBase) {
		CloseLibrary(SocketBase);
		SocketBase = NULL;
	}
}

static enum https_result get(const char *url, const char *accept,
	const struct https_sink *sink, int *status, char *err, ULONG err_size)
{
	static char current[FWREL_HOST_MAX + FWREL_PATH_MAX + 16];
	static char next[FWREL_HOST_MAX + FWREL_PATH_MAX + 16];
	char host[FWREL_HOST_MAX];
	char path[FWREL_PATH_MAX];
	struct conn c;
	enum https_result result = HTTPS_FAILED;
	int redirects;

	*status = 0;
	err[0] = '\0';
	memset(&c, 0, sizeof(c));
	c.fd = -1;
	c.sink = sink;
	c.err = err;
	c.err_size = err_size;

	if (!libraries_open(err, err_size)) {
		libraries_close();
		return HTTPS_FAILED;
	}
	snprintf(current, sizeof(current), "%s", url);
	for (redirects = 0; redirects <= HTTPS_MAX_REDIRECTS; redirects++) {
		if (!fwrel_split_url(current, host, sizeof(host), path,
				sizeof(path))) {
			set_err(&c, "%s", "Unsupported download address");
			result = HTTPS_FAILED;
			break;
		}
		next[0] = '\0';
		if (!conn_open(&c, host))
			result = c.cancelled ? HTTPS_CANCELLED : HTTPS_FAILED;
		else
			result = request(&c, host, path, accept, status, next,
				sizeof(next));
		conn_close(&c);
		if (result != HTTPS_OK || !next[0])
			break;
		snprintf(current, sizeof(current), "%s", next);
		if (redirects == HTTPS_MAX_REDIRECTS) {
			set_err(&c, "%s", "Too many redirects");
			result = HTTPS_FAILED;
		}
	}
	libraries_close();
	return result;
}

enum https_result https_get(const char *url, const char *accept,
	const struct https_sink *sink, int *status, char *err, ULONG err_size)
{
	struct Process *me = (struct Process *)FindTask(NULL);
	APTR window = me->pr_WindowPtr;
	enum https_result result;

	/* AmiSSL opens usergroup.library and falls back to
	 * AmiTCP:Libs/usergroup.library. On a stack without that assign
	 * AmigaDOS would stop ZZTop on an "insert volume AmiTCP" requester
	 * (seen with AmiNetXDuo under Amiberry); nothing here needs one. */
	me->pr_WindowPtr = (APTR)-1;
	result = get(url, accept, sink, status, err, err_size);
	me->pr_WindowPtr = window;
	return result;
}
