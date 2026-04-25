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
#include <devices/timer.h>

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

#define ZZTOP_RELEASE "2.0.1"
#define ZZTOP_DATE    "25.04.2026"

static const char version[] __attribute__((used)) =
	"$VER: ZZTop " ZZTOP_RELEASE " (" ZZTOP_DATE ")\r\n";

struct Gadget *gads[16];

#define MYGAD_ZORROVER     (0)
#define MYGAD_FWVER        (1)
#define MYGAD_TEMP         (2)
#define MYGAD_TEMP_MINMAX  (3)
#define MYGAD_VAUX         (4)
#define MYGAD_VINT         (5)
#define MYGAD_Z9AX         (6)
#define MYGAD_STATUS       (7)
#define MYGAD_RAWREGS      (8)
#define MYGAD_LPF          (9)
#define MYGAD_SCANLINES    (10)
#define MYGAD_PARITY       (11)
#define MYGAD_REFRESHMODE  (12)
#define MYGAD_BTN_TEST     (13)
#define MYGAD_BTN_REFRESH  (14)
#define MYGAD_TEST_RESULT  (15)

#define ZZTOP_REG_SD_STATUS       (0xBC)
#define ZZTOP_REG_SD_BOOT_STATUS  (0xC4)
#define ZZTOP_REG_SCANLINE_MODE   (0x100C)
#define ZZTOP_REG_SCANLINE_PARITY (0x100E)

#define SCANLINE_MODE_COUNT 4
#define REFRESH_MODE_COUNT  3
#define ZZTOP_BUS_TEST_BYTES       (64UL * 1024UL)
#define ZZTOP_BUS_TEST_MEM_BASE    (0x00010000UL)
#define ZZTOP_BUS_TEST_Z2_RESERVED (0x00020000UL)
#define ZZTOP_BUS_TEST_Z3_TOP      (0x03000000UL)
#define ZZTOP_BUS_TEST_NO_RANGE    (0xffffffffUL)
#define ZZTOP_BUS_TEST_NO_BACKUP   (0xfffffffeUL)

static STRPTR parity_labels[] = {
	(STRPTR)"Odd dark",
	(STRPTR)"Even dark",
	NULL
};

static STRPTR scanline_labels[] = {
	(STRPTR)"Off",
	(STRPTR)"Classic",
	(STRPTR)"Soft",
	(STRPTR)"Gradient",
	NULL
};

static STRPTR refresh_labels[] = {
	(STRPTR)"Manual",
	(STRPTR)"1 sec",
	(STRPTR)"5 sec",
	NULL
};

struct TextAttr Topaz80 = { (STRPTR)"topaz.font", 8, 0, 0, };

struct Library* IntuitionBase;
struct Library* GfxBase;
struct Library* GadToolsBase;
struct Library* ExpansionBase;

struct ConfigDev* zz_cd;
volatile UBYTE* zz_regs;
int zorro_version = 0;
uint16_t scanline_mode = 0;
uint16_t scanline_parity = 0;
uint16_t refresh_mode = 0;
double t_min = 0;
double t_max = 0;

char txt_buf[64];

struct timerequest * timerio;
struct MsgPort *timerport;
struct Library *TimerBase;
BOOL timer_pending = FALSE;

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
 * Scanlines V2 register map (FPGA firmware >= 2.0.0 with scanlines-v2
 * bitstream):
 *   0x100C = scanline_width / mode (0=off, 1=classic, 2=soft, 3=gradient)
 *   0x100E = scanline_parity (0=odd dark, 1=even dark)
 *
 * The V1-era 0x1008 / 0x100A intensity registers still decode in the
 * V2 bitstream (now as scanline_intensity / scanline_intensity2) but
 * the V2 modes don't consult them, so they are effectively no-ops under
 * this tool.
 */
void zz_set_scanline_mode(uint16_t mode)
{
	zz_set_reg(ZZTOP_REG_SCANLINE_MODE, mode);
}

uint16_t zz_get_scanline_mode(void)
{
	return zz_get_reg16(ZZTOP_REG_SCANLINE_MODE) & 0x3;
}

void zz_set_scanline_parity(uint16_t parity)
{
	zz_set_reg(ZZTOP_REG_SCANLINE_PARITY, parity & 0x1);
}

uint16_t zz_get_scanline_parity(void)
{
	return zz_get_reg16(ZZTOP_REG_SCANLINE_PARITY) & 0x1;
}

uint16_t zztop_refresh_seconds(void)
{
	if (refresh_mode == 1) return 1;
	if (refresh_mode == 2) return 5;
	return 0;
}

BOOL zztop_open_timer(void)
{
	if (TimerBase) return TRUE;

	if (!(timerport = CreateMsgPort())) return FALSE;

	timerio = (struct timerequest *)CreateIORequest(timerport, sizeof(struct timerequest));
	if (!timerio) {
		DeleteMsgPort(timerport);
		timerport = NULL;
		return FALSE;
	}

	if (OpenDevice((STRPTR)TIMERNAME, UNIT_MICROHZ, (struct IORequest *)timerio, 0) != 0) {
		DeleteIORequest((struct IORequest *)timerio);
		DeleteMsgPort(timerport);
		timerio = NULL;
		timerport = NULL;
		return FALSE;
	}

	TimerBase = (struct Library *)timerio->tr_node.io_Device;
	return TRUE;
}

void zztop_cancel_timer(void)
{
	if (!TimerBase || !timer_pending) return;

	if (!CheckIO((struct IORequest *)timerio)) {
		AbortIO((struct IORequest *)timerio);
	}
	WaitIO((struct IORequest *)timerio);
	timer_pending = FALSE;
}

void zztop_close_timer(void)
{
	zztop_cancel_timer();

	if (TimerBase) {
		CloseDevice((struct IORequest *)timerio);
		TimerBase = NULL;
	}
	if (timerio) {
		DeleteIORequest((struct IORequest *)timerio);
		timerio = NULL;
	}
	if (timerport) {
		DeleteMsgPort(timerport);
		timerport = NULL;
	}
}

void zztop_schedule_timer(void)
{
	uint16_t secs = zztop_refresh_seconds();

	if (!TimerBase || timer_pending || secs == 0) return;

	timerio->tr_node.io_Command = TR_ADDREQUEST;
	timerio->tr_time.tv_secs = secs;
	timerio->tr_time.tv_micro = 0;
	SendIO((struct IORequest *)timerio);
	timer_pending = TRUE;
}

void zztop_restart_timer(void)
{
	zztop_cancel_timer();
	zztop_schedule_timer();
}

double t_old=0;
void refresh_zz_info(struct Window* win)
{
	uint16_t fwrev = zz_get_reg16(REG_ZZ_FW_VERSION);
	uint16_t raw_temp = zz_get_reg16(REG_ZZ_TEMPERATURE);
	uint16_t raw_vaux = zz_get_reg16(REG_ZZ_VOLTAGE_AUX);
	uint16_t raw_usb = zz_get_reg16(REG_ZZ_USB_STATUS);
	uint16_t raw_sd = zz_get_reg16(ZZTOP_REG_SD_STATUS);
	uint16_t raw_sd_boot = zz_get_reg16(ZZTOP_REG_SD_BOOT_STATUS);
	uint16_t raw_scanline = zz_get_reg16(ZZTOP_REG_SCANLINE_MODE);
	uint16_t raw_parity = zz_get_reg16(ZZTOP_REG_SCANLINE_PARITY);

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

	if (t_min == 0 || t < t_min) t_min = t;
	if (t_max == 0 || t > t_max) t_max = t;

	GT_SetGadgetAttrs(gads[MYGAD_ZORROVER], win, NULL, GTIN_Number, zorro_version, TAG_END);

	snprintf(txt_buf, 20, "ABI %d.%d", fwrev_major, fwrev_minor);
	GT_SetGadgetAttrs(gads[MYGAD_FWVER], win, NULL, GTST_String, txt_buf, TAG_END);

	snprintf(txt_buf, 20, "%.1f", t_filt);
	GT_SetGadgetAttrs(gads[MYGAD_TEMP], win, NULL, GTST_String, txt_buf, TAG_END);

	snprintf(txt_buf, 20, "%.1f / %.1f", t_min, t_max);
	GT_SetGadgetAttrs(gads[MYGAD_TEMP_MINMAX], win, NULL, GTST_String, txt_buf, TAG_END);

	snprintf(txt_buf, 20, "%.2f", vaux);
	GT_SetGadgetAttrs(gads[MYGAD_VAUX], win, NULL, GTST_String, txt_buf, TAG_END);

	snprintf(txt_buf, 20, "%.2f", vint);
	GT_SetGadgetAttrs(gads[MYGAD_VINT], win, NULL, GTST_String, txt_buf, TAG_END);

	if (z9ax_present) {
		GT_SetGadgetAttrs(gads[MYGAD_Z9AX], win, NULL, GTST_String, (STRPTR)"Present", TAG_END);
	} else {
		GT_SetGadgetAttrs(gads[MYGAD_Z9AX], win, NULL, GTST_String, (STRPTR)"Not present", TAG_END);
	}

	snprintf(txt_buf, 64, "AX:%c USB:%04x SD:%04x B:%04x",
		z9ax_present ? 'Y' : 'N', raw_usb, raw_sd, raw_sd_boot);
	GT_SetGadgetAttrs(gads[MYGAD_STATUS], win, NULL, GTST_String, txt_buf, TAG_END);

	snprintf(txt_buf, 64, "S:%04x P:%04x T:%04x A:%04x",
		raw_scanline, raw_parity, raw_temp, raw_vaux);
	GT_SetGadgetAttrs(gads[MYGAD_RAWREGS], win, NULL, GTST_String, txt_buf, TAG_END);
}

ULONG zz_perform_memtest(uint32_t offset)
{
	volatile uint32_t* bufferl = (volatile uint32_t*)(zz_cd->cd_BoardAddr+offset);
	volatile uint16_t* bufferw = (volatile uint16_t*)bufferl;
	uint32_t i = 0;
	uint32_t errors = 0;
	uint32_t long_count = ZZTOP_BUS_TEST_BYTES / 4;
	uint32_t word_count = ZZTOP_BUS_TEST_BYTES / 2;

	for (i=0; i<long_count; i++) {
		uint32_t v2 = 0;
		uint32_t v = (i%2)?0xaaaa5555:0x33337777;

		bufferl[i] = v;
		v2 = bufferl[i];

		if (v!=v2) {
			errors++;
		}
	}

	for (i=0; i<word_count; i++) {
		uint16_t v4 = 0;
		uint16_t v3 = (i%2)?0xffff:0x0000;

		bufferw[i] = v3;
		v4 = bufferw[i];

		if (v3!=v4) {
			errors++;
		}
	}

	return errors;
}

ULONG zz_perform_memtest_rand(uint32_t offset, int rep)
{
	uint32_t errors = 0;
	const int sz = 16;
	uint32_t word_count = ZZTOP_BUS_TEST_BYTES / 2;
	volatile uint16_t* buffer = (volatile uint16_t*)(zz_cd->cd_BoardAddr+offset);
	uint16_t tbuf[16];

	for (int k = 0; k < rep; k++) {
		uint32_t pos = (k * sz) % (word_count - sz);
		volatile uint16_t* slot = buffer + pos;

		for (int i=0; i<sz; i++) {
			tbuf[i] = rand();
		}

		for (int i=0; i<sz; i++) {
			slot[i] = tbuf[i];
		}

		for (int i=0; i<sz; i++) {
			uint16_t v = slot[i];
			if (v != tbuf[i]) {
				errors++;
			}
		}
	}

	return errors;
}

ULONG zz_perform_memtest_fpgareg() {
	volatile uint16_t* d1 = (volatile uint16_t*)(zz_cd->cd_BoardAddr+0x1030);
	volatile uint16_t* d2 = (volatile uint16_t*)(zz_cd->cd_BoardAddr+0x1034);

	*d2 = 1;
	for (int i = 0; i < 0x100000*2; i++) {
		*d1 = i;
	}

	return 0;
}

uint32_t zztop_bus_test_offset(void)
{
	uint32_t top;

	if (zorro_version == 3) {
		top = ZZTOP_BUS_TEST_Z3_TOP;
	} else {
		uint32_t board_size = (uint32_t)zz_cd->cd_BoardSize;

		if (board_size <= ZZTOP_BUS_TEST_Z2_RESERVED) return 0;
		top = board_size - ZZTOP_BUS_TEST_Z2_RESERVED;
	}

	if (top <= ZZTOP_BUS_TEST_MEM_BASE + ZZTOP_BUS_TEST_BYTES) return 0;
	return top - ZZTOP_BUS_TEST_BYTES;
}

ULONG zz_perform_memtest_multi() {
	uint32_t offset = zztop_bus_test_offset();
	volatile uint8_t* test_base;
	uint8_t* backup;
	ULONG errors = 0;

	if (offset == 0) return ZZTOP_BUS_TEST_NO_RANGE;

	backup = malloc(ZZTOP_BUS_TEST_BYTES);
	if (!backup) return ZZTOP_BUS_TEST_NO_BACKUP;

	test_base = (volatile uint8_t*)(zz_cd->cd_BoardAddr+offset);
	for (uint32_t i=0; i<ZZTOP_BUS_TEST_BYTES; i++) {
		backup[i] = test_base[i];
	}

	errors += zz_perform_memtest(offset);
	errors += zz_perform_memtest_rand(offset, 1024);
	//zz_perform_memtest_fpgareg();

	for (uint32_t i=0; i<ZZTOP_BUS_TEST_BYTES; i++) {
		test_base[i] = backup[i];
	}

	free(backup);

	return errors;
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
			ULONG errors;

			GT_SetGadgetAttrs(gads[MYGAD_TEST_RESULT], win, NULL,
				GTST_String, (STRPTR)"Running...", TAG_END);
			errors = zz_perform_memtest_multi();
			if (errors == ZZTOP_BUS_TEST_NO_RANGE) {
				GT_SetGadgetAttrs(gads[MYGAD_TEST_RESULT], win, NULL,
					GTST_String, (STRPTR)"No safe range", TAG_END);
			} else if (errors == ZZTOP_BUS_TEST_NO_BACKUP) {
				GT_SetGadgetAttrs(gads[MYGAD_TEST_RESULT], win, NULL,
					GTST_String, (STRPTR)"No backup RAM", TAG_END);
			} else if (errors == 0) {
				snprintf(txt_buf, 20, "OK %luK",
					(unsigned long)(ZZTOP_BUS_TEST_BYTES / 1024));
				GT_SetGadgetAttrs(gads[MYGAD_TEST_RESULT], win, NULL,
					GTST_String, txt_buf, TAG_END);
			} else {
				snprintf(txt_buf, 20, "%lu errors", (unsigned long)errors);
				GT_SetGadgetAttrs(gads[MYGAD_TEST_RESULT], win, NULL,
					GTST_String, txt_buf, TAG_END);
			}
			refresh_zz_info(win);
			break;
		}
		case MYGAD_LPF: {
			zz_set_lpf_freq(code);
			break;
		}
		case MYGAD_SCANLINES: {
			scanline_mode = (scanline_mode + 1) % SCANLINE_MODE_COUNT;
			zz_set_scanline_mode(scanline_mode);
			GT_SetGadgetAttrs(gads[MYGAD_SCANLINES], win, NULL,
				GTCY_Active, scanline_mode, TAG_END);
			refresh_zz_info(win);
			break;
		}
		case MYGAD_PARITY: {
			scanline_parity = (scanline_parity + 1) & 0x1;
			zz_set_scanline_parity(scanline_parity);
			GT_SetGadgetAttrs(gads[MYGAD_PARITY], win, NULL,
				GTCY_Active, scanline_parity, TAG_END);
			refresh_zz_info(win);
			break;
		}
		case MYGAD_REFRESHMODE: {
			refresh_mode = (refresh_mode + 1) % REFRESH_MODE_COUNT;
			GT_SetGadgetAttrs(gads[MYGAD_REFRESHMODE], win, NULL,
				GTCY_Active, refresh_mode, TAG_END);
			zztop_restart_timer();
			break;
		}
	}
}

struct Gadget *createAllGadgets(struct Gadget **glistptr, void *vi, UWORD topborder)
{
	struct NewGadget ng;
	struct Gadget *gad;

	gad = CreateContext(glistptr);

	ng.ng_LeftEdge	 = 170;
	ng.ng_TopEdge		 = 20+topborder;
	ng.ng_Width			 = 220;
	ng.ng_Height		 = 14;
	ng.ng_GadgetText = (STRPTR)"Zorro Version";
	ng.ng_TextAttr	 = &Topaz80;
	ng.ng_VisualInfo = vi;
	ng.ng_GadgetID	 = MYGAD_ZORROVER;
	ng.ng_Flags			 = PLACETEXT_LEFT;

	gads[MYGAD_ZORROVER] = gad = CreateGadget(INTEGER_KIND, gad, &ng,
											GTIN_Number, 0,
											TAG_END);

	ng.ng_TopEdge	= 40+topborder;
	ng.ng_GadgetID	= MYGAD_FWVER;
	ng.ng_GadgetText = (STRPTR)"Firmware ABI";

	gads[MYGAD_FWVER] = gad = CreateGadget(STRING_KIND, gad, &ng,
											GTST_String, "",
											TAG_END);

	ng.ng_TopEdge	= 60+topborder;
	ng.ng_GadgetID	= MYGAD_TEMP;
	ng.ng_GadgetText = (STRPTR)"Die \260C";

	gads[MYGAD_TEMP] = gad = CreateGadget(STRING_KIND, gad, &ng,
											GTST_String, "",
											TAG_END);

	ng.ng_TopEdge	= 80+topborder;
	ng.ng_GadgetID	= MYGAD_TEMP_MINMAX;
	ng.ng_GadgetText = (STRPTR)"Die Min/Max \260C";

	gads[MYGAD_TEMP_MINMAX] = gad = CreateGadget(STRING_KIND, gad, &ng,
											GTST_String, "",
											TAG_END);

	ng.ng_TopEdge	= 100+topborder;
	ng.ng_GadgetID	= MYGAD_VAUX;
	ng.ng_GadgetText = (STRPTR)"VCCAUX V";

	gads[MYGAD_VAUX] = gad = CreateGadget(STRING_KIND, gad, &ng,
											GTST_String, "",
											TAG_END);

	ng.ng_TopEdge	= 120+topborder;
	ng.ng_GadgetID	= MYGAD_VINT;
	ng.ng_GadgetText = (STRPTR)"VCCINT V";

	gads[MYGAD_VINT] = gad = CreateGadget(STRING_KIND, gad, &ng,
											GTST_String, "",
											TAG_END);

	ng.ng_TopEdge	= 140+topborder;
	ng.ng_GadgetID	= MYGAD_Z9AX;
	ng.ng_GadgetText = (STRPTR)"ZZ9000AX";

	gads[MYGAD_Z9AX] = gad = CreateGadget(STRING_KIND, gad, &ng,
											GTST_String, "",
											TAG_END);

	ng.ng_TopEdge	= 160+topborder;
	ng.ng_GadgetID	= MYGAD_STATUS;
	ng.ng_GadgetText = (STRPTR)"Status";

	gads[MYGAD_STATUS] = gad = CreateGadget(STRING_KIND, gad, &ng,
											GTST_String, "",
											TAG_END);

	ng.ng_TopEdge	= 180+topborder;
	ng.ng_GadgetID	= MYGAD_RAWREGS;
	ng.ng_GadgetText = (STRPTR)"Raw Regs";

	gads[MYGAD_RAWREGS] = gad = CreateGadget(STRING_KIND, gad, &ng,
											GTST_String, "",
											TAG_END);

	ng.ng_TopEdge	= 205+topborder;
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

	ng.ng_TopEdge	= 240+topborder;
	ng.ng_GadgetID	= MYGAD_SCANLINES;
	ng.ng_GadgetText = (STRPTR)"Scanlines";

	gads[MYGAD_SCANLINES] = gad = CreateGadget(CYCLE_KIND, gad, &ng,
											GTCY_Labels, scanline_labels,
											GTCY_Active, scanline_mode,
											TAG_END);

	ng.ng_TopEdge	= 265+topborder;
	ng.ng_GadgetID	= MYGAD_PARITY;
	ng.ng_GadgetText = (STRPTR)"Parity";

	gads[MYGAD_PARITY] = gad = CreateGadget(CYCLE_KIND, gad, &ng,
											GTCY_Labels, parity_labels,
											GTCY_Active, scanline_parity,
											TAG_END);

	ng.ng_TopEdge	= 290+topborder;
	ng.ng_GadgetID	= MYGAD_REFRESHMODE;
	ng.ng_GadgetText = (STRPTR)"Auto Refresh";

	gads[MYGAD_REFRESHMODE] = gad = CreateGadget(CYCLE_KIND, gad, &ng,
											GTCY_Labels, refresh_labels,
											GTCY_Active, refresh_mode,
											TAG_END);

	ng.ng_TopEdge	= 320+topborder;
	ng.ng_GadgetID	= MYGAD_TEST_RESULT;
	ng.ng_GadgetText = (STRPTR)"Bus Result";

	gads[MYGAD_TEST_RESULT] = gad = CreateGadget(STRING_KIND, gad, &ng,
											GTST_String, (STRPTR)"Not run",
											TAG_END);

	ng.ng_LeftEdge	 = 20;
	ng.ng_TopEdge		 = 345+topborder;
	ng.ng_Width			 = 110;
	ng.ng_GadgetText = (STRPTR)"Bus Test";
	ng.ng_GadgetID	 = MYGAD_BTN_TEST;
	ng.ng_Flags			 = PLACETEXT_IN;

	gads[MYGAD_BTN_TEST] = gad = CreateGadget(BUTTON_KIND, gad, &ng,
											TAG_END);

	ng.ng_LeftEdge	= 170;
	ng.ng_GadgetID	 = MYGAD_BTN_REFRESH;
	ng.ng_GadgetText = (STRPTR)"Refresh";

	gads[MYGAD_BTN_REFRESH] = gad = CreateGadget(BUTTON_KIND, gad, &ng,
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
	ULONG user_sig = 1U << mywin->UserPort->mp_SigBit;
	ULONG timer_sig = 0;

	if (zztop_open_timer()) {
		timer_sig = 1U << timerport->mp_SigBit;
		zztop_schedule_timer();
	} else {
		errorMessage("Auto refresh disabled: can't open timer.device");
	}

	while (!terminated) {
		ULONG signals = Wait(user_sig | timer_sig);

		if (timer_sig && (signals & timer_sig) && timer_pending &&
			CheckIO((struct IORequest *)timerio)) {
			WaitIO((struct IORequest *)timerio);
			timer_pending = FALSE;
			refresh_zz_info(mywin);
			zztop_schedule_timer();
		}

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
				case IDCMP_MOUSEMOVE:
					if (gad && gad->GadgetID == MYGAD_LPF) {
						handleGadgetEvent(mywin, gad, imsgCode);
					}
					break;
				case IDCMP_GADGETDOWN:
					break;
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
	}

	zztop_close_timer();
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
							WA_Title,			"MNT ZZTop " ZZTOP_RELEASE,
							WA_Gadgets,		glist,			WA_AutoAdjust,		TRUE,
							WA_Width,				430,			WA_MinWidth,			 430,
							WA_InnerHeight, 370,			WA_MinHeight,			 370,
							WA_DragBar,		 TRUE,			WA_DepthGadget,		TRUE,
							WA_Activate,	 TRUE,			WA_CloseGadget,		TRUE,
							WA_SizeGadget, FALSE,			WA_SimpleRefresh, TRUE,
							WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW |
								IDCMP_VANILLAKEY | SLIDERIDCMP | STRINGIDCMP |
								BUTTONIDCMP | CYCLEIDCMP,
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
	scanline_parity = zz_get_scanline_parity();

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
