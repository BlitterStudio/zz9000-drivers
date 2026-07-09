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
#include <libraries/asl.h>
#include <devices/timer.h>

#include <clib/exec_protos.h>
#include <clib/graphics_protos.h>
#include <clib/intuition_protos.h>
#include <clib/gadtools_protos.h>
#include <clib/expansion_protos.h>

#include <clib/timer_protos.h>
#include <clib/asl_protos.h>
#include <clib/dos_protos.h>

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "zz9000.h"
#include "fwup_amiga.h"
#include "zzcfg_amiga.h"

#define ZZTOP_RELEASE "2.3"
#define ZZTOP_DATE    "09.07.2026"

static const char version[] __attribute__((used)) =
	"$VER: ZZTop " ZZTOP_RELEASE " (" ZZTOP_DATE ")\r\n";

/* Scanline mode/parity moved to the Settings window (Project menu). */
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
#define MYGAD_REFRESHMODE  (10)
#define MYGAD_BTN_TEST     (11)
#define MYGAD_BTN_REFRESH  (12)
#define MYGAD_TEST_RESULT  (13)
#define MYGAD_VIDEOCAP     (14)
#define MYGAD_BTN_UPDATE   (15)
#define MYGAD_BTN_RESTORE  (16)
#define MYGAD_FW_STATUS    (17)
#define MYGAD_COUNT        (18)

/* Settings window gadgets (own id space, own window). */
#define SGAD_VIDEOCAP      (0)
#define SGAD_NSVSYNC       (1)
#define SGAD_SCANMODE      (2)
#define SGAD_PARITY        (3)
#define SGAD_INT2          (4)
#define SGAD_MAC           (5)
#define SGAD_HDF           (6)
#define SGAD_CFG_STATUS    (7)
#define SGAD_BTN_SAVE      (8)
#define SGAD_BTN_RELOAD    (9)
#define SGAD_COUNT         (10)

/* Project menu userdata values. */
#define MENU_ID_SETTINGS   (1)
#define MENU_ID_QUIT       (2)

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
#define LABEL_VCAPMODE     "Native Video"
#define LABEL_NSVSYNC      "Exact Refresh"
#define LABEL_INT2         "Interrupt"
#define LABEL_MAC          "MAC Address"
#define LABEL_HDF          "SD HDF Image"
#define LABEL_BTN_SAVE     "Save"
#define LABEL_BTN_RELOAD   "Reload"
#define LABEL_TEST_RESULT  "Result"
#define LABEL_BTN_TEST     "Reg Probe"
#define LABEL_BTN_REFRESH  "Refresh"
#define LABEL_BTN_UPDATE   "Update Firmware"
#define LABEL_BTN_RESTORE  "Restore Backup"
#define LABEL_FW_STATUS    "Firmware Op"

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
#define SAMPLE_FW_STATUS   "Updating... 100% (9999 KB)"

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
	WORD button_col2;
	WORD button_top;
	WORD fw_button_top;
	WORD fw_status_top;
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
	(CONST_STRPTR)LABEL_FW_STATUS,
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
	(CONST_STRPTR)SAMPLE_FW_STATUS,
	NULL
};

static CONST_STRPTR zztop_button_samples[] = {
	(CONST_STRPTR)LABEL_BTN_TEST,
	(CONST_STRPTR)LABEL_BTN_REFRESH,
	(CONST_STRPTR)LABEL_BTN_UPDATE,
	(CONST_STRPTR)LABEL_BTN_RESTORE,
	NULL
};

struct Library* IntuitionBase;
struct Library* GfxBase;
struct Library* GadToolsBase;
struct Library* ExpansionBase;
struct Library* AslBase;

struct ConfigDev* zz_cd;
volatile UBYTE* zz_regs;
int zorro_version = 0;
uint16_t refresh_mode = 0;
double t_min = 0;
double t_max = 0;

char txt_buf[64];

struct timerequest * timerio;
struct MsgPort *timerport;
struct Library *TimerBase;
BOOL timer_pending = FALSE;
char readout_bufs[MYGAD_COUNT][64];

/* Shared with the Settings window (opened from the Project menu). */
static struct Screen *zztop_screen;
static void *zztop_vi;
static struct ZZTopLayout zztop_layout;
static struct Menu *zztop_menustrip;

static struct NewMenu zztop_newmenus[] = {
	{ NM_TITLE, (STRPTR)"Project",     NULL, 0, 0, NULL },
	{ NM_ITEM,  (STRPTR)"Settings...", (STRPTR)"S", 0, 0, (APTR)MENU_ID_SETTINGS },
	{ NM_ITEM,  NM_BARLABEL,           NULL, 0, 0, NULL },
	{ NM_ITEM,  (STRPTR)"Quit",        (STRPTR)"Q", 0, 0, (APTR)MENU_ID_QUIT },
	{ NM_END,   NULL,                  NULL, 0, 0, NULL }
};

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
	/* Second button column sits one full button + gap right of the first,
	 * so wider labels never overlap the left column. */
	layout->button_col2 = layout->margin_x + layout->button_width + button_gap;
	button_window_width = layout->margin_x + layout->button_width +
		button_gap + layout->button_width + layout->margin_x;
	layout->window_width = zztop_max_word(
		layout->gadget_left + layout->gadget_width + layout->margin_x,
		button_window_width);

	y = layout->topborder + layout->margin_y;
	y += layout->row_step * 10;
	y += layout->section_gap;
	y += layout->slider_step;
	y += layout->control_step;
	y += layout->section_gap;
	y += layout->row_step;
	y += layout->section_gap;
	layout->button_top = y;
	/* Second button row (firmware update/restore) + a status line below it. */
	layout->fw_button_top = layout->button_top + layout->row_step;
	layout->fw_status_top = layout->fw_button_top + layout->row_step;
	layout->window_height = layout->fw_status_top + layout->gadget_height + (layout->margin_y / 2);
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

static int fw_confirm(const char *text)
{
	struct EasyStruct es = {
		sizeof(struct EasyStruct), 0,
		(UBYTE *)"ZZTop Firmware",
		(UBYTE *)text,
		(UBYTE *)"Proceed|Cancel"
	};
	return (int)EasyRequestArgs(NULL, &es, NULL, NULL);
}

static int fw_pick_file(char *out, int outsz)
{
	struct TagItem tags[] = {
		{ ASLFR_TitleText,      (ULONG)"Select firmware file to upload" },
		{ ASLFR_DoPatterns,     TRUE },
		{ ASLFR_InitialPattern, (ULONG)"#?.(bin|rom|img)" },
		{ TAG_END,              0 }
	};
	struct FileRequester *fr;
	int ok = 0;

	if (!AslBase) {
		errorMessage("asl.library 37+ is required for the file requester.");
		return 0;
	}
	fr = (struct FileRequester *)AllocAslRequest(ASL_FileRequest, tags);
	if (!fr) return 0;
	if (AslRequest(fr, NULL)) {
		strncpy(out, (const char *)fr->fr_Drawer, outsz - 1);
		out[outsz - 1] = '\0';
		AddPart((STRPTR)out, fr->fr_File, (ULONG)outsz);
		ok = 1;
	}
	FreeAslRequest(fr);
	return ok;
}

static void fw_progress(void *ctx, ULONG done, LONG total)
{
	struct Window *win = (struct Window *)ctx;
	char buf[48];

	if (total > 0) {
		ULONG pct = (ULONG)(((ULONG)done * 100UL) / (ULONG)total);
		if (pct > 100) pct = 100;
		snprintf(buf, sizeof(buf), "Updating... %lu%% (%lu KB)",
			(unsigned long)pct, (unsigned long)(done / 1024));
	} else {
		snprintf(buf, sizeof(buf), "Updating... %lu KB",
			(unsigned long)(done / 1024));
	}
	zztop_set_text_display(win, MYGAD_FW_STATUS, buf);
}

static void do_fw_update(struct Window *win)
{
	char path[256];
	char msg[400];
	UWORD st;

	if (!fwup_probe_board((ULONG)zz_regs)) {
		errorMessage("This firmware does not support the file-push protocol.\n"
			"Update BOOT.bin to a firmware build with FWUP support first.");
		return;
	}
	if (!fw_pick_file(path, sizeof(path)))
		return;

	snprintf(msg, sizeof(msg),
		"Upload\n  %s\nto the ZZ9000 as BOOT.bin?\n\n"
		"Power-cycle the Amiga afterwards to boot the new firmware.", path);
	if (!fw_confirm(msg))
		return;

	zztop_set_text_display(win, MYGAD_FW_STATUS, "Updating...");
	st = fwup_send_file((ULONG)zz_regs, path, "BOOT.bin", fw_progress, win);
	if (st == FWUP_OK) {
		zztop_set_text_display(win, MYGAD_FW_STATUS, "Updated - power-cycle to boot");
		errorMessage("Firmware uploaded as BOOT.bin.\nPower-cycle the Amiga to boot it.");
	} else {
		snprintf(msg, sizeof(msg), "Firmware update failed:\n%s (0x%04x)",
			fwup_strerror(st), (unsigned)st);
		zztop_set_text_display(win, MYGAD_FW_STATUS, "Update failed");
		errorMessage(msg);
	}
}

static void do_fw_restore(struct Window *win)
{
	char msg[256];
	UWORD st;

	if (!fwup_probe_board((ULONG)zz_regs)) {
		errorMessage("This firmware does not support the file-push protocol.");
		return;
	}
	if (!fw_confirm("Restore the backup firmware (BOOT.bak) as the active BOOT.bin?\n\n"
			"The current BOOT.bin is discarded and no backup remains.\n"
			"Power-cycle the Amiga afterwards."))
		return;

	zztop_set_text_display(win, MYGAD_FW_STATUS, "Restoring...");
	st = fwup_restore_board((ULONG)zz_regs, "BOOT.bin");
	if (st == FWUP_OK) {
		zztop_set_text_display(win, MYGAD_FW_STATUS, "Restored - power-cycle to boot");
		errorMessage("Backup restored as BOOT.bin.\nPower-cycle the Amiga to boot it.");
	} else {
		snprintf(msg, sizeof(msg), "Firmware restore failed:\n%s (0x%04x)",
			fwup_strerror(st), (unsigned)st);
		zztop_set_text_display(win, MYGAD_FW_STATUS, "Restore failed");
		errorMessage(msg);
	}
}

/* ------------------------------------------------------------------ */
/* Settings window: edits ZZ9000.CFG on the SD card (issue #33).      */
/* Values load from the firmware's parsed config (cold-boot state)    */
/* plus the raw file; Save regenerates the file and pushes it over    */
/* the FWUP path. Scanline changes also apply live, everything else   */
/* takes effect on the next power cycle.                              */
/* ------------------------------------------------------------------ */

static STRPTR vcapmode_labels[] = {
	(STRPTR)"800x600 60Hz",
	(STRPTR)"PAL 720x576 50Hz",
	NULL
};

static STRPTR nsvsync_labels[] = {
	(STRPTR)"Off",
	(STRPTR)"PAL ~49.92Hz",
	(STRPTR)"NTSC",
	NULL
};

/* Explicit INT6/INT2 choice: index == the config `int2` value. */
static STRPTR interrupt_labels[] = {
	(STRPTR)"INT6 (default)",
	(STRPTR)"INT2",
	NULL
};

static struct Gadget *sgads[SGAD_COUNT];
static struct zzcfg_values settings_vals;
static char settings_status_buf[64];
static char settings_cfg_text[ZZCFG_MAX_SIZE];
/* ZZ9000.CFG needs firmware ABI 2.3+. On older firmware the window
 * still opens for the live scanline controls; the config-file fields
 * and Save/Reload are disabled. */
static BOOL settings_have_cfg;

static CONST_STRPTR settings_label_samples[] = {
	(CONST_STRPTR)LABEL_VCAPMODE,
	(CONST_STRPTR)LABEL_NSVSYNC,
	(CONST_STRPTR)LABEL_SCANLINES,
	(CONST_STRPTR)LABEL_PARITY,
	(CONST_STRPTR)LABEL_INT2,
	(CONST_STRPTR)LABEL_MAC,
	(CONST_STRPTR)LABEL_HDF,
	NULL
};

/* Sized from the widest cycle/string content only; the status line
 * spans the full row instead, so long messages don't inflate the
 * control column (and with it the whole window). */
static CONST_STRPTR settings_value_samples[] = {
	(CONST_STRPTR)"PAL 720x576 50Hz",
	(CONST_STRPTR)"INT6 (default)",
	(CONST_STRPTR)"aa:bb:cc:dd:ee:ff",
	NULL
};

static CONST_STRPTR settings_button_samples[] = {
	(CONST_STRPTR)LABEL_BTN_SAVE,
	(CONST_STRPTR)LABEL_BTN_RELOAD,
	NULL
};

static void settings_set_status(struct Window *win, const char *text)
{
	/* callers may pass settings_status_buf itself - don't self-copy */
	if (text != settings_status_buf) {
		snprintf(settings_status_buf, sizeof(settings_status_buf), "%s", text);
	}
	if (win && sgads[SGAD_CFG_STATUS]) {
		GT_SetGadgetAttrs(sgads[SGAD_CFG_STATUS], win, NULL,
			GTTX_Text, settings_status_buf, TAG_END);
	}
}

static int settings_parse_mac(const char *s)
{
	int i;

	for (i = 0; i < 6; i++) {
		int j;
		for (j = 0; j < 2; j++) {
			char c = s[i * 3 + j];
			if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
					(c >= 'A' && c <= 'F')))
				return 0;
		}
		if (i < 5 && s[i * 3 + 2] != ':' && s[i * 3 + 2] != '-')
			return 0;
	}
	return s[ZZCFG_MAC_CHARS] == '\0';
}

static int settings_env_exists(const char *path)
{
	BPTR f = Open((CONST_STRPTR)path, MODE_OLDFILE);
	if (!f) return 0;
	Close(f);
	return 1;
}

static int settings_env_read_mac(char *out, int outsz)
{
	BPTR f = Open((CONST_STRPTR)"ENV:ZZ9K_MAC", MODE_OLDFILE);
	LONG n;

	if (!f) return 0;
	n = Read(f, out, outsz - 1);
	Close(f);
	if (n < ZZCFG_MAC_CHARS) return 0;
	out[ZZCFG_MAC_CHARS] = '\0';
	return settings_parse_mac(out);
}

/* The drivers apply ENV: variables over the config file, so show the
 * effective values: pre-filling the editor from ENV both matches what
 * the system actually does and turns Save into the migration path
 * (values land in ZZ9000.CFG, then the ENV variables can go). Returns
 * 1 if any override is active so the status line can say so. */
static int settings_apply_env_overrides(struct zzcfg_values *sv)
{
	char envmac[ZZCFG_MAC_CHARS + 3];
	int any = 0;

	if (settings_env_exists("ENV:ZZ9000-VCAP-800x600")) {
		sv->videocap_pal = 0;
		any = 1;
	}
	if (settings_env_exists("ENV:ZZ9000-NS-VSYNC")) {
		sv->vsync = 1;
		any = 1;
	} else if (settings_env_exists("ENV:ZZ9000-NS-VSYNC-NTSC")) {
		sv->vsync = 2;
		any = 1;
	}
	if (settings_env_exists("ENV:ZZ9K_INT2")) {
		sv->int2 = 1;
		any = 1;
	}
	if (settings_env_read_mac(envmac, sizeof(envmac))) {
		strcpy(sv->mac, envmac);
		any = 1;
	}
	return any;
}

/* After a save the config file holds the effective values, so offer
 * to delete the ENV: variables that would keep overriding it. */
static void settings_offer_env_cleanup(void)
{
	static const char *env_names[] = {
		"ZZ9K_MAC", "ZZ9K_INT2", "ZZ9000-VCAP-800x600",
		"ZZ9000-NS-VSYNC", "ZZ9000-NS-VSYNC-NTSC", NULL
	};
	char path[40];
	char msg[400];
	int i, any = 0;

	snprintf(msg, sizeof(msg),
		"These ENV: variables override the saved ZZ9000.CFG "
		"whenever the drivers load:\n");
	for (i = 0; env_names[i]; i++) {
		snprintf(path, sizeof(path), "ENV:%s", env_names[i]);
		if (settings_env_exists(path)) {
			any = 1;
			snprintf(path, sizeof(path), "\n  %s", env_names[i]);
			strncat(msg, path, sizeof(msg) - strlen(msg) - 1);
		}
	}
	if (!any) return;

	strncat(msg, "\n\nDelete them (ENV: and ENVARC:) so the config "
		"file takes effect?", sizeof(msg) - strlen(msg) - 1);

	{
		struct EasyStruct es = {
			sizeof(struct EasyStruct), 0,
			(UBYTE *)"ZZ9000 Settings",
			(UBYTE *)msg,
			(UBYTE *)"Delete|Keep"
		};
		if (!EasyRequestArgs(NULL, &es, NULL, NULL)) return;
	}

	for (i = 0; env_names[i]; i++) {
		snprintf(path, sizeof(path), "ENV:%s", env_names[i]);
		DeleteFile((STRPTR)path);
		snprintf(path, sizeof(path), "ENVARC:%s", env_names[i]);
		DeleteFile((STRPTR)path);
	}
}

/* Populate settings_vals from the board and push into the gadgets. */
static void settings_populate(struct Window *win)
{
	ULONG board = (ULONG)zz_regs;
	struct zzcfg_values *sv = &settings_vals;
	UWORD st, rawlen = 0;

	/* Editor defaults; keys present in the raw file override them.
	 * Scanlines default to the live FPGA state - the config applied
	 * it at cold boot and this tool/ZZScanlines may have changed it
	 * since. */
	memset(sv, 0, sizeof(*sv));
	sv->scanline_mode = zz_get_scanline_mode();
	sv->scanline_parity = zz_get_scanline_parity();

	if (settings_have_cfg) {
		int env_active;

		st = zzcfg_read_raw(board, settings_cfg_text,
			sizeof(settings_cfg_text), &rawlen);
		if (st == ZZ_CFG_FILE_OK) {
			/* The raw SD file - not the firmware's cold-boot parse
			 * (REG_ZZ_CONFIG_KEY) - is the editor's source of truth:
			 * the query would revert values saved or externally
			 * edited since boot on every Reload, and a subsequent
			 * Save would then write those stale values back. */
			zzcfg_parse_text(settings_cfg_text, rawlen, sv);
			env_active = settings_apply_env_overrides(sv);
			if (env_active) {
				snprintf(settings_status_buf, sizeof(settings_status_buf),
					"%u bytes on card + ENV overrides", (unsigned)rawlen);
			} else {
				snprintf(settings_status_buf, sizeof(settings_status_buf),
					"ZZ9000.CFG: %u bytes on card", (unsigned)rawlen);
			}
		} else if (st == ZZ_CFG_FILE_NO_FILE) {
			env_active = settings_apply_env_overrides(sv);
			if (env_active) {
				snprintf(settings_status_buf, sizeof(settings_status_buf),
					"No file yet - showing ENV settings");
			} else {
				snprintf(settings_status_buf, sizeof(settings_status_buf),
					"No ZZ9000.CFG on card yet");
			}
		} else if (st == ZZ_CFG_FILE_IDLE) {
			snprintf(settings_status_buf, sizeof(settings_status_buf),
				"Firmware lacks config support");
		} else {
			snprintf(settings_status_buf, sizeof(settings_status_buf),
				"Config read failed (SD error)");
		}
	} else {
		snprintf(settings_status_buf, sizeof(settings_status_buf),
			"Scanlines only (needs FW 2.3+)");
	}

	if (!win) return;

	GT_SetGadgetAttrs(sgads[SGAD_VIDEOCAP], win, NULL,
		GTCY_Active, sv->videocap_pal, TAG_END);
	GT_SetGadgetAttrs(sgads[SGAD_NSVSYNC], win, NULL,
		GTCY_Active, sv->vsync, TAG_END);
	GT_SetGadgetAttrs(sgads[SGAD_SCANMODE], win, NULL,
		GTCY_Active, sv->scanline_mode, TAG_END);
	GT_SetGadgetAttrs(sgads[SGAD_PARITY], win, NULL,
		GTCY_Active, sv->scanline_parity, TAG_END);
	GT_SetGadgetAttrs(sgads[SGAD_INT2], win, NULL,
		GTCY_Active, sv->int2 ? 1 : 0, TAG_END);
	GT_SetGadgetAttrs(sgads[SGAD_MAC], win, NULL,
		GTST_String, sv->mac, TAG_END);
	GT_SetGadgetAttrs(sgads[SGAD_HDF], win, NULL,
		GTST_String, sv->hdf, TAG_END);
	settings_set_status(win, settings_status_buf);
}

static void settings_save(struct Window *win)
{
	struct zzcfg_values *sv = &settings_vals;
	struct StringInfo *si;
	UWORD st;

	if (!settings_have_cfg) {
		settings_set_status(win, "Config needs firmware 2.3+");
		return;
	}

	si = (struct StringInfo *)sgads[SGAD_MAC]->SpecialInfo;
	snprintf(sv->mac, sizeof(sv->mac), "%s", (const char *)si->Buffer);
	si = (struct StringInfo *)sgads[SGAD_HDF]->SpecialInfo;
	snprintf(sv->hdf, sizeof(sv->hdf), "%s", (const char *)si->Buffer);

	if (sv->mac[0] && !settings_parse_mac(sv->mac)) {
		settings_set_status(win, "Bad MAC - use aa:bb:cc:dd:ee:ff");
		return;
	}
	/* Firmware hdf rules, not the FWUP name rules: they differ (no
	 * leading '.', 63-char cap), and the firmware silently ignores a
	 * name it rejects at the next cold boot. */
	if (sv->hdf[0] && !zzcfg_hdf_name_valid(sv->hdf)) {
		settings_set_status(win, "Bad HDF name (flat root file)");
		return;
	}

	settings_set_status(win, "Saving...");
	st = zzcfg_save((ULONG)zz_regs, sv);
	if (st == FWUP_OK) {
		UWORD rawlen = 0;

		/* The file now holds the effective values - offer to drop the
		 * ENV variables that would keep overriding it. */
		settings_offer_env_cleanup();

		/* Read back for confirmation that the write landed. */
		if (zzcfg_read_raw((ULONG)zz_regs, settings_cfg_text,
				sizeof(settings_cfg_text), &rawlen) == ZZ_CFG_FILE_OK) {
			snprintf(settings_status_buf, sizeof(settings_status_buf),
				"Saved (%u bytes) - power-cycle", (unsigned)rawlen);
			settings_set_status(win, settings_status_buf);
		} else {
			settings_set_status(win, "Saved - power-cycle to apply");
		}
	} else {
		snprintf(settings_status_buf, sizeof(settings_status_buf),
			"Save failed: %s", fwup_strerror(st));
		settings_set_status(win, settings_status_buf);
	}
}

static struct Gadget *settings_create_gadgets(struct Gadget **glistptr,
	void *vi, const struct ZZTopLayout *mainlayout, WORD *out_w, WORD *out_h)
{
	struct NewGadget ng;
	struct Gadget *gad;
	struct ZZTopLayout l = *mainlayout;
	WORD label_width, value_width, button_width, y, i;
	WORD content_right, button_gap;

	/* Same font metrics as the main window, own column widths (the
	 * main window's are sized for its wider content, e.g. the
	 * "Update Firmware" button). */
	{
		struct RastPort *rp = zztop_screen ? &zztop_screen->RastPort : NULL;
		label_width = zztop_max_text_width(rp, settings_label_samples, 8);
		value_width = zztop_max_text_width(rp, settings_value_samples, 8);
		button_width = zztop_max_word(90,
			zztop_max_text_width(rp, settings_button_samples, 8) + 32);
	}
	l.gadget_left = l.margin_x + label_width + l.label_gap;
	l.gadget_width = zztop_max_word(160, value_width + 48);
	content_right = l.gadget_left + l.gadget_width;
	button_gap = 16;

	gad = CreateContext(glistptr);

	for (i = 0; i < SGAD_COUNT; i++) sgads[i] = NULL;

	y = l.topborder + l.margin_y;

	ng.ng_LeftEdge   = l.gadget_left;
	ng.ng_TopEdge    = y;
	ng.ng_Width      = l.gadget_width;
	ng.ng_Height     = l.gadget_height;
	ng.ng_TextAttr   = l.text_attr;
	ng.ng_VisualInfo = vi;
	ng.ng_Flags      = PLACETEXT_LEFT;

	ng.ng_GadgetID   = SGAD_VIDEOCAP;
	ng.ng_GadgetText = (STRPTR)LABEL_VCAPMODE;
	sgads[SGAD_VIDEOCAP] = gad = CreateGadget(CYCLE_KIND, gad, &ng,
		GTCY_Labels, vcapmode_labels, GTCY_Active, 0, TAG_END);
	y += l.row_step;

	ng.ng_TopEdge    = y;
	ng.ng_GadgetID   = SGAD_NSVSYNC;
	ng.ng_GadgetText = (STRPTR)LABEL_NSVSYNC;
	sgads[SGAD_NSVSYNC] = gad = CreateGadget(CYCLE_KIND, gad, &ng,
		GTCY_Labels, nsvsync_labels, GTCY_Active, 0, TAG_END);
	y += l.row_step;

	ng.ng_TopEdge    = y;
	ng.ng_GadgetID   = SGAD_SCANMODE;
	ng.ng_GadgetText = (STRPTR)LABEL_SCANLINES;
	sgads[SGAD_SCANMODE] = gad = CreateGadget(CYCLE_KIND, gad, &ng,
		GTCY_Labels, scanline_labels, GTCY_Active, 0, TAG_END);
	y += l.row_step;

	ng.ng_TopEdge    = y;
	ng.ng_GadgetID   = SGAD_PARITY;
	ng.ng_GadgetText = (STRPTR)LABEL_PARITY;
	sgads[SGAD_PARITY] = gad = CreateGadget(CYCLE_KIND, gad, &ng,
		GTCY_Labels, parity_labels, GTCY_Active, 0, TAG_END);
	y += l.row_step;

	ng.ng_TopEdge    = y;
	ng.ng_GadgetID   = SGAD_INT2;
	ng.ng_GadgetText = (STRPTR)LABEL_INT2;
	sgads[SGAD_INT2] = gad = CreateGadget(CYCLE_KIND, gad, &ng,
		GTCY_Labels, interrupt_labels, GTCY_Active, 0, TAG_END);
	y += l.row_step;

	ng.ng_TopEdge    = y;
	ng.ng_GadgetID   = SGAD_MAC;
	ng.ng_GadgetText = (STRPTR)LABEL_MAC;
	/* MaxChars includes the trailing NUL (intuition StringInfo), so add
	 * one or a full 17-char MAC could not be typed. */
	sgads[SGAD_MAC] = gad = CreateGadget(STRING_KIND, gad, &ng,
		GTST_MaxChars, ZZCFG_MAC_CHARS + 1, GTST_String, "", TAG_END);
	y += l.row_step;

	ng.ng_TopEdge    = y;
	ng.ng_GadgetID   = SGAD_HDF;
	ng.ng_GadgetText = (STRPTR)LABEL_HDF;
	sgads[SGAD_HDF] = gad = CreateGadget(STRING_KIND, gad, &ng,
		GTST_MaxChars, ZZCFG_HDF_CHARS + 1, GTST_String, "", TAG_END);
	y += l.row_step + l.section_gap;

	/* The status line spans the whole row (no side label) so messages
	 * get the label column's width too instead of widening the window. */
	ng.ng_LeftEdge   = l.margin_x;
	ng.ng_TopEdge    = y;
	ng.ng_Width      = content_right - l.margin_x;
	ng.ng_GadgetID   = SGAD_CFG_STATUS;
	ng.ng_GadgetText = NULL;
	sgads[SGAD_CFG_STATUS] = gad = CreateGadget(TEXT_KIND, gad, &ng,
		GTTX_Text, settings_status_buf, GTTX_Border, TRUE, TAG_END);
	y += l.row_step + l.section_gap;

	ng.ng_TopEdge    = y;
	ng.ng_Width      = button_width;
	ng.ng_GadgetID   = SGAD_BTN_SAVE;
	ng.ng_GadgetText = (STRPTR)LABEL_BTN_SAVE;
	ng.ng_Flags      = PLACETEXT_IN;
	sgads[SGAD_BTN_SAVE] = gad = CreateGadget(BUTTON_KIND, gad, &ng,
		TAG_END);

	ng.ng_LeftEdge   = l.margin_x + button_width + button_gap;
	ng.ng_GadgetID   = SGAD_BTN_RELOAD;
	ng.ng_GadgetText = (STRPTR)LABEL_BTN_RELOAD;
	sgads[SGAD_BTN_RELOAD] = gad = CreateGadget(BUTTON_KIND, gad, &ng,
		TAG_END);
	y += l.row_step;

	*out_w = zztop_max_word(content_right + l.margin_x,
		l.margin_x + button_width + button_gap + button_width + l.margin_x);
	*out_h = y + l.gadget_height + (l.margin_y / 2);

	for (i = 0; i < SGAD_COUNT; i++) {
		if (!sgads[i]) return NULL;
	}

	return gad;
}

static VOID settings_window(struct Screen *mysc, void *vi,
	const struct ZZTopLayout *mainlayout)
{
	struct Window *win;
	struct Gadget *glist = NULL;
	struct IntuiMessage *imsg;
	struct Gadget *gad;
	ULONG imsgClass;
	UWORD imsgCode;
	BOOL done = FALSE;
	WORD w = 0, h = 0;

	settings_have_cfg = (zz_get_reg16(REG_ZZ_FW_VERSION) >= 0x0203);

	if (NULL == settings_create_gadgets(&glist, vi, mainlayout, &w, &h)) {
		if (glist) FreeGadgets(glist);
		errorMessage("Settings: gadget creation failed");
		return;
	}

	win = OpenWindowTags(NULL,
		WA_Title,        "ZZ9000 Settings (SD card)",
		WA_Gadgets,      glist,   WA_AutoAdjust,    TRUE,
		WA_Width,        w,       WA_MinWidth,      w,
		WA_InnerHeight,  h,       WA_MinHeight,     h,
		WA_DragBar,      TRUE,    WA_DepthGadget,   TRUE,
		WA_Activate,     TRUE,    WA_CloseGadget,   TRUE,
		WA_SizeGadget,   FALSE,   WA_SimpleRefresh, TRUE,
		WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW |
			BUTTONIDCMP | CYCLEIDCMP | STRINGIDCMP,
		WA_PubScreen, mysc,
		TAG_END);
	if (!win) {
		FreeGadgets(glist);
		errorMessage("Settings: OpenWindow() failed");
		return;
	}

	settings_populate(win);

	if (!settings_have_cfg) {
		/* Live scanline controls stay usable on 2.0-2.2 firmware
		 * (they were on the main window before); everything that
		 * needs the config-file interface is greyed out. */
		static const UWORD cfg_only_gadgets[] = {
			SGAD_VIDEOCAP, SGAD_NSVSYNC, SGAD_INT2,
			SGAD_MAC, SGAD_HDF, SGAD_BTN_SAVE, SGAD_BTN_RELOAD
		};
		size_t i;
		for (i = 0; i < sizeof(cfg_only_gadgets) / sizeof(cfg_only_gadgets[0]); i++) {
			GT_SetGadgetAttrs(sgads[cfg_only_gadgets[i]], win, NULL,
				GA_Disabled, TRUE, TAG_END);
		}
	}

	GT_RefreshWindow(win, NULL);

	while (!done) {
		Wait(1UL << win->UserPort->mp_SigBit);

		while ((imsg = GT_GetIMsg(win->UserPort))) {
			gad = (struct Gadget *)imsg->IAddress;
			imsgClass = imsg->Class;
			imsgCode = imsg->Code;
			GT_ReplyIMsg(imsg);

			switch (imsgClass) {
				case IDCMP_GADGETUP:
					if (!gad) break;
					switch (gad->GadgetID) {
						case SGAD_VIDEOCAP:
							settings_vals.videocap_pal = imsgCode;
							break;
						case SGAD_NSVSYNC:
							settings_vals.vsync = imsgCode;
							break;
						case SGAD_SCANMODE:
							/* live, like the old main-window control */
							settings_vals.scanline_mode = imsgCode;
							zz_set_scanline_mode(imsgCode);
							break;
						case SGAD_PARITY:
							settings_vals.scanline_parity = imsgCode;
							zz_set_scanline_parity(imsgCode);
							break;
						case SGAD_INT2:
							settings_vals.int2 = imsgCode;
							break;
						case SGAD_BTN_SAVE:
							settings_save(win);
							break;
						case SGAD_BTN_RELOAD:
							settings_populate(win);
							break;
					}
					break;
				case IDCMP_CLOSEWINDOW:
					done = TRUE;
					break;
				case IDCMP_REFRESHWINDOW:
					GT_BeginRefresh(win);
					GT_EndRefresh(win, TRUE);
					break;
			}
		}
	}

	CloseWindow(win);
	FreeGadgets(glist);
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
		case MYGAD_BTN_UPDATE: {
			do_fw_update(win);
			break;
		}
		case MYGAD_BTN_RESTORE: {
			do_fw_restore(win);
			break;
		}
		case MYGAD_LPF: {
			zz_set_lpf_freq(code);
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

	ng.ng_LeftEdge	= layout->button_col2;
	ng.ng_GadgetID	 = MYGAD_BTN_REFRESH;
	ng.ng_GadgetText = (STRPTR)LABEL_BTN_REFRESH;

	gads[MYGAD_BTN_REFRESH] = gad = CreateGadget(BUTTON_KIND, gad, &ng,
											TAG_END);

	/* Firmware update / restore (issue #26): a second button row mirroring
	 * the Test/Refresh row, plus a status line below it. */
	ng.ng_LeftEdge	 = layout->margin_x;
	ng.ng_TopEdge		 = layout->fw_button_top;
	ng.ng_Width			 = layout->button_width;
	ng.ng_GadgetText = (STRPTR)LABEL_BTN_UPDATE;
	ng.ng_GadgetID	 = MYGAD_BTN_UPDATE;
	ng.ng_Flags			 = PLACETEXT_IN;
	gads[MYGAD_BTN_UPDATE] = gad = CreateGadget(BUTTON_KIND, gad, &ng,
											TAG_END);

	ng.ng_LeftEdge	 = layout->button_col2;
	ng.ng_GadgetID	 = MYGAD_BTN_RESTORE;
	ng.ng_GadgetText = (STRPTR)LABEL_BTN_RESTORE;
	gads[MYGAD_BTN_RESTORE] = gad = CreateGadget(BUTTON_KIND, gad, &ng,
											TAG_END);

	ng.ng_LeftEdge	 = layout->gadget_left;
	ng.ng_TopEdge		 = layout->fw_status_top;
	ng.ng_Width			 = layout->gadget_width;
	ng.ng_Flags			 = PLACETEXT_LEFT;
	gads[MYGAD_FW_STATUS] = gad = createTextReadoutGadget(gad, &ng,
		MYGAD_FW_STATUS, (STRPTR)LABEL_FW_STATUS, "idle");

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
				case IDCMP_MENUPICK: {
					UWORD menuNumber = imsgCode;
					while (menuNumber != MENUNULL && zztop_menustrip) {
						struct MenuItem *item = ItemAddress(zztop_menustrip, menuNumber);
						if (!item) break;
						switch ((ULONG)GTMENUITEM_USERDATA(item)) {
							case MENU_ID_SETTINGS:
								settings_window(zztop_screen, zztop_vi, &zztop_layout);
								break;
							case MENU_ID_QUIT:
								terminated = TRUE;
								break;
						}
						menuNumber = item->NextSelect;
					}
					break;
				}
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

	if (NULL == (mysc = LockPubScreen(NULL)))
		errorMessage("Couldn't lock default public screen");
	else {
		if (NULL == (vi = GetVisualInfo(mysc, TAG_END)))
			errorMessage("GetVisualInfo() failed");
		else {
			zztop_init_layout(&zztop_layout, mysc);
			zztop_screen = mysc;
			zztop_vi = vi;

			/* Menu strip is optional: without it the tool still works,
			 * just without the Settings window. */
			zztop_menustrip = CreateMenus(zztop_newmenus, TAG_END);
			if (zztop_menustrip &&
					!LayoutMenus(zztop_menustrip, vi, GTMN_NewLookMenus, TRUE, TAG_END)) {
				FreeMenus(zztop_menustrip);
				zztop_menustrip = NULL;
			}

			if (NULL == createAllGadgets(&glist, vi, &zztop_layout))
				errorMessage("createAllGadgets() failed");
			else {
				if (NULL == (mywin = OpenWindowTags(NULL,
						WA_Title,			"ZZTop " ZZTOP_RELEASE,
						WA_Gadgets,		glist,			WA_AutoAdjust,		TRUE,
						WA_Width,				zztop_layout.window_width,			WA_MinWidth,			 zztop_layout.window_width,
						WA_InnerHeight, zztop_layout.window_height,			WA_MinHeight,			 zztop_layout.window_height,
						WA_DragBar,		 TRUE,			WA_DepthGadget,		TRUE,
						WA_Activate,	 TRUE,			WA_CloseGadget,		TRUE,
						WA_SizeGadget, FALSE,			WA_SimpleRefresh, TRUE,
						/* Renders the menu strip with the new-look pens
						 * (black on white). GTMN_NewLookMenus at
						 * LayoutMenus() only handles the layout half;
						 * without this tag Intuition draws the old-style
						 * pens - black on black on many screens. Unknown
						 * (thus ignored) on V37, where menus stay
						 * old-look. */
						WA_NewLookMenus, TRUE,
						WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW |
							IDCMP_VANILLAKEY | IDCMP_MENUPICK | SLIDERIDCMP |
							BUTTONIDCMP | CYCLEIDCMP,
						WA_PubScreen, mysc,
						TAG_END))) {
					errorMessage("OpenWindow() failed");
				} else {
					if (zztop_menustrip) SetMenuStrip(mywin, zztop_menustrip);
					refresh_zz_info(mywin);
					GT_RefreshWindow(mywin, NULL);
					process_window_events(mywin);
					if (zztop_menustrip) ClearMenuStrip(mywin);
					CloseWindow(mywin);
				}
			}

			if (zztop_menustrip) {
				FreeMenus(zztop_menustrip);
				zztop_menustrip = NULL;
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

	if (NULL == (GfxBase = OpenLibrary((CONST_STRPTR)"graphics.library", 37)))
		errorMessage("Requires V37 graphics.library");
	else {
		if (NULL == (GadToolsBase = OpenLibrary((CONST_STRPTR)"gadtools.library", 37)))
			errorMessage("Requires V37 gadtools.library");
		else {
			/* asl.library is optional: only the firmware file requester
			 * needs it, so a missing one just disables the Update picker
			 * rather than blocking the tool. */
			AslBase = OpenLibrary((CONST_STRPTR)"asl.library", 37);
			gadtoolsWindow();
			if (AslBase) CloseLibrary(AslBase);
			CloseLibrary(GadToolsBase);
		}
		CloseLibrary(GfxBase);
	}
	CloseLibrary(IntuitionBase);

	return 0;
}
