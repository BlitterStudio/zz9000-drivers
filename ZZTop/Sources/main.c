/*
 * ZZTop - the ZZ9000 control panel.
 * Copyright (C) 2016-2026, Lucie L. Hartmann <lucie@mntre.com>
 *                          MNT Research GmbH, Berlin
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 *   (ZZTop V2 and all later versions)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <exec/types.h>
#include <intuition/intuition.h>
#include <proto/exec.h>
#include <proto/intuition.h>

#include <stdio.h>

#include "zztop.h"
#include "audio.h"
#include "board.h"
#include "settings.h"
#include "ui.h"
#include "video.h"

#include "zz9k/library_vectors.h"
#include "zz9k/request.h"
#include <proto/zz9k.h>

static const char version[] __attribute__((used)) =
	"$VER: " ZZTOP_NAME " " ZZTOP_RELEASE " (" ZZTOP_DATE ")\r\n";

/* Initialised so they are strong definitions (see ui.c). */
struct IntuitionBase *IntuitionBase = NULL;
struct GfxBase *GfxBase = NULL;
struct Library *GadToolsBase = NULL;
struct Library *AslBase = NULL;
struct Library *UtilityBase = NULL;
struct Library *ZZ9KBase = NULL;

/* A Workbench launch has no console: report startup failures in a
 * requester (intuition is the one library this needs). */
static void fail(const char *text)
{
	struct EasyStruct es;
	ULONG args[1];

	if (!IntuitionBase) return;
	es.es_StructSize = sizeof(es);
	es.es_Flags = 0;
	es.es_Title = (UBYTE *)ZZTOP_NAME;
	es.es_TextFormat = (UBYTE *)"%s";
	es.es_GadgetFormat = (UBYTE *)"OK";
	args[0] = (ULONG)text;
	EasyRequestArgs(NULL, &es, NULL, args);
}

static void zz9k_library_open(void)
{
	ZZ9KCaps caps;

	ZZ9KBase = OpenLibrary((CONST_STRPTR)"zz9k.library", 0);
	if (!ZZ9KBase) return;
	if (ZZ9KQueryCaps(&caps) == ZZ9K_STATUS_OK) {
		board.zz9k_library = TRUE;
		board.zz9k_caps = caps.capability_bits;
	}
}

int main(void)
{
	static char missing[200], message[320];

	IntuitionBase = (struct IntuitionBase *)OpenLibrary(
		(CONST_STRPTR)"intuition.library", 39);
	if (!IntuitionBase) return 20;
	GfxBase = (struct GfxBase *)OpenLibrary(
		(CONST_STRPTR)"graphics.library", 39);
	GadToolsBase = OpenLibrary((CONST_STRPTR)"gadtools.library", 39);
	UtilityBase = OpenLibrary((CONST_STRPTR)"utility.library", 39);
	if (!GfxBase || !GadToolsBase || !UtilityBase) {
		fail("ZZTop needs AmigaOS 3.0 or newer.");
		goto out;
	}
	if (!ui_open_classes(missing, sizeof(missing))) {
		snprintf(message, sizeof(message),
			"ZZTop needs the ReAction classes of AmigaOS 3.2, "
			"or AmigaOS 3.5/3.9 (or ClassAct on 3.1).\n\n"
			"Missing: %s", missing);
		fail(message);
		goto out;
	}
	if (!board_find()) {
		fail("No ZZ9000 found.\n\nZZTop controls the ZZ9000 Zorro card; "
			"check that it is seated and configured.");
		goto out;
	}

	/* asl.library is needed only for the firmware file requester. */
	AslBase = OpenLibrary((CONST_STRPTR)"asl.library", 39);
	zz9k_library_open();

	settings_load();
	video_init();
	audio_init();
	app_init();

	if (ui_create())
		ui_run();
	else
		fail("ZZTop could not open its window.");
	ui_destroy();
	audio_shutdown();

out:
	ui_close_classes();
	if (ZZ9KBase) CloseLibrary(ZZ9KBase);
	if (AslBase) CloseLibrary(AslBase);
	if (UtilityBase) CloseLibrary(UtilityBase);
	if (GadToolsBase) CloseLibrary(GadToolsBase);
	if (GfxBase) CloseLibrary((struct Library *)GfxBase);
	CloseLibrary((struct Library *)IntuitionBase);
	return 0;
}
