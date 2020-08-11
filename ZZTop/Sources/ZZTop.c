/*
 * MNT ZZ9000 Amiga Graphics Card Diagnostics (ZZTop)
 * Copyright (C) 2016-2020, Lukas F. Hartmann <lukas@mntre.com>
 *                          MNT Research GmbH, Berlin
 *                          https://mntre.com
 *
 * More Info: https://mntre.com/zz9000
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * GNU General Public License v3.0 or later
 *
 * https://spdx.org/licenses/GPL-3.0-or-later.html
 */

#include <exec/types.h>
#include <intuition/intuition.h>
#include <intuition/gadgetclass.h>
#include <libraries/gadtools.h>

#include <clib/exec_protos.h>
#include <clib/graphics_protos.h>
#include <clib/intuition_protos.h>
#include <clib/gadtools_protos.h>
#include <clib/expansion_protos.h>

#include <clib/timer_protos.h>

#include <stdio.h>

#include "zz9000.h"

struct Gadget *gads[8];

#define MYGAD_ZORROVER	(0)
#define MYGAD_FWVER		(1)
#define MYGAD_TEMP		(2)
#define MYGAD_VAUX		(3)
#define MYGAD_VINT		(4)
//#define MYGAD_BTN_REFRESH	(5)
#define MYGAD_BTN_TEST		(5)

struct TextAttr Topaz80 = { "topaz.font", 8, 0, 0, };

struct Library* IntuitionBase;
struct Library* GfxBase;
struct Library* GadToolsBase;
struct Library* ExpansionBase;

struct ConfigDev* zz_cd;
volatile UBYTE* zz_regs;
int zorro_version = 0;

char txt_buf[64];

struct timerequest * timerio;
struct MsgPort *timerport;
struct Library *TimerBase;

void errorMessage(STRPTR error)
{
	if (error) printf("Error: %s\n", error);
}

UWORD zz_get_reg(ULONG offset)
{
	return *((volatile UWORD*)(zz_regs+offset));
}

float zz_get_temperature(void)
{
	float temp = (float)(zz_get_reg(REG_ZZ_TEMPERATURE));
	return temp/10.0;
}
float zz_get_voltage_aux(void)
{
	float vaux = (float)(zz_get_reg(REG_ZZ_VOLTAGE_AUX));
	return vaux/100.0;
}
float zz_get_voltage_int(void)
{
	float vint = (float)(zz_get_reg(REG_ZZ_VOLTAGE_INT));
	return vint/100.0;
}
float t_old=0;
void refresh_zz_info(struct Window* win)
{
	UWORD fwrev = zz_get_reg(REG_ZZ_FW_VERSION);
	int fwrev_major = fwrev>>8;
	int fwrev_minor = fwrev&0xff;
	float t = zz_get_temperature();
	float vaux=zz_get_voltage_aux();
	float vint=zz_get_voltage_int();

	float t_filt;
	if(t_old==0)
		t_filt=t;
	else
		t_filt=0.1*t+0.9*t_old;
	t_old=t_filt;

	GT_SetGadgetAttrs(gads[MYGAD_ZORROVER], win, NULL, GTIN_Number, zorro_version, TAG_END);

	sprintf(txt_buf, "ZZ9000 %d.%d", fwrev_major, fwrev_minor);
	GT_SetGadgetAttrs(gads[MYGAD_FWVER], win, NULL, GTST_String, txt_buf, TAG_END);

	sprintf(txt_buf, "%.1f", t_filt);
	GT_SetGadgetAttrs(gads[MYGAD_TEMP], win, NULL, GTST_String, txt_buf, TAG_END);

	sprintf(txt_buf, "%.2f", vaux);
	GT_SetGadgetAttrs(gads[MYGAD_VAUX], win, NULL, GTST_String, txt_buf, TAG_END);

	sprintf(txt_buf, "%.2f", vint);
	GT_SetGadgetAttrs(gads[MYGAD_VINT], win, NULL, GTST_String, txt_buf, TAG_END);
}

ULONG zz_perform_memtest(struct Window* win)
{
	volatile uint32_t* bufferl = (volatile uint32_t*)(zz_cd->cd_BoardAddr+0x10000);
	volatile uint16_t* bufferw = (volatile uint16_t*)bufferl;
	uint32_t i = 0;
	uint32_t errors = 0;

	printf("1MB framebuffer write/read test (combined words/longs)...\n");
	for (i=0; i<1024*256; i++) {
		uint32_t v2 = 0;
		uint32_t v = (i%2)?0xaaaa5555:0x33337777;
		uint16_t v4 = 0;
		uint16_t v3 = (i%2)?0xffff:0x0000;

		bufferl[i] = v;
		v2 = bufferl[i];

		if (v!=v2) {
			printf("32-bit mismatch at 0x%p: 0x%lx should be 0x%lx\n",&bufferl[i],v2,v);
			errors++;
		}

		bufferw[i] = v3;
		v4 = bufferw[i];

		if (v3!=v4) {
			printf("16-bit mismatch at 0x%p: 0x%x should be 0x%x\n",&bufferw[i],v4,v3);
			errors++;
		}
	}
	printf("Done. %ld errors.\n", errors);
	return errors;
}

VOID handleGadgetEvent(struct Window *win, struct Gadget *gad, ULONG code)
{
	switch (gad->GadgetID)
	{
//		case MYGAD_BTN_REFRESH: {
//			refresh_zz_info(win);
//			break;
//		}
		case MYGAD_BTN_TEST: {
			zz_perform_memtest(win);
			break;
		}
	}
}

struct Gadget *createAllGadgets(struct Gadget **glistptr, void *vi, UWORD topborder)
{
	struct NewGadget ng;
	struct Gadget *gad;

	gad = CreateContext(glistptr);

	ng.ng_LeftEdge   = 20+70;
	ng.ng_TopEdge    = 90+topborder+20+20;
	ng.ng_Width      = 100;
	ng.ng_Height     = 14;
	ng.ng_GadgetText = "Bus Test";
	ng.ng_TextAttr   = &Topaz80;
	ng.ng_VisualInfo = vi;
	ng.ng_GadgetID   = MYGAD_BTN_TEST;
	ng.ng_Flags      = 0;

//	gads[MYGAD_BTN_REFRESH] = gad = CreateGadget(BUTTON_KIND, gad, &ng,
//                    TAG_END);
//
//	ng.ng_LeftEdge	= 160;
//	ng.ng_GadgetID   = MYGAD_BTN_REFRESH;
//	ng.ng_GadgetText = "Refresh";
//
	gads[MYGAD_BTN_TEST] = gad = CreateGadget(BUTTON_KIND, gad, &ng,
                    TAG_END);

	ng.ng_LeftEdge	= 160;
	ng.ng_TopEdge	= 20+topborder;
	ng.ng_GadgetID	= MYGAD_ZORROVER;
	ng.ng_GadgetText = "Zorro Version";

	gads[MYGAD_ZORROVER] = gad = CreateGadget(INTEGER_KIND, gad, &ng,
                    GTIN_Number, 0,
                    TAG_END);

	ng.ng_TopEdge	= 40+topborder;
	ng.ng_GadgetID	= MYGAD_FWVER;
	ng.ng_GadgetText = "Firmware Version";

	gads[MYGAD_FWVER] = gad = CreateGadget(STRING_KIND, gad, &ng,
                    GTST_String, "",
                    TAG_END);

	ng.ng_TopEdge	= 60+topborder;
	ng.ng_GadgetID	= MYGAD_TEMP;
	ng.ng_GadgetText = "Core °C";

	gads[MYGAD_TEMP] = gad = CreateGadget(STRING_KIND, gad, &ng,
                    GTST_String, "",
                    TAG_END);

	ng.ng_TopEdge	= 80+topborder;
	ng.ng_GadgetID	= MYGAD_VAUX;
	ng.ng_GadgetText = "Aux Voltage V";

	gads[MYGAD_VAUX] = gad = CreateGadget(STRING_KIND, gad, &ng,
                    GTST_String, "",
                    TAG_END);

	ng.ng_TopEdge	= 100+topborder;
	ng.ng_GadgetID	= MYGAD_VINT;
	ng.ng_GadgetText = "Core Voltage V";

	gads[MYGAD_VINT] = gad = CreateGadget(STRING_KIND, gad, &ng,
                    GTST_String, "",
                    TAG_END);

	return(gad);
}

VOID process_window_events(struct Window *mywin)
{
	struct IntuiMessage *imsg;
	ULONG imsgClass;
	UWORD imsgCode;
	struct Gadget *gad;
	BOOL terminated = FALSE;

	if((timerport = CreateMsgPort())) {
		if((timerio=(struct timerequest *)CreateIORequest(timerport, sizeof(struct timerequest)))) {
			if(OpenDevice((STRPTR) TIMERNAME, UNIT_MICROHZ, (struct IORequest *) timerio,0) == 0) {
				TimerBase = (struct Library *)timerio->tr_node.io_Device;
			}
			else {
				DeleteIORequest((struct IORequest *)timerio);
				DeleteMsgPort(timerport);
			}
		}
		else {
			DeleteMsgPort(timerport);
		}
	}
	if(!TimerBase) {
		errorMessage("Can't open timer.device");
		return;
	}
	timerio->tr_node.io_Command = TR_ADDREQUEST;
	timerio->tr_time.tv_secs = 1;
	timerio->tr_time.tv_micro = 0;
	SendIO((struct IORequest *) timerio);

	while (!terminated) {
		Wait ((1U << mywin->UserPort->mp_SigBit) | (1U << timerport->mp_SigBit) );

		if ((!terminated) && (1U << timerport->mp_SigBit)) {
			refresh_zz_info(mywin);
		}
		while ((!terminated) && (imsg = GT_GetIMsg(mywin->UserPort))) {
			gad = (struct Gadget *)imsg->IAddress;

			imsgClass = imsg->Class;
			imsgCode = imsg->Code;

			GT_ReplyIMsg(imsg);

			switch (imsgClass) {
				/* GadTools puts the gadget address into IAddress of IDCMP_MOUSEMOVE
				** messages.  This is NOT true for standard Intuition messages,
				** but is an added feature of GadTools.
				*/
				case IDCMP_GADGETDOWN:
				case IDCMP_MOUSEMOVE:
				case IDCMP_GADGETUP:
					handleGadgetEvent(mywin, gad, imsgCode);
					break;
				case IDCMP_VANILLAKEY:
					//handleVanillaKey(mywin, imsgCode, slider_level);
					break;
				case IDCMP_CLOSEWINDOW:
					terminated = TRUE;
					break;
				case IDCMP_REFRESHWINDOW:
					/* With GadTools, the application must use GT_BeginRefresh()
					** where it would normally have used BeginRefresh()
					*/
					GT_BeginRefresh(mywin);
					GT_EndRefresh(mywin, TRUE);
					break;
			}
		}
		timerio->tr_node.io_Command = TR_ADDREQUEST;
		timerio->tr_time.tv_secs = 1;
		timerio->tr_time.tv_micro = 0;
		SendIO((struct IORequest *) timerio);
	}
	if(TimerBase) {
		WaitIO((struct IORequest *) timerio);
		CloseDevice((struct IORequest *) timerio);
		DeleteIORequest((struct IORequest *) timerio);
		DeleteMsgPort(timerport);
		TimerBase = NULL;
	}
}

VOID gadtoolsWindow(VOID) {
	struct TextFont *font;
	struct Screen   *mysc;
	struct Window   *mywin;
	struct Gadget   *glist;
	void            *vi;
	UWORD           topborder;

	if (NULL == (font = OpenFont(&Topaz80)))
		errorMessage("Failed to open Topaz 80");
	else {
		if (NULL == (mysc = LockPubScreen(NULL)))
			errorMessage("Couldn't lock default public screen");
		else {
			if (NULL == (vi = GetVisualInfo(mysc, TAG_END)))
				errorMessage("GetVisualInfo() failed");
			else {
				topborder = mysc->WBorTop + (mysc->Font->ta_YSize + 1);

				if (NULL == createAllGadgets(&glist, vi, topborder))
					errorMessage("createAllGadgets() failed");
				else {
					if (NULL == (mywin = OpenWindowTags(NULL,
							WA_Title,     "MNT ZZTop 1.1",
							WA_Gadgets,   glist,      WA_AutoAdjust,    TRUE,
							WA_Width,       280,      WA_MinWidth,       280,
							WA_InnerHeight, 160,      WA_MinHeight,      160,
							WA_DragBar,    TRUE,      WA_DepthGadget,   TRUE,
							WA_Activate,   TRUE,      WA_CloseGadget,   TRUE,
							WA_SizeGadget, FALSE,     WA_SimpleRefresh, TRUE,
							WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW |
							IDCMP_VANILLAKEY | SLIDERIDCMP | STRINGIDCMP |
							BUTTONIDCMP,
							WA_PubScreen, mysc,
							TAG_END))) {
						errorMessage("OpenWindow() failed");
					} else {
						refresh_zz_info(mywin);
						GT_RefreshWindow(mywin, NULL);
						process_window_events(mywin);
						CloseWindow(mywin);
					}
				}

				FreeGadgets(glist);
				FreeVisualInfo(vi);
			}
			UnlockPubScreen(NULL, mysc);
		}
		CloseFont(font);
	}
}

void main(void) {
	if (!(ExpansionBase = (struct Library*)OpenLibrary("expansion.library",0L))) {
		errorMessage("Requires expansion.library");
		return;
	}

	zz_cd = (struct ConfigDev*)FindConfigDev(zz_cd,0x6d6e,0x3);
	if (zz_cd) {
		zorro_version = 2;
	} else {
		zz_cd = (struct ConfigDev*)FindConfigDev(zz_cd,0x6d6e,0x4);
		CloseLibrary(ExpansionBase);
		if (zz_cd) {
			zorro_version = 3;
		} else {
			errorMessage("MNT ZZ9000 not found.\n");
			return;
		}
	}

	zz_regs = (UBYTE*)zz_cd->cd_BoardAddr;
	CloseLibrary(ExpansionBase);

	if (NULL == (IntuitionBase = OpenLibrary("intuition.library", 37)))
		errorMessage( "Requires V37 intuition.library");
	else {
		if (NULL == (GfxBase = OpenLibrary("graphics.library", 37)))
			errorMessage( "Requires V37 graphics.library");
		else {
			if (NULL == (GadToolsBase = OpenLibrary("gadtools.library", 37)))
				errorMessage( "Requires V37 gadtools.library");
			else {
				gadtoolsWindow();
				CloseLibrary(GadToolsBase);
			}
			CloseLibrary(GfxBase);
		}
		CloseLibrary(IntuitionBase);
	}

	return;
}
