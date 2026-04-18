/*
 * MNT ZZ9000 Amiga Graphics Card Diagnostics (ZZTop)
 * Copyright (C) 2016-2026, Lucie L. Hartmann <lucie@mntre.com>
 *													MNT Research GmbH, Berlin
 *													https://mntre.com
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
#include <stdlib.h>
#include <stdint.h>

#include "zz9000.h"

struct Gadget *gads[10];

#define MYGAD_ZORROVER     (0)
#define MYGAD_FWVER        (1)
#define MYGAD_TEMP         (2)
#define MYGAD_VAUX         (3)
#define MYGAD_VINT         (4)
#define MYGAD_BTN_TEST     (5)
#define MYGAD_BTN_REFRESH  (6)
#define MYGAD_Z9AX         (7)
#define MYGAD_LPF          (8)
#define MYGAD_SCANLINES    (9)

struct TextAttr Topaz80 = { (STRPTR)"topaz.font", 8, 0, 0, };

struct Library* IntuitionBase;
struct Library* GfxBase;
struct Library* GadToolsBase;
struct Library* ExpansionBase;

struct ConfigDev* zz_cd;
volatile UBYTE* zz_regs;
int zorro_version = 0;
uint16_t scanline_mode = 0;

char txt_buf[64];

struct timerequest * timerio;
struct MsgPort *timerport;
struct Library *TimerBase;

void errorMessage(char* error)
{
	if (error) printf("Error: %s\n", error);
}

uint32_t zz_get_reg(uint32_t offset)
{
	return *((volatile uint32_t*)(zz_regs+offset));
}

uint16_t zz_get_reg16(uint32_t offset)
{
	return *((volatile uint16_t*)(zz_regs+offset));
}

void zz_set_reg(uint32_t offset, uint16_t value)
{
	*((volatile uint16_t*)(zz_regs+offset)) = value;
}

double zz_get_temperature(void)
{
	double temp = (double)(zz_get_reg16(REG_ZZ_TEMPERATURE));
	return temp/10.0;
}

double zz_get_voltage_aux(void)
{
	double vaux = (double)(zz_get_reg16(REG_ZZ_VOLTAGE_AUX));
	return vaux/100.0;
}

double zz_get_voltage_int(void)
{
	double vint = (double)(zz_get_reg16(REG_ZZ_VOLTAGE_INT));
	return vint/100.0;
}

uint32_t zz_get_ax_present(void)
{
	return zz_get_reg(REG_ZZ_AUDIO_CONFIG);
}

uint32_t zz_get_usb_status(void)
{
	return zz_get_reg(REG_ZZ_USB_STATUS);
}

uint32_t zz_get_usb_capacity(void)
{
	return zz_get_reg(REG_ZZ_USB_CAPACITY);
}

void zz_set_lpf_freq(uint16_t freq)
{
	zz_set_reg(REG_ZZ_AUDIO_PARAM, 9);
	zz_set_reg(REG_ZZ_AUDIO_VAL, freq);
	zz_set_reg(REG_ZZ_AUDIO_PARAM, 0);
}

/*
 * Scanlines V2 register map (FPGA firmware >= 1.14 with scanlines-v2
 * bitstream):
 *   0x100C = scanline_width / mode (0=off, 1=classic, 2=soft, 3=gradient)
 *   0x100E = scanline_parity (0=odd dark, 1=even dark) — set via ZZScanlines CLI
 *
 * The V1-era 0x1008 / 0x100A intensity registers still decode in the
 * V2 bitstream (now as scanline_intensity / scanline_intensity2) but
 * the V2 modes don't consult them, so they are effectively no-ops under
 * this tool.
 */
void zz_set_scanline_mode(uint16_t mode)
{
	zz_set_reg(0x100C, mode);
}

uint16_t zz_get_scanline_mode(void)
{
	return zz_get_reg16(0x100C) & 0x3;
}

double t_old=0;
void refresh_zz_info(struct Window* win)
{
	uint16_t fwrev = zz_get_reg16(REG_ZZ_FW_VERSION);

	int fwrev_major = fwrev>>8;
	int fwrev_minor = fwrev&0xff;
	double t = zz_get_temperature();
	double vaux = zz_get_voltage_aux();
	double vint = zz_get_voltage_int();
	int z9ax_present = zz_get_ax_present();

	double t_filt;
	if (t_old==0)
		t_filt=t;
	else
		t_filt=0.1*t+0.9*t_old;
	t_old=t_filt;

	GT_SetGadgetAttrs(gads[MYGAD_ZORROVER], win, NULL, GTIN_Number, zorro_version, TAG_END);

	snprintf(txt_buf, 20, "ZZ9000 %d.%d", fwrev_major, fwrev_minor);
	GT_SetGadgetAttrs(gads[MYGAD_FWVER], win, NULL, GTST_String, txt_buf, TAG_END);

	snprintf(txt_buf, 20, "%.1f", t_filt);
	GT_SetGadgetAttrs(gads[MYGAD_TEMP], win, NULL, GTST_String, txt_buf, TAG_END);

	snprintf(txt_buf, 20, "%.2f", vaux);
	GT_SetGadgetAttrs(gads[MYGAD_VAUX], win, NULL, GTST_String, txt_buf, TAG_END);

	snprintf(txt_buf, 20, "%.2f", vint);
	GT_SetGadgetAttrs(gads[MYGAD_VINT], win, NULL, GTST_String, txt_buf, TAG_END);

	if (z9ax_present) {
		GT_SetGadgetAttrs(gads[MYGAD_Z9AX], win, NULL, GTST_String, (STRPTR)"Present", TAG_END);
	} else {
		GT_SetGadgetAttrs(gads[MYGAD_Z9AX], win, NULL, GTST_String, (STRPTR)"Not present", TAG_END);
	}
}

ULONG zz_perform_memtest(uint32_t offset)
{
	volatile uint32_t* bufferl = (volatile uint32_t*)(zz_cd->cd_BoardAddr+offset);
	volatile uint16_t* bufferw = (volatile uint16_t*)bufferl;
	uint32_t i = 0;
	uint32_t errors = 0;

	printf("zz_perform_memtest...\n");

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

ULONG zz_perform_memtest_rand(uint32_t offset, int rep)
{
	uint32_t errors = 0;
	const int sz = 16;
	volatile uint16_t* buffer = (volatile uint16_t*)(zz_cd->cd_BoardAddr+offset);

	printf("zz_perform_memtest_rand...\n");

	uint16_t* tbuf = malloc(2*sz);
	if (!tbuf) {
		printf("Error: Could not allocate memory for test buffer\n");
		return 1;
	}

	for (int k = 0; k < rep; k++) {
		if ((k % 128) == 0) {
			printf("`-- Test 0x%lx %d/%d...\n", offset, k, rep);
		}
		// step 1: fill buffer with random data
		for (int i=0; i<sz; i++) {
			tbuf[sz] = rand();
		}

		buffer[0] = tbuf[0];
		buffer[1] = tbuf[1];
		buffer[2] = tbuf[2];
		buffer[3] = tbuf[3];
		buffer[4] = tbuf[4];
		buffer[5] = tbuf[5];
		buffer[6] = tbuf[6];
		buffer[7] = tbuf[7];
		buffer[8] = tbuf[8];
		buffer[9] = tbuf[9];
		buffer[10] = tbuf[10];
		buffer[11] = tbuf[11];
		buffer[12] = tbuf[12];
		buffer[13] = tbuf[13];
		buffer[14] = tbuf[14];
		buffer[15] = tbuf[15];

		for (int i=0; i<sz; i++) {
			uint16_t v = buffer[i];
			if (v != tbuf[i]) {
				if (errors<100) printf("Mismatch at 0x%p: 0x%x should be 0x%x\n",&buffer[i],v,tbuf[i]);
				errors++;
			}
		}
	}

	free(tbuf);

	printf("Done. %ld errors.\n", errors);
	return errors;
}

ULONG zz_perform_memtest_fpgareg() {
	volatile uint16_t* d1 = (volatile uint16_t*)(zz_cd->cd_BoardAddr+0x1030);
	volatile uint16_t* d2 = (volatile uint16_t*)(zz_cd->cd_BoardAddr+0x1034);
	volatile uint16_t* dr = (volatile uint16_t*)(zz_cd->cd_BoardAddr+0x1030);

	printf("zz_perform_memtest_fpgareg...\n");

	*d2 = 1;
	for (int i = 0; i < 0x100000*2; i++) {
		*d1 = i;
	}

	printf("Done. Result: %lx\n", *dr);

	return 0;
}

ULONG zz_perform_memtest_multi() {
	uint32_t offset = 0x100000;
	zz_perform_memtest(offset);
	zz_perform_memtest_rand(offset, 1024);
	//zz_perform_memtest_fpgareg();

	return 0;
}

VOID handleGadgetEvent(struct Window *win, struct Gadget *gad, ULONG code)
{
	switch (gad->GadgetID)
	{
		case MYGAD_BTN_REFRESH: {
			refresh_zz_info(win);
			break;
		}
		case MYGAD_BTN_TEST: {
			zz_perform_memtest_multi();
			break;
		}
		case MYGAD_LPF: {
			zz_set_lpf_freq(code);
			break;
		}
		case MYGAD_SCANLINES: {
			scanline_mode = code;
			zz_set_scanline_mode(code);
			break;
		}
	}
}

struct Gadget *createAllGadgets(struct Gadget **glistptr, void *vi, UWORD topborder)
{
	struct NewGadget ng;
	struct Gadget *gad;

	gad = CreateContext(glistptr);

	ng.ng_LeftEdge	 = 20;
	ng.ng_TopEdge		 = 210+topborder;
	ng.ng_Width			 = 100;
	ng.ng_Height		 = 14;
	ng.ng_GadgetText = (STRPTR)"Bus Test";
	ng.ng_TextAttr	 = &Topaz80;
	ng.ng_VisualInfo = vi;
	ng.ng_GadgetID	 = MYGAD_BTN_TEST;
	ng.ng_Flags			 = 0;

	gads[MYGAD_BTN_REFRESH] = gad = CreateGadget(BUTTON_KIND, gad, &ng,
										TAG_END);

	ng.ng_LeftEdge	= 160;
	ng.ng_GadgetID	 = MYGAD_BTN_REFRESH;
	ng.ng_GadgetText = (STRPTR)"Refresh";

	gads[MYGAD_BTN_TEST] = gad = CreateGadget(BUTTON_KIND, gad, &ng,
										TAG_END);

	ng.ng_LeftEdge	= 160;
	ng.ng_TopEdge	= 20+topborder;
	ng.ng_GadgetID	= MYGAD_ZORROVER;
	ng.ng_GadgetText = (STRPTR)"Zorro Version";

	gads[MYGAD_ZORROVER] = gad = CreateGadget(INTEGER_KIND, gad, &ng,
										GTIN_Number, 0,
										TAG_END);

	ng.ng_TopEdge	= 40+topborder;
	ng.ng_GadgetID	= MYGAD_FWVER;
	ng.ng_GadgetText = (STRPTR)"Firmware Version";

	gads[MYGAD_FWVER] = gad = CreateGadget(STRING_KIND, gad, &ng,
										GTST_String, "",
										TAG_END);

	ng.ng_TopEdge	= 60+topborder;
	ng.ng_GadgetID	= MYGAD_TEMP;
	ng.ng_GadgetText = (STRPTR)"Core �C";

	gads[MYGAD_TEMP] = gad = CreateGadget(STRING_KIND, gad, &ng,
										GTST_String, "",
										TAG_END);

	ng.ng_TopEdge	= 80+topborder;
	ng.ng_GadgetID	= MYGAD_VAUX;
	ng.ng_GadgetText = (STRPTR)"Aux Voltage V";

	gads[MYGAD_VAUX] = gad = CreateGadget(STRING_KIND, gad, &ng,
										GTST_String, "",
										TAG_END);

	ng.ng_TopEdge	= 100+topborder;
	ng.ng_GadgetID	= MYGAD_VINT;
	ng.ng_GadgetText = (STRPTR)"Core Voltage V";

	gads[MYGAD_VINT] = gad = CreateGadget(STRING_KIND, gad, &ng,
										GTST_String, "",
										TAG_END);

	ng.ng_TopEdge	= 120+topborder;
	ng.ng_GadgetID	= MYGAD_Z9AX;
	ng.ng_GadgetText = (STRPTR)"ZZ9000AX";

	gads[MYGAD_Z9AX] = gad = CreateGadget(STRING_KIND, gad, &ng,
										GTST_String, "",
										TAG_END);

	ng.ng_TopEdge	= 140+topborder;
	ng.ng_GadgetID	= MYGAD_LPF;
	ng.ng_GadgetText = (STRPTR)"AX Lowpass";

	gads[MYGAD_LPF] = gad = CreateGadget(SLIDER_KIND, gad, &ng,
										GTSL_Min, 0,
										GTSL_Max, 23900,
										GTSL_Level, 23900,
										GTSL_LevelFormat, "%ld Hz",
										GTSL_MaxLevelLen, 10,
										GTSL_LevelPlace, PLACETEXT_BELOW,
										TAG_END);

	ng.ng_TopEdge	= 175+topborder;
	ng.ng_GadgetID	= MYGAD_SCANLINES;
	ng.ng_GadgetText = (STRPTR)"Scanlines";

	/* V2 modes: 0=off 1=classic 2=soft 3=gradient. Parity (odd/even
	 * dark) is secondary and set via the ZZScanlines CLI tool. */
	gads[MYGAD_SCANLINES] = gad = CreateGadget(SLIDER_KIND, gad, &ng,
										GTSL_Min, 0,
										GTSL_Max, 3,
										GTSL_Level, scanline_mode,
										GTSL_LevelFormat, "Mode %ld",
										GTSL_MaxLevelLen, 10,
										GTSL_LevelPlace, PLACETEXT_BELOW,
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

	/*if((timerport = CreateMsgPort())) {
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
	SendIO((struct IORequest *) timerio);*/

	while (!terminated) {
		Wait ((1U << mywin->UserPort->mp_SigBit)); // | (1U << timerport->mp_SigBit) );

		/*if ((!terminated) && (1U << timerport->mp_SigBit)) {
			refresh_zz_info(mywin);
		}*/

		while ((!terminated) && (imsg = GT_GetIMsg(mywin->UserPort))) {
			gad = (struct Gadget *)imsg->IAddress;

			imsgClass = imsg->Class;
			imsgCode = imsg->Code;

			GT_ReplyIMsg(imsg);

			switch (imsgClass) {
				/* GadTools puts the gadget address into IAddress of IDCMP_MOUSEMOVE
				** messages.	This is NOT true for standard Intuition messages,
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

		/*timerio->tr_node.io_Command = TR_ADDREQUEST;
		timerio->tr_time.tv_secs = 1;
		timerio->tr_time.tv_micro = 0;
		SendIO((struct IORequest *) timerio);*/
	}

	/*if(TimerBase) {
		WaitIO((struct IORequest *) timerio);
		CloseDevice((struct IORequest *) timerio);
		DeleteIORequest((struct IORequest *) timerio);
		DeleteMsgPort(timerport);
		TimerBase = NULL;
	}*/
}

VOID gadtoolsWindow(VOID) {
	struct TextFont *font;
	struct Screen		*mysc;
	struct Window		*mywin;
	struct Gadget		*glist;
	void						*vi;
	UWORD						topborder;

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
							WA_Title,			"MNT ZZTop 1.11",
							WA_Gadgets,		glist,			WA_AutoAdjust,		TRUE,
							WA_Width,				280,			WA_MinWidth,			 280,
							WA_InnerHeight, 230,			WA_MinHeight,			 230,
							WA_DragBar,		 TRUE,			WA_DepthGadget,		TRUE,
							WA_Activate,	 TRUE,			WA_CloseGadget,		TRUE,
							WA_SizeGadget, FALSE,			WA_SimpleRefresh, TRUE,
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

int main(void) {
	if (!(ExpansionBase = (struct Library*)OpenLibrary((CONST_STRPTR)"expansion.library",0L))) {
		errorMessage("Requires expansion.library");
		return 0;
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
			return 0;
		}
	}

	zz_regs = (UBYTE*)zz_cd->cd_BoardAddr;
	CloseLibrary(ExpansionBase);

	/* Sync the slider with whatever mode the FPGA currently holds — the
	 * V2 bitstream keeps scanline state across soft resets, so a prior
	 * CLI or ZZTop session may have left a non-zero mode configured. */
	scanline_mode = zz_get_scanline_mode();

	if (NULL == (IntuitionBase = OpenLibrary((CONST_STRPTR)"intuition.library", 37)))
		errorMessage("Requires V37 intuition.library");
	else {
		if (NULL == (GfxBase = OpenLibrary((CONST_STRPTR)"graphics.library", 37)))
			errorMessage("Requires V37 graphics.library");
		else {
			if (NULL == (GadToolsBase = OpenLibrary((CONST_STRPTR)"gadtools.library", 37)))
				errorMessage("Requires V37 gadtools.library");
			else {
				gadtoolsWindow();
				CloseLibrary(GadToolsBase);
			}
			CloseLibrary(GfxBase);
		}
		CloseLibrary(IntuitionBase);
	}

	return 0;
}
