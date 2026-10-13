/*
 * ZZTop native-screen capture framing editor.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ZZTOP_ALIGN_H
#define ZZTOP_ALIGN_H

#include <exec/types.h>
#include "zz_vcap_client.h"

/* Runs the native-screen crop editor. Returns TRUE only after Enter accepts
 * a fully acknowledged Custom crop. Escape restores the entry control. */
BOOL align_picture_run(const struct zz_vcap_io *io,
	const struct zz_vcap_snapshot *entry,
	struct zz_vcap_control *accepted, char *message, UWORD message_size);

#endif /* ZZTOP_ALIGN_H */
