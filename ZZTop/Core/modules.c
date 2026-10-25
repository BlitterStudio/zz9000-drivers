/*
 * Firmware module status model for the ZZTop Firmware page.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <exec/types.h>
#include <proto/zz9k.h>
#include <stdio.h>
#include <string.h>

#include "board.h"
#include "modules.h"
#include "zz9k/abi.h"
#include "zz9k/request.h"

static struct module_list modules;
static char status_text[80];

static void modules_set_status(const char *text)
{
	snprintf(status_text, sizeof(status_text), "%s", text);
}

const char *modules_state_text(ULONG state)
{
	switch (state) {
	case ZZ9K_MODULE_LOADED: return "loaded";
	case ZZ9K_MODULE_MISSING: return "missing";
	case ZZ9K_MODULE_REJECTED_ABI: return "rejected ABI";
	case ZZ9K_MODULE_REJECTED_HASH: return "rejected hash";
	case ZZ9K_MODULE_REJECTED_SLOT: return "rejected slot";
	default: return "unknown";
	}
}

const struct module_list *modules_current(void)
{
	return &modules;
}

const char *modules_status_text(void)
{
	return status_text;
}

BOOL modules_refresh(void)
{
	UWORD index;

	memset(&modules, 0, sizeof(modules));
	status_text[0] = '\0';
	if ((board.fw_caps & ZZ_FW_CAP_FIRMWARE_MODULES) == 0)
		return TRUE;
	if (!board.zz9k_library || !ZZ9KBase) {
		modules_set_status("Module status needs zz9k.library");
		return FALSE;
	}
	for (index = 0; index <= MODULE_STATUS_MAX; index++) {
		ZZ9KModuleStatusRequestPayload request_payload;
		ZZ9KModuleStatusPayload payload;
		ZZ9KRequest request;
		ZZ9KMailboxEntry reply;
		int status;

		memset(&request_payload, 0, sizeof(request_payload));
		zz9k_put_be32(request_payload.index, index);
		zz9k_request_init(&request, ZZ9K_OP_MODULE_STATUS);
		request.entry.payload_len = sizeof(request_payload);
		memcpy(request.entry.payload.inline_data, &request_payload,
			sizeof(request_payload));
		status = ZZ9KCall(&request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
		if (status == ZZ9K_STATUS_NOT_FOUND) {
			modules.available = TRUE;
			return TRUE;
		}
		if (status != ZZ9K_STATUS_OK ||
			reply.payload_len != sizeof(payload)) {
			modules_set_status("Could not read module status");
			return FALSE;
		}
		if (index == MODULE_STATUS_MAX) {
			modules_set_status("Module manifest is too large");
			return FALSE;
		}
		memcpy(&payload, reply.payload.inline_data, sizeof(payload));
		modules.entries[index].id = zz9k_get_be32(payload.id);
		modules.entries[index].version = zz9k_get_be32(payload.version);
		modules.entries[index].abi_version = zz9k_get_be32(payload.abi_version);
		modules.entries[index].load_address = zz9k_get_be32(payload.load_address);
		modules.entries[index].image_size = zz9k_get_be32(payload.image_size);
		modules.entries[index].bss_size = zz9k_get_be32(payload.bss_size);
		modules.entries[index].state = zz9k_get_be32(payload.state);
		memcpy(modules.entries[index].name, payload.name,
			sizeof(payload.name));
		modules.entries[index].name[sizeof(payload.name)] = '\0';
		modules.count++;
	}
	/* The index after the last representable entry answered unexpectedly. */
	modules_set_status("Module manifest is too large");
	return FALSE;
}
