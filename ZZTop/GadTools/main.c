/*
 * ZZTop - the ZZ9000 control panel.
 * Copyright (C) 2016-2026, Lucie L. Hartmann <lucie@mntre.com>
 *                          MNT Research GmbH, Berlin
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <exec/types.h>
#include <intuition/intuition.h>
#include <proto/exec.h>
#include <proto/intuition.h>

#include <stdio.h>

#include "amiga_stack.h"
#include "audio.h"
#include "board.h"
#include "settings.h"
#include "video.h"
#include "zztop.h"
#include "zz9k/library_vectors.h"
#include "zz9k/request.h"
#include <proto/zz9k.h>

#include "gt.h"

static char startup_message[240];
static const char version[] __attribute__((used)) =
	"$VER: ZZTop " ZZTOP_RELEASE " (" ZZTOP_DATE ")\r\n";

struct IntuitionBase *IntuitionBase = NULL;
struct GfxBase *GfxBase = NULL;
struct Library *GadToolsBase = NULL;
struct Library *AslBase = NULL;
struct Library *UtilityBase = NULL;
struct Library *ZZ9KBase = NULL;

/* Present an early startup failure to a Workbench launch. */
static void fail(const char *text)
{
	struct EasyStruct es;
	ULONG args[1];

	if (!IntuitionBase)
		return;
	es.es_StructSize = sizeof(es);
	es.es_Flags = 0;
	es.es_Title = (STRPTR)"ZZTop";
	es.es_TextFormat = (STRPTR)"%s";
	es.es_GadgetFormat = (STRPTR)"OK";
	args[0] = (ULONG)text;
	EasyRequestArgs(NULL, &es, NULL, args);
}

/* Open the optional zz9k.library and cache its advertised capability bits. */
static void zz9k_library_open(void)
{
	ZZ9KCaps caps;

	ZZ9KBase = OpenLibrary((CONST_STRPTR)"zz9k.library", 0);
	if (!ZZ9KBase)
		return;
	if (ZZ9KQueryCaps(&caps) == ZZ9K_STATUS_OK) {
		board.zz9k_library = TRUE;
		board.zz9k_caps = caps.capability_bits;
	}
}

/* Initialise the shared core then run the small-screen GadTools frontend. */
static int zztop_main(void)
{
	IntuitionBase = (struct IntuitionBase *)OpenLibrary(
		(CONST_STRPTR)"intuition.library", 37);
	if (!IntuitionBase)
		return 20;
	GfxBase = (struct GfxBase *)OpenLibrary((CONST_STRPTR)"graphics.library", 37);
	/* V39: every page reads its gadgets with GT_GetGadgetAttrs. */
	GadToolsBase = OpenLibrary((CONST_STRPTR)"gadtools.library", 39);
	UtilityBase = OpenLibrary((CONST_STRPTR)"utility.library", 37);
	if (!GfxBase || !GadToolsBase || !UtilityBase) {
		fail("ZZTop needs AmigaOS 3.0 or newer.");
		goto out;
	}
	if (!board_find()) {
		fail("No ZZ9000 found.\n\nZZTop controls the ZZ9000 Zorro card;\n"
			"check that it is seated and configured.");
		goto out;
	}
	AslBase = OpenLibrary((CONST_STRPTR)"asl.library", 37);
	zz9k_library_open();
	settings_load();
	video_init();
	audio_init();
	app_init();
	if (gt_create())
		gt_run();
	else {
		snprintf(startup_message, sizeof(startup_message), "ZZTop could not open its window.\n\n%s",
			gt_create_error());
		fail(startup_message);
	}
	gt_destroy();
	audio_shutdown();
out:
	if (ZZ9KBase)
		CloseLibrary(ZZ9KBase);
	if (AslBase)
		CloseLibrary(AslBase);
	if (UtilityBase)
		CloseLibrary(UtilityBase);
	if (GadToolsBase)
		CloseLibrary(GadToolsBase);
	if (GfxBase)
		CloseLibrary((struct Library *)GfxBase);
	CloseLibrary((struct Library *)IntuitionBase);
	return 0;
}

static int zztop_entry(void *ctx)
{
	(void)ctx;
	return zztop_main();
}

int main(void)
{
	return amiga_stack_run("ZZTop", 32768UL, zztop_entry, NULL);
}
