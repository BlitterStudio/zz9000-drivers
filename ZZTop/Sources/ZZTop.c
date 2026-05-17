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
#include <stdint.h>
#include <string.h>

#include "zz9000.h"

#define ZZTOP_RELEASE "2.0.2"
#define ZZTOP_DATE    "17.05.2026"

static const char version[] __attribute__((used)) =
	"$VER: ZZTop " ZZTOP_RELEASE " (" ZZTOP_DATE ")\r\n";

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
#define MYGAD_VIDEOCAP     (16)
#define MYGAD_COUNT        (17)

#define LABEL_ZORROVER     "Zorro Version"
#define LABEL_FWVER        "Firmware ABI"
#define LABEL_TEMP         "Die \260C"
#define LABEL_TEMP_MINMAX  "Die Min/Max \260C"
#define LABEL_VAUX         "VCCAUX V"
#define LABEL_VINT         "VCCINT V"
#define LABEL_Z9AX         "ZZ9000AX"
#define LABEL_STATUS       "Status"
#define LABEL_RAWREGS      "Raw Regs"
#define LABEL_VIDEOCAP     "VideoCap"
#define LABEL_LPF          "AX Lowpass"
#define LABEL_SCANLINES    "Scanlines"
#define LABEL_PARITY       "Parity"
#define LABEL_REFRESHMODE  "Auto Refresh"
#define LABEL_TEST_RESULT  "Result"
#define LABEL_BTN_TEST     "Reg Probe"
#define LABEL_BTN_REFRESH  "Refresh"

#define SAMPLE_FWVER       "ABI 255.255"
#define SAMPLE_TEMP        "999.9"
#define SAMPLE_TEMP_MINMAX "999.9 / 999.9"
#define SAMPLE_VOLTAGE     "99.99"
#define SAMPLE_Z9AX        "Not present"
#define SAMPLE_STATUS      "AX:Y USB:ffff SD:ffff B:ffff"
#define SAMPLE_RAWREGS     "S:ffff P:ffff T:ffff A:ffff"
#define SAMPLE_VIDEOCAP    "Lines:1023  Max:3/3  Min:3/3"
#define SAMPLE_LPF_LEVEL   "23900 Hz"
#define SAMPLE_TEST_RESULT "No timer.device"

struct Gadget *gads[MYGAD_COUNT];

#define ZZTOP_REG_SD_STATUS       (0xBC)
#define ZZTOP_REG_SD_BOOT_STATUS  (0xC4)
#define ZZTOP_REG_SCANLINE_MODE   (0x100C)
#define ZZTOP_REG_SCANLINE_PARITY (0x100E)

/* Bit layout for REG_ZZ_VIDEOCAP_STATS (issue #11 diagnostic).
 *   [9:0]   videocap_ymax  (lines per detected field, max 1023)
 *   [11:10] top 2 bits of the per-field MAX HSYNC pulse width
 *   [13:12] top 2 bits of the per-field MIN HSYNC pulse width
 *           (0=short, 3=very wide; both tiers use the same scale)
 *   [15:14] reserved (always 0)
 * Comparing max and min tells genlock failure modes apart:
 *   max only wide  => some pulses wide (CSYNC pulses in VBI)
 *   max + min wide => every pulse wide (polarity flip / EXTSYNC timing) */
#define VCAP_LINES_MASK         (0x3FF)
#define VCAP_PW_MAX_TIER_SHIFT  (10)
#define VCAP_PW_MIN_TIER_SHIFT  (12)
#define VCAP_PW_TIER_MASK       (0x3)
#define VCAP_PW_TIER_MAX        (3)

#define SCANLINE_MODE_COUNT 4
#define REFRESH_MODE_COUNT  3
#define ZZTOP_PROBE_READS 8

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

struct ZZTopLayout {
	WORD margin_x;
	WORD margin_y;
	WORD label_gap;
	WORD gadget_left;
	WORD gadget_width;
	WORD gadget_height;
	WORD row_step;
	WORD section_gap;
	WORD slider_step;
	WORD control_step;
	WORD button_width;
	WORD button_top;
	WORD window_width;
	WORD window_height;
	UWORD topborder;
	const struct TextAttr *text_attr;
};

static CONST_STRPTR zztop_label_samples[] = {
	(CONST_STRPTR)LABEL_ZORROVER,
	(CONST_STRPTR)LABEL_FWVER,
	(CONST_STRPTR)LABEL_TEMP,
	(CONST_STRPTR)LABEL_TEMP_MINMAX,
	(CONST_STRPTR)LABEL_VAUX,
	(CONST_STRPTR)LABEL_VINT,
	(CONST_STRPTR)LABEL_Z9AX,
	(CONST_STRPTR)LABEL_STATUS,
	(CONST_STRPTR)LABEL_RAWREGS,
	(CONST_STRPTR)LABEL_VIDEOCAP,
	(CONST_STRPTR)LABEL_LPF,
	(CONST_STRPTR)LABEL_SCANLINES,
	(CONST_STRPTR)LABEL_PARITY,
	(CONST_STRPTR)LABEL_REFRESHMODE,
	(CONST_STRPTR)LABEL_TEST_RESULT,
	NULL
};

static CONST_STRPTR zztop_value_samples[] = {
	(CONST_STRPTR)SAMPLE_FWVER,
	(CONST_STRPTR)SAMPLE_TEMP,
	(CONST_STRPTR)SAMPLE_TEMP_MINMAX,
	(CONST_STRPTR)SAMPLE_VOLTAGE,
	(CONST_STRPTR)SAMPLE_Z9AX,
	(CONST_STRPTR)SAMPLE_STATUS,
	(CONST_STRPTR)SAMPLE_RAWREGS,
	(CONST_STRPTR)SAMPLE_VIDEOCAP,
	(CONST_STRPTR)SAMPLE_LPF_LEVEL,
	(CONST_STRPTR)SAMPLE_TEST_RESULT,
	(CONST_STRPTR)"Gradient",
	(CONST_STRPTR)"Even dark",
	(CONST_STRPTR)"Manual",
	NULL
};

static CONST_STRPTR zztop_button_samples[] = {
	(CONST_STRPTR)LABEL_BTN_TEST,
	(CONST_STRPTR)LABEL_BTN_REFRESH,
	NULL
};

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
char readout_bufs[MYGAD_COUNT][64];

static WORD zztop_max_word(WORD a, WORD b)
{
	return (a > b) ? a : b;
}

static WORD zztop_text_width(struct RastPort *rp, CONST_STRPTR text, WORD fallback_char_width)
{
	ULONG len;

	if (!text) return 0;

	len = strlen((const char *)text);
	if (rp && rp->Font) return TextLength(rp, text, len);

	return (WORD)(len * fallback_char_width);
}

static WORD zztop_max_text_width(struct RastPort *rp, CONST_STRPTR *texts, WORD fallback_char_width)
{
	WORD width = 0;

	while (*texts) {
		width = zztop_max_word(width, zztop_text_width(rp, *texts, fallback_char_width));
		texts++;
	}

	return width;
}

static void zztop_store_text_display(UWORD gadget_id, const char *text)
{
	if (gadget_id >= MYGAD_COUNT) return;

	snprintf(readout_bufs[gadget_id], sizeof(readout_bufs[gadget_id]),
		"%s", text ? text : "");
}

static void zztop_set_text_display(struct Window *win, UWORD gadget_id, const char *text)
{
	if (gadget_id >= MYGAD_COUNT || !gads[gadget_id]) return;

	zztop_store_text_display(gadget_id, text);
	GT_SetGadgetAttrs(gads[gadget_id], win, NULL,
		GTTX_Text, readout_bufs[gadget_id],
		TAG_END);
}

static void zztop_init_layout(struct ZZTopLayout *layout, struct Screen *screen)
{
	struct RastPort *rp = screen ? &screen->RastPort : NULL;
	const struct TextAttr *text_attr = (screen && screen->Font) ? screen->Font : &Topaz80;
	WORD font_x = (rp && rp->TxWidth) ? rp->TxWidth : 8;
	WORD font_y = (rp && rp->TxHeight) ? rp->TxHeight : text_attr->ta_YSize;
	WORD label_width;
	WORD value_width;
	WORD button_text_width;
	WORD text_padding;
	WORD button_gap;
	WORD button_window_width;
	WORD y;

	if (font_x < 1) font_x = 8;
	if (font_y < 1) font_y = 8;

	layout->text_attr = text_attr;
	layout->topborder = screen ? (UWORD)(screen->WBorTop + font_y + 1) : (UWORD)(font_y + 2);
	layout->margin_x = zztop_max_word(20, font_x * 2 + 4);
	layout->margin_y = zztop_max_word(20, font_y + 12);
	layout->label_gap = zztop_max_word(16, font_x * 2);
	layout->gadget_height = zztop_max_word(14, font_y + 6);
	layout->row_step = layout->gadget_height + zztop_max_word(6, font_y / 2);
	layout->section_gap = zztop_max_word(5, font_y / 2);
	layout->slider_step = layout->gadget_height + font_y + zztop_max_word(13, (font_y / 2) + 8);
	layout->control_step = layout->row_step + layout->section_gap;

	text_padding = zztop_max_word(32, font_x * 4);
	label_width = zztop_max_text_width(rp, zztop_label_samples, font_x);
	value_width = zztop_max_text_width(rp, zztop_value_samples, font_x);
	button_text_width = zztop_max_text_width(rp, zztop_button_samples, font_x);

	layout->gadget_left = layout->margin_x + label_width + layout->label_gap;
	layout->gadget_width = zztop_max_word(240, value_width + text_padding);
	layout->button_width = zztop_max_word(110, button_text_width + text_padding);

	button_gap = zztop_max_word(16, font_x * 3);
	button_window_width = layout->margin_x + layout->button_width +
		button_gap + layout->button_width + layout->margin_x;
	layout->window_width = zztop_max_word(
		layout->gadget_left + layout->gadget_width + layout->margin_x,
		button_window_width);

	y = layout->topborder + layout->margin_y;
	y += layout->row_step * 10;
	y += layout->section_gap;
	y += layout->slider_step;
	y += layout->control_step * 3;
	y += layout->section_gap;
	y += layout->row_step;
	y += layout->section_gap;
	layout->button_top = y;
	layout->window_height = layout->button_top + layout->gadget_height + (layout->margin_y / 2);
}

void errorMessage(const char* error)
{
	struct EasyStruct requester = {
		sizeof(struct EasyStruct),
		0,
		(UBYTE *)"ZZTop",
		NULL,
		(UBYTE *)"OK"
	};

	if (!error) return;

	if (IntuitionBase) {
		requester.es_TextFormat = (UBYTE *)error;
		EasyRequestArgs(NULL, &requester, NULL, NULL);
	} else {
		printf("Error: %s\n", error);
	}
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
	return zz_get_reg16(REG_ZZ_AUDIO_CONFIG) & 1;
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

BOOL zztop_restart_timer(void)
{
	if (zztop_refresh_seconds() == 0) {
		zztop_close_timer();
		return TRUE;
	}

	if (!TimerBase) {
		if (!zztop_open_timer()) return FALSE;
	}

	zztop_cancel_timer();
	zztop_schedule_timer();
	return TRUE;
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
	uint16_t raw_vcap = zz_get_reg16(REG_ZZ_VIDEOCAP_STATS);

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

	GT_SetGadgetAttrs(gads[MYGAD_ZORROVER], win, NULL, GTNM_Number, zorro_version, TAG_END);

	snprintf(txt_buf, 20, "ABI %d.%d", fwrev_major, fwrev_minor);
	zztop_set_text_display(win, MYGAD_FWVER, txt_buf);

	snprintf(txt_buf, 20, "%.1f", t_filt);
	zztop_set_text_display(win, MYGAD_TEMP, txt_buf);

	snprintf(txt_buf, 20, "%.1f / %.1f", t_min, t_max);
	zztop_set_text_display(win, MYGAD_TEMP_MINMAX, txt_buf);

	snprintf(txt_buf, 20, "%.2f", vaux);
	zztop_set_text_display(win, MYGAD_VAUX, txt_buf);

	snprintf(txt_buf, 20, "%.2f", vint);
	zztop_set_text_display(win, MYGAD_VINT, txt_buf);

	if (z9ax_present) {
		zztop_set_text_display(win, MYGAD_Z9AX, "Present");
	} else {
		zztop_set_text_display(win, MYGAD_Z9AX, "Not present");
	}

	snprintf(txt_buf, 64, "AX:%c USB:%04x SD:%04x B:%04x",
		z9ax_present ? 'Y' : 'N', raw_usb, raw_sd, raw_sd_boot);
	zztop_set_text_display(win, MYGAD_STATUS, txt_buf);

	snprintf(txt_buf, 64, "S:%04x P:%04x T:%04x A:%04x",
		raw_scanline, raw_parity, raw_temp, raw_vaux);
	zztop_set_text_display(win, MYGAD_RAWREGS, txt_buf);

	/* Videocap diagnostic readout (issue #11 genlock investigation).
	 * Pulse-width tiers are the per-field max and min, so a wide-sync
	 * reading is sticky across the frame and won't be missed by an
	 * unlucky sample. Two tiers let the reporter tell apart "all pulses
	 * wide" from "some pulses wide". */
	{
		uint16_t lines = raw_vcap & VCAP_LINES_MASK;
		uint16_t pw_max = (raw_vcap >> VCAP_PW_MAX_TIER_SHIFT) & VCAP_PW_TIER_MASK;
		uint16_t pw_min = (raw_vcap >> VCAP_PW_MIN_TIER_SHIFT) & VCAP_PW_TIER_MASK;
		snprintf(txt_buf, 64, "Lines:%u  Max:%u/%u  Min:%u/%u",
			lines, pw_max, VCAP_PW_TIER_MAX, pw_min, VCAP_PW_TIER_MAX);
		zztop_set_text_display(win, MYGAD_VIDEOCAP, txt_buf);
	}
}

ULONG zz_perform_register_probe(void)
{
	uint16_t fw = zz_get_reg16(REG_ZZ_FW_VERSION);
	uint16_t hw = zz_get_reg16(REG_ZZ_HW_VERSION);
	ULONG errors = 0;

	for (int i = 0; i < ZZTOP_PROBE_READS; i++) {
		if (zz_get_reg16(REG_ZZ_FW_VERSION) != fw) errors++;
		if (zz_get_reg16(REG_ZZ_HW_VERSION) != hw) errors++;
	}

	if (fw == 0 || fw == 0xffff || hw == 0xffff) errors++;

	return errors;
}

VOID handleGadgetEvent(struct Window *win, struct Gadget *gad, ULONG code)
{
	if (!gad) return;

	switch (gad->GadgetID)
	{
		case MYGAD_BTN_REFRESH: {
			refresh_zz_info(win);
			break;
		}
		case MYGAD_BTN_TEST: {
			ULONG errors;

			zztop_set_text_display(win, MYGAD_TEST_RESULT, "Reading...");
			errors = zz_perform_register_probe();
			if (errors == 0) {
				zztop_set_text_display(win, MYGAD_TEST_RESULT, "OK read-only");
			} else {
				snprintf(txt_buf, 20, "%lu read errs", (unsigned long)errors);
				zztop_set_text_display(win, MYGAD_TEST_RESULT, txt_buf);
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
			if (!zztop_restart_timer()) {
				refresh_mode = 0;
				zztop_set_text_display(win, MYGAD_TEST_RESULT, "No timer.device");
			}
			GT_SetGadgetAttrs(gads[MYGAD_REFRESHMODE], win, NULL,
				GTCY_Active, refresh_mode, TAG_END);
			break;
		}
	}
}

static struct Gadget *createTextReadoutGadget(struct Gadget *gad, struct NewGadget *ng,
	UWORD gadget_id, STRPTR label, const char *initial)
{
	if (gadget_id >= MYGAD_COUNT) return NULL;

	zztop_store_text_display(gadget_id, initial);
	ng->ng_GadgetID = gadget_id;
	ng->ng_GadgetText = label;

	return CreateGadget(TEXT_KIND, gad, ng,
		GTTX_Text, readout_bufs[gadget_id],
		GTTX_Border, TRUE,
		TAG_END);
}

struct Gadget *createAllGadgets(struct Gadget **glistptr, void *vi, const struct ZZTopLayout *layout)
{
	struct NewGadget ng;
	struct Gadget *gad;
	WORD y;

	gad = CreateContext(glistptr);

	y = layout->topborder + layout->margin_y;

	ng.ng_LeftEdge	 = layout->gadget_left;
	ng.ng_TopEdge		 = y;
	ng.ng_Width			 = layout->gadget_width;
	ng.ng_Height		 = layout->gadget_height;
	ng.ng_GadgetText = (STRPTR)LABEL_ZORROVER;
	ng.ng_TextAttr	 = layout->text_attr;
	ng.ng_VisualInfo = vi;
	ng.ng_GadgetID	 = MYGAD_ZORROVER;
	ng.ng_Flags			 = PLACETEXT_LEFT;

	gads[MYGAD_ZORROVER] = gad = CreateGadget(NUMBER_KIND, gad, &ng,
											GTNM_Number, 0,
											GTNM_Border, TRUE,
											TAG_END);
	y += layout->row_step;

	ng.ng_TopEdge	= y;
	gads[MYGAD_FWVER] = gad = createTextReadoutGadget(gad, &ng,
		MYGAD_FWVER, (STRPTR)LABEL_FWVER, "");
	y += layout->row_step;

	ng.ng_TopEdge	= y;
	gads[MYGAD_TEMP] = gad = createTextReadoutGadget(gad, &ng,
		MYGAD_TEMP, (STRPTR)LABEL_TEMP, "");
	y += layout->row_step;

	ng.ng_TopEdge	= y;
	gads[MYGAD_TEMP_MINMAX] = gad = createTextReadoutGadget(gad, &ng,
		MYGAD_TEMP_MINMAX, (STRPTR)LABEL_TEMP_MINMAX, "");
	y += layout->row_step;

	ng.ng_TopEdge	= y;
	gads[MYGAD_VAUX] = gad = createTextReadoutGadget(gad, &ng,
		MYGAD_VAUX, (STRPTR)LABEL_VAUX, "");
	y += layout->row_step;

	ng.ng_TopEdge	= y;
	gads[MYGAD_VINT] = gad = createTextReadoutGadget(gad, &ng,
		MYGAD_VINT, (STRPTR)LABEL_VINT, "");
	y += layout->row_step;

	ng.ng_TopEdge	= y;
	gads[MYGAD_Z9AX] = gad = createTextReadoutGadget(gad, &ng,
		MYGAD_Z9AX, (STRPTR)LABEL_Z9AX, "");
	y += layout->row_step;

	ng.ng_TopEdge	= y;
	gads[MYGAD_STATUS] = gad = createTextReadoutGadget(gad, &ng,
		MYGAD_STATUS, (STRPTR)LABEL_STATUS, "");
	y += layout->row_step;

	ng.ng_TopEdge	= y;
	gads[MYGAD_RAWREGS] = gad = createTextReadoutGadget(gad, &ng,
		MYGAD_RAWREGS, (STRPTR)LABEL_RAWREGS, "");
	y += layout->row_step;

	ng.ng_TopEdge	= y;
	gads[MYGAD_VIDEOCAP] = gad = createTextReadoutGadget(gad, &ng,
		MYGAD_VIDEOCAP, (STRPTR)LABEL_VIDEOCAP, "");
	y += layout->row_step + layout->section_gap;

	ng.ng_TopEdge	= y;
	ng.ng_GadgetID	= MYGAD_LPF;
	ng.ng_GadgetText = (STRPTR)LABEL_LPF;

	gads[MYGAD_LPF] = gad = CreateGadget(SLIDER_KIND, gad, &ng,
										GTSL_Min, 0,
										GTSL_Max, 23900,
										GTSL_Level, 23900,
										GTSL_LevelFormat, "%ld Hz",
										GTSL_MaxLevelLen, 10,
											GTSL_LevelPlace, PLACETEXT_BELOW,
											TAG_END);
	y += layout->slider_step;

	ng.ng_TopEdge	= y;
	ng.ng_GadgetID	= MYGAD_SCANLINES;
	ng.ng_GadgetText = (STRPTR)LABEL_SCANLINES;

	gads[MYGAD_SCANLINES] = gad = CreateGadget(CYCLE_KIND, gad, &ng,
											GTCY_Labels, scanline_labels,
											GTCY_Active, scanline_mode,
											TAG_END);
	y += layout->control_step;

	ng.ng_TopEdge	= y;
	ng.ng_GadgetID	= MYGAD_PARITY;
	ng.ng_GadgetText = (STRPTR)LABEL_PARITY;

	gads[MYGAD_PARITY] = gad = CreateGadget(CYCLE_KIND, gad, &ng,
											GTCY_Labels, parity_labels,
											GTCY_Active, scanline_parity,
											TAG_END);
	y += layout->control_step;

	ng.ng_TopEdge	= y;
	ng.ng_GadgetID	= MYGAD_REFRESHMODE;
	ng.ng_GadgetText = (STRPTR)LABEL_REFRESHMODE;

	gads[MYGAD_REFRESHMODE] = gad = CreateGadget(CYCLE_KIND, gad, &ng,
											GTCY_Labels, refresh_labels,
											GTCY_Active, refresh_mode,
											TAG_END);
	y += layout->control_step + layout->section_gap;

	ng.ng_TopEdge	= y;
	gads[MYGAD_TEST_RESULT] = gad = createTextReadoutGadget(gad, &ng,
		MYGAD_TEST_RESULT, (STRPTR)LABEL_TEST_RESULT, "Not run");

	ng.ng_LeftEdge	 = layout->margin_x;
	ng.ng_TopEdge		 = layout->button_top;
	ng.ng_Width			 = layout->button_width;
	ng.ng_GadgetText = (STRPTR)LABEL_BTN_TEST;
	ng.ng_GadgetID	 = MYGAD_BTN_TEST;
	ng.ng_Flags			 = PLACETEXT_IN;

	gads[MYGAD_BTN_TEST] = gad = CreateGadget(BUTTON_KIND, gad, &ng,
											TAG_END);

	ng.ng_LeftEdge	= layout->gadget_left;
	ng.ng_GadgetID	 = MYGAD_BTN_REFRESH;
	ng.ng_GadgetText = (STRPTR)LABEL_BTN_REFRESH;

	gads[MYGAD_BTN_REFRESH] = gad = CreateGadget(BUTTON_KIND, gad, &ng,
											TAG_END);

	for (int i=0; i<MYGAD_COUNT; i++) {
		if (!gads[i]) return NULL;
	}

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

	while (!terminated) {
		ULONG signals;

		timer_sig = TimerBase ? (1U << timerport->mp_SigBit) : 0;
		signals = Wait(user_sig | timer_sig);

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
	struct Screen		*mysc;
	struct Window		*mywin;
	struct Gadget		*glist = NULL;
	void						*vi;
	struct ZZTopLayout layout;

	if (NULL == (mysc = LockPubScreen(NULL)))
		errorMessage("Couldn't lock default public screen");
	else {
		if (NULL == (vi = GetVisualInfo(mysc, TAG_END)))
			errorMessage("GetVisualInfo() failed");
		else {
			zztop_init_layout(&layout, mysc);

			if (NULL == createAllGadgets(&glist, vi, &layout))
				errorMessage("createAllGadgets() failed");
			else {
				if (NULL == (mywin = OpenWindowTags(NULL,
						WA_Title,			"ZZTop " ZZTOP_RELEASE,
						WA_Gadgets,		glist,			WA_AutoAdjust,		TRUE,
						WA_Width,				layout.window_width,			WA_MinWidth,			 layout.window_width,
						WA_InnerHeight, layout.window_height,			WA_MinHeight,			 layout.window_height,
						WA_DragBar,		 TRUE,			WA_DepthGadget,		TRUE,
						WA_Activate,	 TRUE,			WA_CloseGadget,		TRUE,
						WA_SizeGadget, FALSE,			WA_SimpleRefresh, TRUE,
						WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW |
							IDCMP_VANILLAKEY | SLIDERIDCMP | BUTTONIDCMP |
							CYCLEIDCMP,
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

			if (glist) FreeGadgets(glist);
			FreeVisualInfo(vi);
		}
		UnlockPubScreen(NULL, mysc);
	}
}

int main(void) {
	if (NULL == (IntuitionBase = OpenLibrary((CONST_STRPTR)"intuition.library", 37))) {
		errorMessage("Requires V37 intuition.library");
		return 0;
	}

	if (!(ExpansionBase = (struct Library*)OpenLibrary((CONST_STRPTR)"expansion.library",0L))) {
		errorMessage("Requires expansion.library");
		CloseLibrary(IntuitionBase);
		return 0;
	}

	zz_cd = (struct ConfigDev*)FindConfigDev(zz_cd,0x6d6e,0x3);
	if (zz_cd) {
		zorro_version = 2;
	} else {
		zz_cd = (struct ConfigDev*)FindConfigDev(zz_cd,0x6d6e,0x4);
		if (zz_cd) {
			zorro_version = 3;
		} else {
			errorMessage("MNT ZZ9000 not found");
			CloseLibrary(ExpansionBase);
			CloseLibrary(IntuitionBase);
			return 0;
		}
	}

	zz_regs = (UBYTE*)zz_cd->cd_BoardAddr;
	CloseLibrary(ExpansionBase);

	/* Sync the controls with whatever mode the FPGA currently holds - the
	 * V2 bitstream keeps scanline state across soft resets, so a prior
	 * CLI or ZZTop session may have left a non-zero mode configured. */
	scanline_mode = zz_get_scanline_mode();
	scanline_parity = zz_get_scanline_parity();

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

	return 0;
}
