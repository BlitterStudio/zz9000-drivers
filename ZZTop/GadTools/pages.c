/*
 * ZZTop GadTools page registry.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "gt.h"

extern struct gt_page gt_page_status;
extern struct gt_page gt_page_display;
extern struct gt_page gt_page_capture;
extern struct gt_page gt_page_audio;
extern struct gt_page gt_page_tuning;
extern struct gt_page gt_page_system;
extern struct gt_page gt_page_firmware;
extern struct gt_page gt_page_diag;

struct gt_page *gt_pages[GT_PAGE_COUNT] = {
	&gt_page_status,
	&gt_page_display,
	&gt_page_capture,
	&gt_page_audio,
	&gt_page_tuning,
	&gt_page_system,
	&gt_page_firmware,
	&gt_page_diag
};
