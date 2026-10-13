/*
 * ZZTop page registry: the order here is the page list order and must
 * match enum ui_page_id.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ui.h"

extern struct ui_page page_status, page_display, page_capture, page_audio,
	page_tuning, page_system, page_firmware, page_diag;

struct ui_page *ui_pages[PAGE_COUNT] = {
	&page_status,     /* PAGE_STATUS */
	&page_display,    /* PAGE_DISPLAY */
	&page_capture,    /* PAGE_CAPTURE  (advanced, under Display) */
	&page_audio,      /* PAGE_AUDIO */
	&page_tuning,     /* PAGE_TUNING   (advanced, under Audio) */
	&page_system,     /* PAGE_SYSTEM */
	&page_firmware,   /* PAGE_FIRMWARE */
	&page_diag,       /* PAGE_DIAG     (advanced) */
};
