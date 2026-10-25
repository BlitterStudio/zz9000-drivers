/*
 * Firmware module status model for the ZZTop Firmware page.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ZZTOP_MODULES_H
#define ZZTOP_MODULES_H

#include <exec/types.h>

#define MODULE_STATUS_MAX 16

struct module_status {
	ULONG id;
	ULONG version;
	ULONG abi_version;
	ULONG load_address;
	ULONG image_size;
	ULONG bss_size;
	ULONG state;
	char name[17];
};

struct module_list {
	UWORD count;
	BOOL available;
	struct module_status entries[MODULE_STATUS_MAX];
};

/* Refresh the fixed firmware manifest. FALSE leaves a diagnostic available
 * through modules_status_text(); a manifest longer than MODULE_STATUS_MAX is
 * rejected rather than presenting a partial list. */
BOOL modules_refresh(void);
const struct module_list *modules_current(void);
const char *modules_state_text(ULONG state);
const char *modules_status_text(void);

#endif /* ZZTOP_MODULES_H */
