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
#include <intuition/screens.h>
#include <graphics/displayinfo.h>
#include <graphics/rastport.h>
#include <libraries/gadtools.h>
#include <libraries/asl.h>
#include <devices/timer.h>
#include <devices/inputevent.h>

#include <clib/debug_protos.h>
#include <clib/graphics_protos.h>
#include <clib/intuition_protos.h>
#include <clib/gadtools_protos.h>
#include <clib/expansion_protos.h>

#include <clib/timer_protos.h>
#include <clib/asl_protos.h>
#include <clib/dos_protos.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "zz9000.h"
#include "fwup_amiga.h"
#include "zzcfg_amiga.h"
#include "zz_vcap_live.h"
/* zz9k.library client surface for the firmware audio control plane
 * (the Audio window). The staged zz9k-headers tree is synced from the
 * SDK checkout by build-gcc.sh, like the AHI/MHI drivers. */
#include "zz9k/library_vectors.h"
#include "zz9k/request.h"
#include <proto/zz9k.h>
#include <math.h>

#define ZZTOP_RELEASE "2.8"
#define ZZTOP_DATE    "11.08.2026"

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
#define MYGAD_REFRESHMODE  (9)
#define MYGAD_BTN_TEST     (10)
#define MYGAD_BTN_REFRESH  (11)
#define MYGAD_TEST_RESULT  (12)
#define MYGAD_VIDEOCAP     (13)
#define MYGAD_BTN_UPDATE   (14)
#define MYGAD_BTN_RESTORE  (15)
/* Opens the Audio window; the LPF slider that used to sit on the main
 * window moved there with the rest of the master-chain controls. */
#define MYGAD_FW_STATUS    (16)
#define MYGAD_BTN_AUDIO    (17)
#define MYGAD_COUNT        (18)

/* Settings window gadgets (own id space, own window). */
#define SGAD_VIDEOCAP      (0)
#define SGAD_VCAP_ADVANCED (1)
#define SGAD_SCANMODE      (2)
#define SGAD_PARITY        (3)
#define SGAD_INT2          (4)
#define SGAD_MAC           (5)
#define SGAD_HDF           (6)
#define SGAD_OFFSCREEN     (7)
#define SGAD_OVERLAY       (8)
#define SGAD_CFG_STATUS    (9)
#define SGAD_BTN_SAVE      (10)
#define SGAD_BTN_RELOAD    (11)
#define SGAD_COUNT         (12)

/* Advanced native-video window gadgets. */
#define AGAD_VCAP_SAMPLE   (0)
#define AGAD_VCAP_FRAMING  (1)
#define AGAD_VCAP_CROP_H   (2)
#define AGAD_VCAP_CROP_V   (3)
#define AGAD_BTN_CALIBRATE (4)
#define AGAD_STATUS        (5)
#define AGAD_BTN_DONE      (6)
#define AGAD_BTN_CANCEL    (7)
#define AGAD_COUNT         (8)

/* Audio window gadgets (own id space, own window). */
#define AUDGAD_SCENE        (0)
#define AUDGAD_BTN_EDIT     (1)
#define AUDGAD_OUT_PEAK     (2)
#define AUDGAD_OUT_COUNTS   (3)
#define AUDGAD_IN_PEAK      (4)
#define AUDGAD_IN_COUNTS    (5)
#define AUDGAD_GAIN_RED     (6)
#define AUDGAD_BASE_PAULA   (7)
#define AUDGAD_BASE_AX      (8)
#define AUDGAD_CEIL_PAULA   (9)
#define AUDGAD_CEIL_AX      (10)
#define AUDGAD_STATUS       (11)
#define AUDGAD_BTN_SAVE     (12)
#define AUDGAD_BTN_RENAME   (13)
#define AUDGAD_BTN_BALANCE  (14)
#define AUDGAD_COUNT        (15)

/* Scene-editor window gadgets (sub-window of the Audio window). */
#define SEGAD_LPF           (0)
#define SEGAD_PREFACTOR     (1)
#define SEGAD_VOLUME        (2)
#define SEGAD_PAN           (3)
#define SEGAD_EQ_BASE       (4)  /* SEGAD_EQ_BASE + 0..9 = bands 1..10 */
#define SEGAD_STATUS        (14)
#define SEGAD_BTN_DONE      (15)
#define SEGAD_COUNT         (16)

/* Scene-rename dialog gadgets (sub-window of the Audio window). */
#define RNGAD_NAME          (0)
#define RNGAD_BTN_OK        (1)
#define RNGAD_BTN_CANCEL    (2)
#define RNGAD_COUNT         (3)

#define VCAP_RAWKEY_KEYPAD_ENTER 0x43
#define VCAP_RAWKEY_RETURN       0x44
#define VCAP_RAWKEY_ESCAPE       0x45
#define VCAP_RAWKEY_UP           0x4c
#define VCAP_RAWKEY_DOWN         0x4d
#define VCAP_RAWKEY_RIGHT        0x4e
#define VCAP_RAWKEY_LEFT         0x4f
#define VCAP_APPLY_TIMEOUT_TICKS 100

/* Project menu userdata values. */
#define MENU_ID_SETTINGS   (1)
#define MENU_ID_QUIT       (2)
#define MENU_ID_FWUPDATE   (3)
#define MENU_ID_FWRESTORE  (4)
#define MENU_ID_AUDIOLOG   (5)

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
#define LABEL_SCANLINES    "Scanlines"
#define LABEL_PARITY       "Parity"
#define LABEL_REFRESHMODE  "Auto Refresh"
#define LABEL_VCAPMODE     "Native Output"
#define LABEL_VCAP_SAMPLE  "Capture Sample"
#define LABEL_VCAP_FRAMING "Framing"
#define LABEL_VCAP_CROP    "Crop H / V"
#define LABEL_VCAP_ADVANCED "Advanced Video..."
#define LABEL_INT2         "Interrupt"
#define LABEL_OFFSCREEN    "Offscreen BMs"
#define LABEL_OVERLAY      "Video overlay"
#define LABEL_MAC          "MAC Address"
#define LABEL_HDF          "SD HDF Image"
#define LABEL_BTN_SAVE     "Save"
#define LABEL_BTN_BALANCE  "Balanced"
#define LABEL_BTN_RELOAD   "Reload"
#define LABEL_TEST_RESULT  "Result"
#define LABEL_BTN_TEST     "Reg Probe"
#define LABEL_BTN_REFRESH  "Refresh"
#define LABEL_BTN_UPDATE   "Update Firmware"
#define LABEL_BTN_RESTORE  "Restore Backup"
#define LABEL_FW_STATUS    "Firmware Op"
#define LABEL_BTN_AUDIO    "Audio..."
#define LABEL_AUDIO_SCENE  "Scene"
#define LABEL_BTN_EDITSCN  "Edit..."
#define LABEL_BTN_RENAME   "Rename..."
#define LABEL_AUD_OUT_PEAK "Out Peak L/R"
#define LABEL_AUD_OUT_CNT  "Out Clip/Urun"
#define LABEL_AUD_IN_PEAK  "In Peak L/R"
#define LABEL_AUD_IN_CNT   "In Clip/Over"
#define LABEL_AUD_GR       "Gain Red"
#define LABEL_AUD_PAULA    "Paula Level"
#define LABEL_AUD_AX       "AX Level"
#define LABEL_AUD_CEIL_PAULA "Paula Ceiling"
#define LABEL_AUD_CEIL_AX    "AX Ceiling"
#define LABEL_AUD_DONE     "Done"
#define LABEL_AUD_PREF     "Prefactor"
#define LABEL_AUD_VOL      "Volume"
#define LABEL_AUD_PAN      "Pan"
#define LABEL_AUD_LPF      "AX Lowpass"

/* Firmware scene-name cap: 8 staged chunks of two chars each. */
#define ZZTOP_AUDIO_NAME_CHARS   16
#define ZZTOP_AUDIO_NAME_CHUNKS  8

#define SAMPLE_FWVER       "ABI 255.255"
#define SAMPLE_TEMP        "999.9"
#define SAMPLE_TEMP_MINMAX "999.9 / 999.9"
#define SAMPLE_VOLTAGE     "99.99"
#define SAMPLE_Z9AX        "Not present"
#define SAMPLE_STATUS      "AX:Y USB:ffff SD:ffff B:ffff"
#define SAMPLE_RAWREGS     "S:ffff P:ffff T:ffff A:ffff"
#define SAMPLE_VIDEOCAP    "Lines:1023  Max:3/3  Min:3/3"
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
	WORD control_step;
	WORD button_width;
	WORD button_col2;
	WORD button_col3;
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
	(CONST_STRPTR)SAMPLE_TEST_RESULT,
	(CONST_STRPTR)"Gradient",
	(CONST_STRPTR)"Even dark",
	(CONST_STRPTR)"Manual",
	(CONST_STRPTR)SAMPLE_FW_STATUS,
	NULL
};

static CONST_STRPTR zztop_button_samples[] = {
	(CONST_STRPTR)LABEL_BTN_TEST,
	(CONST_STRPTR)LABEL_BTN_RESTORE,
	(CONST_STRPTR)LABEL_BTN_AUDIO,
	(CONST_STRPTR)LABEL_BTN_UPDATE,
	NULL
};

struct Library* IntuitionBase;
struct Library* GfxBase;
struct Library* GadToolsBase;
struct Library* AslBase;
/* zz9k.library base for the proto inline calls; opened once at startup
 * (optional, like asl.library) for the Audio control-plane window. */
struct Library* ZZ9KBase = NULL;

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
	/* Also on buttons near the bottom of the window. Duplicated here so
	 * they stay reachable on a short screen (PAL HighRes) where the
	 * button row can be below the visible area. */
	{ NM_ITEM,  (STRPTR)"Update Firmware...", (STRPTR)"U", 0, 0, (APTR)MENU_ID_FWUPDATE },
	{ NM_ITEM,  (STRPTR)"Restore Backup...",  (STRPTR)"R", 0, 0, (APTR)MENU_ID_FWRESTORE },
	{ NM_ITEM,  NM_BARLABEL,           NULL, 0, 0, NULL },
	{ NM_ITEM,  (STRPTR)"Audio Debug Log", (STRPTR)"A", CHECKIT, 0, (APTR)MENU_ID_AUDIOLOG },
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

/* Height a window may occupy on this screen, excluding its own title bar.
 * WA_InnerHeight is measured below that bar, so the comparison has to be
 * against the same thing the caller passes. */
static WORD zztop_usable_height(struct Screen *screen)
{
	WORD h = screen ? screen->Height : 256;
	WORD bar = screen ? (WORD)(screen->BarHeight + 1) : 11;

	return (WORD)(h - bar);
}

/* All the vertical spacing in one place, in two flavours. `compact` keeps
 * gadget_height (GadTools needs it for a usable cycle/string gadget) and
 * takes the reduction out of the padding between things. */
static void zztop_set_vertical_metrics(struct ZZTopLayout *layout, WORD font_y,
	BOOL compact)
{
	layout->margin_y = compact
		? zztop_max_word(6, font_y - 2)
		: zztop_max_word(20, font_y + 12);
	layout->gadget_height = zztop_max_word(14, font_y + 6);
	layout->row_step = layout->gadget_height + (compact
		? zztop_max_word(2, font_y / 4)
		: zztop_max_word(6, font_y / 2));
	layout->section_gap = compact
		? zztop_max_word(2, font_y / 4)
		: zztop_max_word(5, font_y / 2);
	layout->control_step = layout->row_step + layout->section_gap;
}

/* Walk the main window's rows and derive its height. Split out so the
 * compact retry can re-run it without duplicating the row list. */
static void zztop_place_rows(struct ZZTopLayout *layout)
{
	WORD y = layout->topborder + layout->margin_y;

	y += layout->row_step * 10;
	y += layout->section_gap;
	y += layout->control_step;
	y += layout->section_gap;
	y += layout->row_step;
	y += layout->section_gap;
	layout->button_top = y;
	/* Second button row (firmware update/restore) + a status line below it. */
	layout->fw_button_top = layout->button_top + layout->row_step;
	layout->fw_status_top = layout->fw_button_top + layout->row_step;
	/* Gadget coordinates are window-relative and start below the title
	 * bar, so the content spans topborder..bottom. This value is passed as
	 * WA_InnerHeight, which excludes that bar - leaving topborder in would
	 * add exactly that much dead space under the last row. */
	layout->window_height = layout->fw_status_top + layout->gadget_height
		+ (layout->margin_y / 2) - layout->topborder;
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

	if (font_x < 1) font_x = 8;
	if (font_y < 1) font_y = 8;

	layout->text_attr = text_attr;
	layout->topborder = screen ? (UWORD)(screen->WBorTop + font_y + 1) : (UWORD)(font_y + 2);
	layout->margin_x = zztop_max_word(20, font_x * 2 + 4);
	layout->label_gap = zztop_max_word(16, font_x * 2);
	zztop_set_vertical_metrics(layout, font_y, FALSE);

	text_padding = zztop_max_word(32, font_x * 4);
	label_width = zztop_max_text_width(rp, zztop_label_samples, font_x);
	value_width = zztop_max_text_width(rp, zztop_value_samples, font_x);
	button_text_width = zztop_max_text_width(rp, zztop_button_samples, font_x);

	layout->gadget_left = layout->margin_x + label_width + layout->label_gap;
	layout->gadget_width = zztop_max_word(240, value_width + text_padding);
	layout->button_width = zztop_max_word(110, button_text_width + text_padding);

	button_gap = zztop_max_word(16, font_x * 3);
	/* Second and third button columns sit one full button + gap right of
	 * the previous, so wider labels never overlap the left column. */
	layout->button_col2 = layout->margin_x + layout->button_width + button_gap;
	layout->button_col3 = layout->button_col2 + layout->button_width + button_gap;
	button_window_width = layout->margin_x + layout->button_width +
		button_gap + layout->button_width + button_gap +
		layout->button_width + layout->margin_x;
	layout->window_width = zztop_max_word(
		layout->gadget_left + layout->gadget_width + layout->margin_x,
		button_window_width);

	zztop_place_rows(layout);

	/* PAL HighRes (640x256) is a real target: it is what you get with no
	 * startup-sequence, or when the RTG driver has not come up. The roomy
	 * spacing above does not fit there, so fall back to compact metrics
	 * rather than opening a window taller than the screen. Large screens
	 * keep the comfortable layout. */
	if (screen && layout->window_height > zztop_usable_height(screen)) {
		zztop_set_vertical_metrics(layout, font_y, TRUE);
		zztop_place_rows(layout);
	}
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
	(STRPTR)"1280x1024 Fixed 60Hz (Full detail)",
	(STRPTR)"1280x1024 Match PAL/NTSC (Full detail)",
	(STRPTR)"800x600 60Hz (Filtered)",
	(STRPTR)"720x576 50Hz (Filtered)",
	(STRPTR)"Exact PAL Amiga (Filtered)",
	(STRPTR)"Exact NTSC Amiga (Filtered)",
	(STRPTR)"1280x1024 Centered in 1080p60",
	NULL
};

static STRPTR vcapmode_legacy_labels[] = {
	(STRPTR)"1280x1024 Fixed 60Hz (Full detail)",
	(STRPTR)"1280x1024 Match PAL/NTSC (Full detail)",
	(STRPTR)"800x600 60Hz (Filtered)",
	(STRPTR)"720x576 50Hz (Filtered)",
	(STRPTR)"Exact PAL Amiga (Filtered)",
	(STRPTR)"Exact NTSC Amiga (Filtered)",
	NULL
};

static STRPTR vcapsample_labels[] = {
	(STRPTR)"Average (recommended)",
	(STRPTR)"Even (diagnostic)",
	(STRPTR)"Odd (diagnostic)",
	NULL
};

static STRPTR vcapframing_labels[] = {
	(STRPTR)"Automatic (recommended)",
	(STRPTR)"Custom",
	NULL
};

/* Feature kill-switches: index == the config value, so 1 is enabled -
 * which is also what ZZ9000.card assumes when the key is absent. */
static STRPTR enable_labels[] = {
	(STRPTR)"Disabled",
	(STRPTR)"Enabled",
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

enum vcap_apply_result {
	VCAP_APPLY_ERROR = -2,
	VCAP_APPLY_CONFLICT = -1,
	VCAP_APPLY_TIMEOUT = 0,
	VCAP_APPLY_OK = 1
};

struct settings_live_session {
	BOOL supported;
	struct zz_vcap_snapshot current;
	struct zz_vcap_anchors anchors;
	struct zz_vcap_control preview_control;
	BOOL preview_valid;
};

static ULONG vcap_read32(ULONG offset)
{
	ULONG high = zz_get_reg16(offset);
	ULONG low = zz_get_reg16(offset + 2);

	return (high << 16) | low;
}

static BOOL vcap_read_stable_status(ULONG *status)
{
	ULONG first = vcap_read32(ZZ_VCAP_LIVE_STATUS);
	ULONG second = vcap_read32(ZZ_VCAP_LIVE_STATUS);

	if (first != second) return FALSE;
	*status = first;
	return TRUE;
}

static BOOL vcap_capture_snapshot(struct zz_vcap_snapshot *snapshot)
{
	int tries;

	for (tries = 0; tries < 8; tries++) {
		ULONG before = vcap_read32(ZZ_VCAP_LIVE_STATUS);
		ULONG raw = vcap_read32(ZZ_VCAP_LIVE_APPLIED_RAW);
		ULONG effective = vcap_read32(ZZ_VCAP_LIVE_EFFECTIVE_CROP);
		ULONG after = vcap_read32(ZZ_VCAP_LIVE_STATUS);

		if (zz_vcap_snapshot_status_valid(before, after)) {
			snapshot->status = after;
			snapshot->raw = raw;
			zz_vcap_effective_unpack(effective, &snapshot->effective_h,
				&snapshot->effective_v);
			return TRUE;
		}
	}
	return FALSE;
}

static BOOL vcap_begin_apply(ULONG raw, UBYTE *expected_sequence)
{
	ULONG status_raw;
	struct zz_vcap_status status;

	if (!vcap_read_stable_status(&status_raw)) return FALSE;
	zz_vcap_status_unpack(status_raw, &status);
	if (!status.applied_valid || status.busy ||
		status.request_sequence != status.applied_sequence)
		return FALSE;

	*expected_sequence = zz_vcap_next_sequence(status.request_sequence);
	zz_set_reg(ZZ_VCAP_LIVE_STAGED_RAW_HI, (UWORD)(raw >> 16));
	zz_set_reg(ZZ_VCAP_LIVE_STAGED_RAW_LO, (UWORD)raw);
	zz_set_reg(ZZ_VCAP_LIVE_COMMIT, ZZ_VCAP_LIVE_COMMIT_TOKEN);
	return TRUE;
}

static int vcap_poll_apply(UBYTE expected_sequence, ULONG expected_raw,
	UWORD ticks, struct zz_vcap_snapshot *applied)
{
	UWORD elapsed;

	for (elapsed = 0; elapsed < ticks; elapsed++) {
		ULONG status;

		if (vcap_read_stable_status(&status)) {
			if (zz_vcap_request_complete(status, expected_sequence)) {
				int request_result;

				if (!vcap_capture_snapshot(applied))
					return VCAP_APPLY_ERROR;
				request_result = zz_vcap_request_result(status,
					expected_sequence, applied->raw, expected_raw);
				if (request_result == ZZ_VCAP_REQUEST_CONFLICT)
					return VCAP_APPLY_CONFLICT;
				return VCAP_APPLY_OK;
			}
		}
		Delay(1);
	}
	return VCAP_APPLY_TIMEOUT;
}

static int vcap_apply_raw(ULONG raw, struct zz_vcap_snapshot *applied,
	UBYTE *pending_sequence)
{
	UBYTE expected;

	if (!vcap_begin_apply(raw, &expected)) return VCAP_APPLY_ERROR;
	if (pending_sequence) *pending_sequence = expected;
	return vcap_poll_apply(expected, raw, VCAP_APPLY_TIMEOUT_TICKS, applied);
}

static void settings_live_init(struct settings_live_session *session,
	UWORD firmware_revision, UWORD firmware_capabilities)
{
	ULONG capability;

	memset(session, 0, sizeof(*session));
	capability = vcap_read32(ZZ_VCAP_LIVE_CAPABILITY);
	session->supported = zz_vcap_live_supported(firmware_revision,
		firmware_capabilities, capability);
	zz_vcap_anchors_init(&session->anchors);
}

static BOOL settings_live_refresh(struct settings_live_session *session)
{
	struct zz_vcap_snapshot snapshot;

	if (!session->supported || !vcap_capture_snapshot(&snapshot))
		return FALSE;
	session->current = snapshot;
	if (!session->anchors.valid[ZZ_VCAP_ANCHOR_SETTINGS])
		zz_vcap_anchor_store(&session->anchors, ZZ_VCAP_ANCHOR_SETTINGS,
			&snapshot);
	return TRUE;
}

static BOOL settings_live_restore(struct settings_live_session *session,
	UWORD owner)
{
	struct zz_vcap_snapshot anchor;
	struct zz_vcap_snapshot applied;
	int result;

	if (!session->supported ||
		!zz_vcap_anchor_load(&session->anchors, owner, &anchor))
		return TRUE;
	if (settings_live_refresh(session) && session->current.raw == anchor.raw)
		return TRUE;
	result = vcap_apply_raw(anchor.raw, &applied, NULL);
	if (result != VCAP_APPLY_OK) return FALSE;
	session->current = applied;
	return TRUE;
}

static void settings_path_for(struct zz_vcap_path *path, UWORD profile,
	UWORD sample)
{
	UWORD pal_mode, full_width, vsync;

	zzcfg_profile_to_legacy(profile, &pal_mode,
		&full_width, &vsync);
	path->sample = sample;
	path->full_width = full_width;
}

static BOOL settings_current_path_matches(
	const struct settings_live_session *session, UWORD profile, UWORD sample)
{
	struct zz_vcap_control control;
	struct zz_vcap_path staged;
	struct zz_vcap_path applied;

	if (!zz_vcap_control_unpack(session->current.raw, &control))
		return FALSE;
	settings_path_for(&staged, profile, sample);
	applied.sample = control.sample;
	applied.full_width = control.full_width;
	return zz_vcap_path_equal(&staged, &applied);
}

static BOOL settings_profile_matches(struct settings_live_session *session,
	UWORD profile, UWORD sample)
{
	return settings_live_refresh(session) &&
		settings_current_path_matches(session, profile, sample);
}

static BOOL settings_path_matches(struct settings_live_session *session,
	UWORD sample)
{
	return settings_profile_matches(session,
		settings_vals.videocap_profile, sample);
}

static BOOL settings_custom_framing(void)
{
	return settings_vals.videocap_crop_h_present ||
		settings_vals.videocap_crop_v_present;
}

static BOOL settings_custom_save_allowed(struct settings_live_session *session)
{
	if (!session->supported || !settings_custom_framing()) return TRUE;
	return settings_path_matches(session, settings_vals.videocap_sample);
}

enum vcap_pending_action {
	VCAP_PENDING_NONE = 0,
	VCAP_PENDING_ADJUST,
	VCAP_PENDING_ACCEPT,
	VCAP_PENDING_RESTORE
};

struct vcap_calibration {
	struct Screen *screen;
	struct Window *window;
	struct zz_vcap_snapshot entry;
	struct zz_vcap_snapshot current;
	struct zz_vcap_working working;
	struct zz_vcap_working pending_working;
	ULONG pending_raw;
	UBYTE pending_sequence;
	UWORD pending_action;
	BOOL cancel_requested;
	BOOL accepted;
	BOOL done;
	char message[96];
};

static void vcap_draw_line(struct RastPort *rp, WORD x1, WORD y1,
	WORD x2, WORD y2, UWORD pen)
{
	SetAPen(rp, pen);
	Move(rp, x1, y1);
	Draw(rp, x2, y2);
}

static void vcap_draw_calibration(struct vcap_calibration *calibration)
{
	struct RastPort *rp = calibration->window->RPort;
	WORD width = calibration->screen->Width;
	WORD height = calibration->screen->Height;
	WORD safe_x = width / 10;
	WORD safe_y = height / 10;
	char line[96];
	const char *legend =
		"Arrows adjust  Shift+Arrows coarse  Enter accept  Esc cancel";

	SetDrMd(rp, JAM1);
	SetRast(rp, 0);
	/* Outer edge, a conservative safe-area box, and an unobscured centre
	 * cross make both overscan loss and framing asymmetry obvious. */
	vcap_draw_line(rp, 0, 0, width - 1, 0, 2);
	vcap_draw_line(rp, width - 1, 0, width - 1, height - 1, 2);
	vcap_draw_line(rp, width - 1, height - 1, 0, height - 1, 2);
	vcap_draw_line(rp, 0, height - 1, 0, 0, 2);
	vcap_draw_line(rp, safe_x, safe_y, width - safe_x - 1, safe_y, 3);
	vcap_draw_line(rp, width - safe_x - 1, safe_y,
		width - safe_x - 1, height - safe_y - 1, 3);
	vcap_draw_line(rp, width - safe_x - 1, height - safe_y - 1,
		safe_x, height - safe_y - 1, 3);
	vcap_draw_line(rp, safe_x, height - safe_y - 1, safe_x, safe_y, 3);
	vcap_draw_line(rp, width / 2 - 24, height / 2,
		width / 2 + 24, height / 2, 1);
	vcap_draw_line(rp, width / 2, height / 2 - 24,
		width / 2, height / 2 + 24, 1);

	SetAPen(rp, 1);
	snprintf(line, sizeof(line), "Live Custom H:%u  V:%u",
		(unsigned)calibration->working.crop_h,
		(unsigned)calibration->working.crop_v);
	Move(rp, 8, 12);
	Text(rp, (CONST_STRPTR)line, strlen(line));
	Move(rp, 8, 24);
	Text(rp, (CONST_STRPTR)calibration->message,
		strlen(calibration->message));
	Move(rp, 8, height - 8);
	Text(rp, (CONST_STRPTR)legend, strlen(legend));
}

static int vcap_calibration_start_apply(struct vcap_calibration *calibration,
	const struct zz_vcap_working *candidate, UWORD action)
{
	struct zz_vcap_snapshot applied;
	ULONG raw = action == VCAP_PENDING_RESTORE ? calibration->entry.raw :
		zz_vcap_control_pack(&candidate->control);
	int result;

	if (raw == calibration->current.raw) {
		if (action == VCAP_PENDING_ACCEPT) {
			calibration->working = *candidate;
			calibration->accepted = TRUE;
			calibration->done = TRUE;
		} else if (action == VCAP_PENDING_RESTORE) {
			calibration->done = TRUE;
		}
		return VCAP_APPLY_OK;
	}

	result = vcap_apply_raw(raw, &applied, &calibration->pending_sequence);
	if (result == VCAP_APPLY_OK) {
		calibration->current = applied;
		if (action == VCAP_PENDING_RESTORE) {
			calibration->done = TRUE;
		} else {
			calibration->working = *candidate;
			if (action == VCAP_PENDING_ACCEPT) {
				calibration->accepted = TRUE;
				calibration->done = TRUE;
			}
		}
		return result;
	}
	if (result == VCAP_APPLY_TIMEOUT) {
		calibration->pending_raw = raw;
		calibration->pending_action = action;
		if (candidate) calibration->pending_working = *candidate;
		snprintf(calibration->message, sizeof(calibration->message),
			"No frame acknowledgement; Esc restores when frames return");
	} else if (result == VCAP_APPLY_CONFLICT) {
		calibration->current = applied;
		snprintf(calibration->message, sizeof(calibration->message),
			"Another writer won the request; retry or Esc to restore");
	} else {
		snprintf(calibration->message, sizeof(calibration->message),
			"Request not accepted; retry or Esc to restore");
	}
	return result;
}

static void vcap_calibration_reconcile(struct vcap_calibration *calibration)
{
	struct zz_vcap_snapshot applied;
	struct zz_vcap_status status;
	ULONG status_raw;
	int result;

	if (calibration->pending_action == VCAP_PENDING_NONE) return;
	result = vcap_poll_apply(calibration->pending_sequence,
		calibration->pending_raw, 1, &applied);
	if (result == VCAP_APPLY_CONFLICT) {
		UWORD conflict_action = calibration->pending_action;

		calibration->current = applied;
		calibration->pending_action = VCAP_PENDING_NONE;
		if (calibration->cancel_requested ||
			conflict_action == VCAP_PENDING_RESTORE)
			vcap_calibration_start_apply(calibration, NULL,
				VCAP_PENDING_RESTORE);
		else
			snprintf(calibration->message, sizeof(calibration->message),
				"Another writer won the request; press the key again");
		return;
	}
	if (result != VCAP_APPLY_OK) {
		/* A valid live request can lose an idle-arbiter race to a firmware
		 * or legacy operation-16 event. Rejected means that payload never
		 * became pending, so the last acknowledged state is still exact. */
		if (vcap_read_stable_status(&status_raw)) {
			zz_vcap_status_unpack(status_raw, &status);
			if (status.rejected && !status.busy &&
				status.request_sequence == status.applied_sequence &&
				status.request_sequence != calibration->pending_sequence) {
				UWORD rejected_action = calibration->pending_action;

				calibration->pending_action = VCAP_PENDING_NONE;
				if (calibration->cancel_requested ||
					rejected_action == VCAP_PENDING_RESTORE)
					vcap_calibration_start_apply(calibration, NULL,
						VCAP_PENDING_RESTORE);
				else
					snprintf(calibration->message,
						sizeof(calibration->message),
						"Request lost an arbiter race; press the key again");
			}
		}
		return;
	}

	calibration->current = applied;
	if (calibration->pending_action == VCAP_PENDING_RESTORE) {
		calibration->pending_action = VCAP_PENDING_NONE;
		calibration->done = TRUE;
		return;
	}
	calibration->working = calibration->pending_working;
	result = calibration->pending_action;
	calibration->pending_action = VCAP_PENDING_NONE;
	if (calibration->cancel_requested) {
		vcap_calibration_start_apply(calibration, NULL,
			VCAP_PENDING_RESTORE);
	} else if (result == VCAP_PENDING_ACCEPT) {
		calibration->accepted = TRUE;
		calibration->done = TRUE;
	} else {
		snprintf(calibration->message, sizeof(calibration->message),
			"Applied at capture-frame boundary");
	}
}

static void vcap_calibration_cancel(struct vcap_calibration *calibration)
{
	calibration->cancel_requested = TRUE;
	if (calibration->pending_action != VCAP_PENDING_NONE) {
		snprintf(calibration->message, sizeof(calibration->message),
			"Cancel pending; waiting to restore the entry state");
		return;
	}
	vcap_calibration_start_apply(calibration, NULL, VCAP_PENDING_RESTORE);
}

static void vcap_calibration_key(struct vcap_calibration *calibration,
	UWORD code, UWORD qualifier)
{
	struct zz_vcap_working candidate;
	UWORD move;

	if (code & IECODE_UP_PREFIX) return;
	if (calibration->done) {
		if (calibration->accepted && code == VCAP_RAWKEY_ESCAPE) {
			calibration->done = FALSE;
			calibration->accepted = FALSE;
			vcap_calibration_cancel(calibration);
		}
		return;
	}
	if (code == VCAP_RAWKEY_ESCAPE) {
		vcap_calibration_cancel(calibration);
		return;
	}
	if (calibration->pending_action != VCAP_PENDING_NONE ||
		calibration->cancel_requested)
		return;

	if (code == VCAP_RAWKEY_RETURN || code == VCAP_RAWKEY_KEYPAD_ENTER) {
		candidate = calibration->working;
		zz_vcap_accept(&candidate);
		vcap_calibration_start_apply(calibration, &candidate,
			VCAP_PENDING_ACCEPT);
		return;
	}
	switch (code) {
	case VCAP_RAWKEY_LEFT:  move = ZZ_VCAP_MOVE_LEFT; break;
	case VCAP_RAWKEY_RIGHT: move = ZZ_VCAP_MOVE_RIGHT; break;
	case VCAP_RAWKEY_UP:    move = ZZ_VCAP_MOVE_UP; break;
	case VCAP_RAWKEY_DOWN:  move = ZZ_VCAP_MOVE_DOWN; break;
	default: return;
	}

	candidate = calibration->working;
	if (!zz_vcap_adjust(&candidate, move,
		(qualifier & (IEQUALIFIER_LSHIFT | IEQUALIFIER_RSHIFT)) != 0)) {
		snprintf(calibration->message, sizeof(calibration->message),
			"At 0..4095 limit; no hardware request sent");
		return;
	}
	vcap_calibration_start_apply(calibration, &candidate,
		VCAP_PENDING_ADJUST);
}

/* PAL.monitor and NTSC.monitor are optional virtual monitors. If native
 * frames are active, preserve their detected standard. If RTG Workbench has
 * left the native input idle, bootstrap from default.monitor instead of the
 * protocol's necessarily stale last-detected standard. HIRES_KEY cannot
 * select an RTG monitor. */
static ULONG vcap_calibration_display_id(UWORD detected_standard,
	UWORD *display_standard, ULONG *mode_error)
{
	struct DisplayInfo display_info;
	ULONG display_id;
	ULONG error;
	ULONG default_error = ModeNotAvailable(HIRES_KEY);
	UWORD default_standard = ZZ_VCAP_STANDARD_UNKNOWN;

	memset(&display_info, 0, sizeof(display_info));
	if (default_error == 0 &&
		GetDisplayInfoData(NULL, (UBYTE *)&display_info,
			sizeof(display_info), DTAG_DISP, HIRES_KEY) != 0 &&
		(display_info.PropertyFlags & DIPF_IS_FOREIGN) == 0) {
		default_standard =
			(display_info.PropertyFlags & DIPF_IS_PAL) ?
			ZZ_VCAP_STANDARD_PAL : ZZ_VCAP_STANDARD_NTSC;
	}

	if (detected_standard == ZZ_VCAP_STANDARD_UNKNOWN) {
		if (default_standard == ZZ_VCAP_STANDARD_UNKNOWN) {
			*mode_error = default_error;
			return (ULONG)INVALID_ID;
		}
		*display_standard = default_standard;
		*mode_error = 0;
		return HIRES_KEY;
	}

	display_id = detected_standard == ZZ_VCAP_STANDARD_NTSC ?
		(NTSC_MONITOR_ID | HIRES_KEY) : (PAL_MONITOR_ID | HIRES_KEY);
	error = ModeNotAvailable(display_id);
	if (error == 0) {
		*display_standard = detected_standard;
		*mode_error = 0;
		return display_id;
	}
	if (default_standard == detected_standard) {
		*display_standard = default_standard;
		*mode_error = 0;
		return HIRES_KEY;
	}

	*mode_error = error;
	return (ULONG)INVALID_ID;
}

static BOOL vcap_screen_is_foreign(struct Screen *screen)
{
	struct DisplayInfo display_info;
	ULONG display_id;

	if (!screen) return FALSE;
	display_id = GetVPModeID(&screen->ViewPort);
	if (display_id == (ULONG)INVALID_ID) return FALSE;
	memset(&display_info, 0, sizeof(display_info));
	return GetDisplayInfoData(NULL, (UBYTE *)&display_info,
		sizeof(display_info), DTAG_DISP, display_id) != 0 &&
		(display_info.PropertyFlags & DIPF_IS_FOREIGN) != 0;
}

static int vcap_calibration_run(struct Screen *return_screen,
	struct Window *return_window, struct settings_live_session *session,
	struct zz_vcap_control *accepted_control, char *failure,
	UWORD failure_size)
{
	static struct ColorSpec colors[] = {
		{ 0, 0, 0, 0 }, { 1, 15, 15, 15 },
		{ 2, 15, 3, 3 }, { 3, 3, 8, 15 }, { -1, 0, 0, 0 }
	};
	struct vcap_calibration calibration;
	ULONG display_id;
	ULONG mode_error;
	ULONG screen_error = 0;
	struct IntuiMessage *message;
	char last_message[96];
	UWORD last_h;
	UWORD last_v;
	UWORD detected_standard;
	UWORD display_standard;
	UWORD lines;
	BOOL caller_is_foreign;
	BOOL want_ntsc;

	memset(&calibration, 0, sizeof(calibration));
	if (!settings_live_refresh(session)) {
		snprintf(failure, failure_size,
			"Live calibration state became unavailable");
		return -1;
	}
	calibration.entry = session->current;
	calibration.current = session->current;
	zz_vcap_anchor_store(&session->anchors, ZZ_VCAP_ANCHOR_CALIBRATION,
		&calibration.entry);
	if (!zz_vcap_control_unpack(calibration.entry.raw,
		&calibration.working.control)) {
		snprintf(failure, failure_size,
			"Live calibration control word is invalid");
		return -1;
	}
	calibration.working.crop_h = calibration.entry.effective_h;
	calibration.working.crop_v = calibration.entry.effective_v;
	snprintf(calibration.message, sizeof(calibration.message),
		"Adjust until the boxes are centred");
	lines = zz_get_reg16(REG_ZZ_VIDEOCAP_STATS) & VCAP_LINES_MASK;
	caller_is_foreign = vcap_screen_is_foreign(return_screen);
	detected_standard = zz_vcap_calibration_standard(
		calibration.entry.status, lines, caller_is_foreign);
	display_id = vcap_calibration_display_id(detected_standard,
		&display_standard, &mode_error);
	if (display_id == (ULONG)INVALID_ID) {
		if (detected_standard == ZZ_VCAP_STANDARD_UNKNOWN)
			snprintf(failure, failure_size,
				"Native default Hires unavailable (mode error %lu)",
				(unsigned long)mode_error);
		else
			snprintf(failure, failure_size,
				"Native %s Hires unavailable (error %lu, %s, lines %u)",
				detected_standard == ZZ_VCAP_STANDARD_NTSC ?
				"NTSC" : "PAL", (unsigned long)mode_error,
				caller_is_foreign ? "RTG" : "native", (unsigned)lines);
		return -2;
	}
	want_ntsc = display_standard == ZZ_VCAP_STANDARD_NTSC;

	calibration.screen = OpenScreenTags(NULL,
		SA_Type, CUSTOMSCREEN, SA_DisplayID, display_id,
		SA_Depth, 2, SA_Overscan, OSCAN_TEXT, SA_Colors, colors,
		SA_SysFont, 0, SA_Quiet, TRUE, SA_ShowTitle, FALSE,
		SA_Behind, TRUE, SA_AutoScroll, FALSE,
		SA_ErrorCode, &screen_error, TAG_END);
	if (!calibration.screen) {
		snprintf(failure, failure_size,
			"Native %s screen open failed (Intuition error %lu)",
			want_ntsc ? "NTSC" : "PAL", (unsigned long)screen_error);
		return -3;
	}
	calibration.window = OpenWindowTags(NULL,
		WA_CustomScreen, calibration.screen,
		WA_Left, 0, WA_Top, 0,
		WA_Width, calibration.screen->Width,
		WA_Height, calibration.screen->Height,
		WA_Borderless, TRUE, WA_Backdrop, TRUE, WA_RMBTrap, TRUE,
		WA_NoCareRefresh, TRUE, WA_Activate, FALSE,
		WA_RptQueue, 16, WA_IDCMP, IDCMP_RAWKEY, TAG_END);
	if (!calibration.window) {
		CloseScreen(calibration.screen);
		snprintf(failure, failure_size,
			"Native calibration window could not be opened");
		return -4;
	}

	vcap_draw_calibration(&calibration);
	snprintf(last_message, sizeof(last_message), "%s", calibration.message);
	last_h = calibration.working.crop_h;
	last_v = calibration.working.crop_v;
	ScreenToFront(calibration.screen);
	ActivateWindow(calibration.window);
	while (!calibration.done) {
		ULONG signal = 1UL << calibration.window->UserPort->mp_SigBit;

		if (calibration.pending_action == VCAP_PENDING_NONE)
			Wait(signal);
		else
			Delay(1);
		while ((message = (struct IntuiMessage *)
			GetMsg(calibration.window->UserPort))) {
			ULONG message_class = message->Class;
			UWORD code = message->Code;
			UWORD qualifier = message->Qualifier;

			ReplyMsg((struct Message *)message);
			if (message_class == IDCMP_RAWKEY)
				vcap_calibration_key(&calibration, code, qualifier);
		}
		vcap_calibration_reconcile(&calibration);
		if (last_h != calibration.working.crop_h ||
			last_v != calibration.working.crop_v ||
			strcmp(last_message, calibration.message) != 0) {
			vcap_draw_calibration(&calibration);
			last_h = calibration.working.crop_h;
			last_v = calibration.working.crop_v;
			snprintf(last_message, sizeof(last_message), "%s",
				calibration.message);
		}
	}

	while ((message = (struct IntuiMessage *)
		GetMsg(calibration.window->UserPort)))
		ReplyMsg((struct Message *)message);
	CloseWindow(calibration.window);
	CloseScreen(calibration.screen);
	ScreenToFront(return_screen);
	ActivateWindow(return_window);

	session->current = calibration.current;
	if (!calibration.accepted) return 0;
	*accepted_control = calibration.working.control;
	zz_vcap_anchor_store(&session->anchors, ZZ_VCAP_ANCHOR_PREVIEW,
		&calibration.current);
	session->preview_control = calibration.working.control;
	session->preview_valid = TRUE;
	return 1;
}

static CONST_STRPTR settings_label_samples[] = {
	(CONST_STRPTR)LABEL_VCAPMODE,
	(CONST_STRPTR)LABEL_SCANLINES,
	(CONST_STRPTR)LABEL_PARITY,
	(CONST_STRPTR)LABEL_INT2,
	(CONST_STRPTR)LABEL_MAC,
	(CONST_STRPTR)LABEL_HDF,
	(CONST_STRPTR)LABEL_OFFSCREEN,
	(CONST_STRPTR)LABEL_OVERLAY,
	NULL
};

/* Sized from the widest cycle/string content only; the status line
 * spans the full row instead, so long messages don't inflate the
 * control column (and with it the whole window). */
static CONST_STRPTR settings_value_samples[] = {
	(CONST_STRPTR)"1280x1024 Match PAL/NTSC (Full detail)",
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

static int settings_parse_u12(const char *s, UWORD *out)
{
	ULONG value = 0;

	if (!s || !*s) return 0;
	while (*s) {
		if (*s < '0' || *s > '9') return 0;
		value = value * 10 + (ULONG)(*s - '0');
		if (value > 4095) return 0;
		s++;
	}
	*out = (UWORD)value;
	return 1;
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
	UWORD pal_mode, full, vsync;
	int any = 0;
	int native_override = 0;

	zzcfg_profile_to_legacy(sv->videocap_profile, &pal_mode, &full, &vsync);
	if (settings_env_exists("ENV:ZZ9000-VCAP-800x600")) {
		pal_mode = 0;
		full = 0;
		any = 1;
		native_override = 1;
	}
	if (settings_env_exists("ENV:ZZ9000-NS-VSYNC")) {
		vsync = 1;
		any = 1;
		native_override = 1;
	} else if (settings_env_exists("ENV:ZZ9000-NS-VSYNC-NTSC")) {
		vsync = 2;
		any = 1;
		native_override = 1;
	}
	/* MAC/INT2 overrides are unrelated and must not collapse a centered
	 * profile through the lossy legacy tuple. Native-video ENV overrides
	 * intentionally select a legacy profile. */
	if (native_override)
		sv->videocap_profile = zzcfg_profile_from_legacy(pal_mode, full, vsync);
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
static void settings_populate(struct Window *win, UWORD fw_capabilities)
{
	ULONG board = (ULONG)zz_regs;
	struct zzcfg_values *sv = &settings_vals;
	UWORD st, rawlen = 0;
	BOOL centered_fallback = FALSE;

	/* Editor defaults; keys present in the raw file override them.
	 * Scanlines default to the live FPGA state - the config applied
	 * it at cold boot and this tool/ZZScanlines may have changed it
	 * since. */
	memset(sv, 0, sizeof(*sv));
	sv->videocap_profile = ZZCFG_VCAP_FULL_60;
	sv->use_videocap_profile_key =
		(fw_capabilities & ZZ_FW_CAP_VIDEOCAP_PROFILE) != 0;
	sv->firmware_capabilities = fw_capabilities;
	sv->videocap_crop_h = ZZCFG_VIDEOCAP_CROP_H_COMPAT;
	sv->videocap_crop_v = ZZCFG_VIDEOCAP_CROP_V_COMPAT;
	sv->scanline_mode = zz_get_scanline_mode();
	sv->scanline_parity = zz_get_scanline_parity();
	/* ZZ9000.card enables both of these when the key is absent, so the
	 * editor must default to on as well. Save writes every supported key,
	 * so a zeroed default here would disable off-screen bitmaps and PIP
	 * for anyone who merely opened the window and pressed Save. */
	sv->offscreen_bitmaps = 1;
	sv->video_overlay = 1;

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
			if (!zzcfg_profile_supported(sv->videocap_profile,
					fw_capabilities)) {
				sv->videocap_profile = zzcfg_profile_sanitize(
					sv->videocap_profile, fw_capabilities);
				centered_fallback = TRUE;
			}
			env_active = settings_apply_env_overrides(sv);
			if (centered_fallback) {
				snprintf(settings_status_buf, sizeof(settings_status_buf),
					"Centered 1080p needs matching firmware/bitstream; Full 60 staged");
			} else if (env_active) {
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
		GTCY_Active, sv->videocap_profile, TAG_END);
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
	GT_SetGadgetAttrs(sgads[SGAD_OFFSCREEN], win, NULL,
		GTCY_Active, sv->offscreen_bitmaps ? 1 : 0, TAG_END);
	GT_SetGadgetAttrs(sgads[SGAD_OVERLAY], win, NULL,
		GTCY_Active, sv->video_overlay ? 1 : 0, TAG_END);
	settings_set_status(win, settings_status_buf);
}

static BOOL settings_save(struct Window *win)
{
	struct zzcfg_values *sv = &settings_vals;
	struct StringInfo *si;
	UWORD st;

	if (!settings_have_cfg) {
		settings_set_status(win, "Config needs firmware 2.3+");
		return FALSE;
	}

	si = (struct StringInfo *)sgads[SGAD_MAC]->SpecialInfo;
	snprintf(sv->mac, sizeof(sv->mac), "%s", (const char *)si->Buffer);
	si = (struct StringInfo *)sgads[SGAD_HDF]->SpecialInfo;
	snprintf(sv->hdf, sizeof(sv->hdf), "%s", (const char *)si->Buffer);

	if (sv->mac[0] && !settings_parse_mac(sv->mac)) {
		settings_set_status(win, "Bad MAC - use aa:bb:cc:dd:ee:ff");
		return FALSE;
	}
	/* Firmware hdf rules, not the FWUP name rules: they differ (no
	 * leading '.', 63-char cap), and the firmware silently ignores a
	 * name it rejects at the next cold boot. */
	if (sv->hdf[0] && !zzcfg_hdf_name_valid(sv->hdf)) {
		settings_set_status(win, "Bad HDF name (flat root file)");
		return FALSE;
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
		return TRUE;
	} else {
		snprintf(settings_status_buf, sizeof(settings_status_buf),
			"Save failed: %s", fwup_strerror(st));
		settings_set_status(win, settings_status_buf);
	}
	return FALSE;
}

static struct Gadget *settings_create_gadgets(struct Gadget **glistptr,
	void *vi, const struct ZZTopLayout *mainlayout, UWORD fw_capabilities,
	WORD *out_w, WORD *out_h)
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
		GTCY_Labels,
		(fw_capabilities & ZZ_FW_CAP_VIDEOCAP_CENTERED_1080P) ?
			vcapmode_labels : vcapmode_legacy_labels,
		GTCY_Active, 0, TAG_END);
	y += l.row_step;

	ng.ng_TopEdge    = y;
	ng.ng_GadgetID   = SGAD_VCAP_ADVANCED;
	ng.ng_GadgetText = (STRPTR)LABEL_VCAP_ADVANCED;
	ng.ng_Flags      = PLACETEXT_IN;
	sgads[SGAD_VCAP_ADVANCED] = gad = CreateGadget(BUTTON_KIND, gad, &ng,
		TAG_END);
	ng.ng_Flags      = PLACETEXT_LEFT;
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
	y += l.row_step;

	ng.ng_TopEdge    = y;
	ng.ng_GadgetID   = SGAD_OFFSCREEN;
	ng.ng_GadgetText = (STRPTR)LABEL_OFFSCREEN;
	sgads[SGAD_OFFSCREEN] = gad = CreateGadget(CYCLE_KIND, gad, &ng,
		GTCY_Labels, enable_labels, GTCY_Active, 1, TAG_END);
	y += l.row_step;

	ng.ng_TopEdge    = y;
	ng.ng_GadgetID   = SGAD_OVERLAY;
	ng.ng_GadgetText = (STRPTR)LABEL_OVERLAY;
	sgads[SGAD_OVERLAY] = gad = CreateGadget(CYCLE_KIND, gad, &ng,
		GTCY_Labels, enable_labels, GTCY_Active, 1, TAG_END);
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
	/* Same WA_InnerHeight convention as the main window: gadget positions
	 * include the title bar, the inner height does not. */
	*out_h = y + l.gadget_height + (l.margin_y / 2) - l.topborder;

	for (i = 0; i < SGAD_COUNT; i++) {
		if (!sgads[i]) return NULL;
	}

	return gad;
}

static BOOL settings_video_advanced_candidate(struct Window *win,
	struct Gadget **agads, UWORD sample, UWORD framing,
	BOOL framing_changed, const struct zzcfg_values *base,
	char *status, UWORD status_size, struct zzcfg_values *candidate,
	BOOL *changed)
{
	struct StringInfo *hsi =
		(struct StringInfo *)agads[AGAD_VCAP_CROP_H]->SpecialInfo;
	struct StringInfo *vsi =
		(struct StringInfo *)agads[AGAD_VCAP_CROP_V]->SpecialInfo;
	UWORD crop_h, crop_v;
	UWORD crop_h_present, crop_v_present;

	*candidate = *base;
	if (framing == 0) {
		*changed = sample != base->videocap_sample ||
			base->videocap_crop_h_present || base->videocap_crop_v_present;
		candidate->videocap_sample = sample;
		candidate->videocap_crop_h_present = 0;
		candidate->videocap_crop_v_present = 0;
		return TRUE;
	}

	if (!settings_parse_u12((const char *)hsi->Buffer, &crop_h)) {
		snprintf(status, status_size, "Bad Crop H - use 0..4095");
		GT_SetGadgetAttrs(agads[AGAD_STATUS], win, NULL,
			GTTX_Text, status, TAG_END);
		return FALSE;
	}
	if (!settings_parse_u12((const char *)vsi->Buffer, &crop_v)) {
		snprintf(status, status_size, "Bad Crop V - use 0..4095");
		GT_SetGadgetAttrs(agads[AGAD_STATUS], win, NULL,
			GTTX_Text, status, TAG_END);
		return FALSE;
	}

	/* A one-axis hand-edited file opens as Custom. Merely pressing Done must
	 * not materialize its missing axis; changing that field does. A deliberate
	 * framing cycle (including Automatic -> Custom) makes both axes explicit. */
	if (framing_changed) {
		crop_h_present = 1;
		crop_v_present = 1;
	} else {
		crop_h_present = base->videocap_crop_h_present ||
			crop_h != base->videocap_crop_h;
		crop_v_present = base->videocap_crop_v_present ||
			crop_v != base->videocap_crop_v;
	}

	*changed = sample != base->videocap_sample ||
		crop_h != base->videocap_crop_h || crop_v != base->videocap_crop_v ||
		crop_h_present != base->videocap_crop_h_present ||
		crop_v_present != base->videocap_crop_v_present;
	candidate->videocap_sample = sample;
	candidate->videocap_crop_h = crop_h;
	candidate->videocap_crop_v = crop_v;
	candidate->videocap_crop_h_present = crop_h_present;
	candidate->videocap_crop_v_present = crop_v_present;
	return TRUE;
}

static void settings_control_from_values(const struct zzcfg_values *values,
	struct zz_vcap_control *control)
{
	struct zz_vcap_path path;

	settings_path_for(&path, values->videocap_profile,
		values->videocap_sample);
	control->sample = path.sample;
	control->full_width = path.full_width;
	control->crop_h = values->videocap_crop_h;
	control->crop_v = values->videocap_crop_v;
	control->crop_h_present = values->videocap_crop_h_present;
	control->crop_v_present = values->videocap_crop_v_present;
}

enum vcap_calibration_availability {
	VCAP_CALIBRATION_UNKNOWN = -1,
	VCAP_CALIBRATION_UNSUPPORTED = 0,
	VCAP_CALIBRATION_WAITING,
	VCAP_CALIBRATION_PATH_MISMATCH,
	VCAP_CALIBRATION_READY
};

static enum vcap_calibration_availability
settings_video_calibration_availability(
	struct settings_live_session *session, UWORD sample, char *status,
	UWORD status_size)
{
	if (!session->supported) {
		snprintf(status, status_size,
			"Calibrate needs firmware 2.8 live control and protocol-1 bitstream");
		return VCAP_CALIBRATION_UNSUPPORTED;
	}
	if (!settings_live_refresh(session)) {
		snprintf(status, status_size,
			"Calibrate waiting for stable native video frames");
		return VCAP_CALIBRATION_WAITING;
	}
	if (!settings_current_path_matches(session,
		settings_vals.videocap_profile, sample)) {
		snprintf(status, status_size,
			"Staged capture path differs; restore it or Save Automatic + reboot");
		return VCAP_CALIBRATION_PATH_MISMATCH;
	}
	snprintf(status, status_size,
		"Calibrate is ready on the currently applied capture path");
	return VCAP_CALIBRATION_READY;
}

/* Sampling phase and capture origin are calibration/diagnostic controls, not
 * independent output-mode choices. Keeping them in a small secondary window
 * makes the normal Settings path describe outcomes instead of implementation
 * details. Values are committed to settings_vals only by Done; the main
 * window's Save button still writes the card. */
static BOOL settings_video_advanced_window(struct Screen *mysc, void *vi,
	const struct ZZTopLayout *mainlayout,
	struct settings_live_session *live_session)
{
	static CONST_STRPTR label_samples[] = {
		(CONST_STRPTR)LABEL_VCAP_SAMPLE,
		(CONST_STRPTR)LABEL_VCAP_FRAMING,
		(CONST_STRPTR)LABEL_VCAP_CROP,
		NULL
	};
	static CONST_STRPTR value_samples[] = {
		(CONST_STRPTR)"Automatic (recommended)",
		(CONST_STRPTR)"Average (recommended)",
		(CONST_STRPTR)"Even (diagnostic)",
		NULL
	};
	struct ZZTopLayout l = *mainlayout;
	struct NewGadget ng;
	struct Gadget *glist = NULL;
	struct Gadget *gad;
	struct Gadget *agads[AGAD_COUNT];
	struct Window *win;
	struct IntuiMessage *imsg;
	struct zzcfg_values entry_values = settings_vals;
	struct zzcfg_values candidate_values;
	struct zz_vcap_control candidate_control;
	struct zz_vcap_control accepted_control;
	UWORD sample = settings_vals.videocap_sample;
	UWORD framing = (settings_vals.videocap_crop_h_present ||
		settings_vals.videocap_crop_v_present) ? 1 : 0;
	ULONG imsg_class;
	UWORD imsg_code;
	WORD label_width, value_width, y, content_right;
	WORD crop_width, crop_gap, button_width, button_gap;
	WORD w, h;
	BOOL done = FALSE;
	BOOL changed = FALSE;
	BOOL framing_changed = FALSE;
	BOOL cancel = FALSE;
	char crop_h_buf[6];
	char crop_v_buf[6];
	char status[128] = "Checking live calibration support...";
	enum vcap_calibration_availability availability =
		VCAP_CALIBRATION_UNKNOWN;
	int i;

	zz_vcap_anchor_clear(&live_session->anchors, ZZ_VCAP_ANCHOR_ADVANCED);
	zz_vcap_anchor_clear(&live_session->anchors, ZZ_VCAP_ANCHOR_PREVIEW);
	live_session->preview_valid = FALSE;
	if (settings_live_refresh(live_session))
		zz_vcap_anchor_store(&live_session->anchors, ZZ_VCAP_ANCHOR_ADVANCED,
			&live_session->current);

	label_width = zztop_max_text_width(
		zztop_screen ? &zztop_screen->RastPort : NULL, label_samples, 8);
	value_width = zztop_max_text_width(
		zztop_screen ? &zztop_screen->RastPort : NULL, value_samples, 8);
	l.gadget_left = l.margin_x + label_width + l.label_gap;
	l.gadget_width = zztop_max_word(184, value_width + 48);
	content_right = l.gadget_left + l.gadget_width;
	crop_gap = 8;
	crop_width = (l.gadget_width - crop_gap) / 2;
	button_gap = 16;
	button_width = 88;

	snprintf(crop_h_buf, sizeof(crop_h_buf), "%u",
		(unsigned)settings_vals.videocap_crop_h);
	snprintf(crop_v_buf, sizeof(crop_v_buf), "%u",
		(unsigned)settings_vals.videocap_crop_v);

	gad = CreateContext(&glist);
	for (i = 0; i < AGAD_COUNT; i++) agads[i] = NULL;
	y = l.topborder + l.margin_y;

	ng.ng_LeftEdge = l.gadget_left;
	ng.ng_TopEdge = y;
	ng.ng_Width = l.gadget_width;
	ng.ng_Height = l.gadget_height;
	ng.ng_TextAttr = l.text_attr;
	ng.ng_VisualInfo = vi;
	ng.ng_Flags = PLACETEXT_LEFT;
	ng.ng_GadgetID = AGAD_VCAP_SAMPLE;
	ng.ng_GadgetText = (STRPTR)LABEL_VCAP_SAMPLE;
	agads[AGAD_VCAP_SAMPLE] = gad = CreateGadget(CYCLE_KIND, gad, &ng,
		GTCY_Labels, vcapsample_labels, GTCY_Active, sample, TAG_END);
	y += l.row_step;

	ng.ng_TopEdge = y;
	ng.ng_GadgetID = AGAD_VCAP_FRAMING;
	ng.ng_GadgetText = (STRPTR)LABEL_VCAP_FRAMING;
	agads[AGAD_VCAP_FRAMING] = gad = CreateGadget(CYCLE_KIND, gad, &ng,
		GTCY_Labels, vcapframing_labels, GTCY_Active, framing, TAG_END);
	y += l.row_step;

	ng.ng_TopEdge = y;
	ng.ng_Width = crop_width;
	ng.ng_GadgetID = AGAD_VCAP_CROP_H;
	ng.ng_GadgetText = (STRPTR)LABEL_VCAP_CROP;
	agads[AGAD_VCAP_CROP_H] = gad = CreateGadget(STRING_KIND, gad, &ng,
		GTST_MaxChars, 5, GTST_String, crop_h_buf,
		GA_Disabled, framing == 0, TAG_END);
	ng.ng_LeftEdge = l.gadget_left + crop_width + crop_gap;
	ng.ng_GadgetID = AGAD_VCAP_CROP_V;
	ng.ng_GadgetText = NULL;
	agads[AGAD_VCAP_CROP_V] = gad = CreateGadget(STRING_KIND, gad, &ng,
		GTST_MaxChars, 5, GTST_String, crop_v_buf,
		GA_Disabled, framing == 0, TAG_END);
	y += l.row_step;

	ng.ng_LeftEdge = l.gadget_left;
	ng.ng_TopEdge = y;
	ng.ng_Width = l.gadget_width;
	ng.ng_GadgetID = AGAD_BTN_CALIBRATE;
	ng.ng_GadgetText = (STRPTR)"Calibrate...";
	ng.ng_Flags = PLACETEXT_IN;
	agads[AGAD_BTN_CALIBRATE] = gad = CreateGadget(BUTTON_KIND, gad, &ng,
		GA_Disabled, TRUE, TAG_END);
	y += l.row_step + l.section_gap;

	ng.ng_LeftEdge = l.margin_x;
	ng.ng_TopEdge = y;
	ng.ng_Width = content_right - l.margin_x;
	ng.ng_GadgetID = AGAD_STATUS;
	ng.ng_GadgetText = NULL;
	ng.ng_Flags = PLACETEXT_LEFT;
	agads[AGAD_STATUS] = gad = CreateGadget(TEXT_KIND, gad, &ng,
		GTTX_Text, status, GTTX_Border, TRUE, TAG_END);
	y += l.row_step + l.section_gap;

	ng.ng_LeftEdge = l.margin_x;
	ng.ng_TopEdge = y;
	ng.ng_Width = button_width;
	ng.ng_GadgetID = AGAD_BTN_DONE;
	ng.ng_GadgetText = (STRPTR)"Done";
	ng.ng_Flags = PLACETEXT_IN;
	agads[AGAD_BTN_DONE] = gad = CreateGadget(BUTTON_KIND, gad, &ng, TAG_END);
	ng.ng_LeftEdge = l.margin_x + button_width + button_gap;
	ng.ng_GadgetID = AGAD_BTN_CANCEL;
	ng.ng_GadgetText = (STRPTR)"Cancel";
	agads[AGAD_BTN_CANCEL] = gad = CreateGadget(BUTTON_KIND, gad, &ng, TAG_END);
	y += l.row_step;

	for (i = 0; i < AGAD_COUNT; i++) {
		if (!agads[i]) {
			if (glist) FreeGadgets(glist);
			errorMessage("Advanced Video: gadget creation failed");
			return FALSE;
		}
	}

	w = zztop_max_word(content_right + l.margin_x,
		l.margin_x + button_width + button_gap + button_width + l.margin_x);
	h = y + l.gadget_height + (l.margin_y / 2) - l.topborder;
	win = OpenWindowTags(NULL,
		WA_Title, "Advanced Native Video",
		WA_Gadgets, glist, WA_AutoAdjust, TRUE,
		WA_Width, w, WA_MinWidth, w,
		WA_InnerHeight, h, WA_MinHeight, h,
		WA_DragBar, TRUE, WA_DepthGadget, TRUE,
		WA_Activate, TRUE, WA_CloseGadget, TRUE,
		WA_SizeGadget, FALSE, WA_SimpleRefresh, TRUE,
		WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW |
			BUTTONIDCMP | CYCLEIDCMP | STRINGIDCMP,
		WA_PubScreen, mysc,
		TAG_END);
	if (!win) {
		FreeGadgets(glist);
		errorMessage("Advanced Video: OpenWindow() failed");
		return FALSE;
	}

	GT_RefreshWindow(win, NULL);
	while (!done) {
		enum vcap_calibration_availability now_available =
			settings_video_calibration_availability(
			live_session, sample, status, sizeof(status));

		if (now_available == VCAP_CALIBRATION_READY &&
			!live_session->anchors.valid[ZZ_VCAP_ANCHOR_ADVANCED])
			zz_vcap_anchor_store(&live_session->anchors,
				ZZ_VCAP_ANCHOR_ADVANCED, &live_session->current);
		if (now_available != availability) {
			availability = now_available;
			GT_SetGadgetAttrs(agads[AGAD_BTN_CALIBRATE], win, NULL,
				GA_Disabled, availability != VCAP_CALIBRATION_READY, TAG_END);
			GT_SetGadgetAttrs(agads[AGAD_STATUS], win, NULL,
				GTTX_Text, status, TAG_END);
		}
		Delay(2);
		while ((imsg = GT_GetIMsg(win->UserPort))) {
			gad = (struct Gadget *)imsg->IAddress;
			imsg_class = imsg->Class;
			imsg_code = imsg->Code;
			GT_ReplyIMsg(imsg);

			if (imsg_class == IDCMP_CLOSEWINDOW) {
				cancel = TRUE;
			} else if (imsg_class == IDCMP_REFRESHWINDOW) {
				GT_BeginRefresh(win);
				GT_EndRefresh(win, TRUE);
			} else if (imsg_class == IDCMP_GADGETUP && gad) {
				if (gad->GadgetID == AGAD_VCAP_SAMPLE) {
					sample = imsg_code;
					availability = VCAP_CALIBRATION_UNKNOWN;
				} else if (gad->GadgetID == AGAD_VCAP_FRAMING) {
					if (imsg_code != framing) {
						framing_changed = TRUE;
						framing = imsg_code ? 1 : 0;
						GT_SetGadgetAttrs(agads[AGAD_VCAP_CROP_H], win, NULL,
							GA_Disabled, framing == 0, TAG_END);
						GT_SetGadgetAttrs(agads[AGAD_VCAP_CROP_V], win, NULL,
							GA_Disabled, framing == 0, TAG_END);
					}
				} else if (gad->GadgetID == AGAD_BTN_CALIBRATE &&
					availability == VCAP_CALIBRATION_READY) {
					int result = vcap_calibration_run(mysc, win, live_session,
						&accepted_control, status, sizeof(status));

					if (result == 1) {
						sample = accepted_control.sample;
						framing = 1;
						framing_changed = TRUE;
						snprintf(crop_h_buf, sizeof(crop_h_buf), "%u",
							(unsigned)accepted_control.crop_h);
						snprintf(crop_v_buf, sizeof(crop_v_buf), "%u",
							(unsigned)accepted_control.crop_v);
						GT_SetGadgetAttrs(agads[AGAD_VCAP_SAMPLE], win, NULL,
							GTCY_Active, sample, TAG_END);
						GT_SetGadgetAttrs(agads[AGAD_VCAP_FRAMING], win, NULL,
							GTCY_Active, 1, TAG_END);
						GT_SetGadgetAttrs(agads[AGAD_VCAP_CROP_H], win, NULL,
							GTST_String, crop_h_buf, GA_Disabled, FALSE, TAG_END);
						GT_SetGadgetAttrs(agads[AGAD_VCAP_CROP_V], win, NULL,
							GTST_String, crop_v_buf, GA_Disabled, FALSE, TAG_END);
						snprintf(status, sizeof(status),
							"Live preview accepted; Done stages it, Cancel restores it");
						GT_SetGadgetAttrs(agads[AGAD_STATUS], win, NULL,
							GTTX_Text, status, TAG_END);
					} else if (result < 0) {
						GT_SetGadgetAttrs(agads[AGAD_STATUS], win, NULL,
							GTTX_Text, status, TAG_END);
					}
				} else if (gad->GadgetID == AGAD_BTN_CANCEL) {
					cancel = TRUE;
				} else if (gad->GadgetID == AGAD_BTN_DONE) {
					if (settings_video_advanced_candidate(win, agads, sample,
						framing, framing_changed, &entry_values, status,
						sizeof(status), &candidate_values, &changed)) {
						settings_control_from_values(&candidate_values,
							&candidate_control);
						if ((!live_session->preview_valid ||
							!zz_vcap_control_equal(&candidate_control,
								&live_session->preview_control)) &&
							!settings_live_restore(live_session,
								ZZ_VCAP_ANCHOR_ADVANCED)) {
							snprintf(status, sizeof(status),
								"Cannot restore live preview; keep window open and retry");
							GT_SetGadgetAttrs(agads[AGAD_STATUS], win, NULL,
								GTTX_Text, status, TAG_END);
						} else {
							settings_vals = candidate_values;
							live_session->preview_valid =
								live_session->preview_valid &&
								zz_vcap_control_equal(&candidate_control,
									&live_session->preview_control);
							done = TRUE;
						}
					}
				}
			}
		}
		if (cancel) {
			if (settings_live_restore(live_session,
				ZZ_VCAP_ANCHOR_ADVANCED)) {
				live_session->preview_valid = FALSE;
				changed = FALSE;
				done = TRUE;
			} else {
				cancel = FALSE;
				snprintf(status, sizeof(status),
					"Cancel waiting for acknowledged live restore; retry or cold boot");
				GT_SetGadgetAttrs(agads[AGAD_STATUS], win, NULL,
					GTTX_Text, status, TAG_END);
			}
		}
	}

	CloseWindow(win);
	FreeGadgets(glist);
	return changed;
}

static BOOL settings_update_save_gate(struct Window *win,
	struct settings_live_session *live_session, BOOL explain)
{
	BOOL allowed = settings_have_cfg &&
		settings_custom_save_allowed(live_session);

	GT_SetGadgetAttrs(sgads[SGAD_BTN_SAVE], win, NULL,
		GA_Disabled, !allowed, TAG_END);
	if (!allowed && settings_have_cfg && explain)
		settings_set_status(win,
			"Custom crop belongs to another capture path; use Automatic, Save and reboot");
	return allowed;
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
	UWORD fw_version;
	UWORD fw_capabilities;
	struct settings_live_session live_session;
	BOOL done = FALSE;
	WORD w = 0, h = 0;

	fw_version = zz_get_reg16(REG_ZZ_FW_VERSION);
	fw_capabilities = zz_get_reg16(REG_ZZ_FW_CAPABILITIES);
	settings_have_cfg = (fw_version >= 0x0203);
	settings_live_init(&live_session, fw_version, fw_capabilities);

	if (NULL == settings_create_gadgets(&glist, vi, mainlayout,
			fw_capabilities, &w, &h)) {
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

	settings_populate(win, fw_capabilities);
	settings_live_refresh(&live_session);

	if (!settings_have_cfg) {
		/* Live scanline controls stay usable on 2.0-2.2 firmware
		 * (they were on the main window before); everything that
		 * needs the config-file interface is greyed out. */
		static const UWORD cfg_only_gadgets[] = {
			SGAD_VIDEOCAP, SGAD_VCAP_ADVANCED, SGAD_INT2,
			SGAD_MAC, SGAD_HDF, SGAD_OFFSCREEN, SGAD_OVERLAY,
			SGAD_BTN_SAVE, SGAD_BTN_RELOAD
		};
		size_t i;
		for (i = 0; i < sizeof(cfg_only_gadgets) / sizeof(cfg_only_gadgets[0]); i++) {
			GT_SetGadgetAttrs(sgads[cfg_only_gadgets[i]], win, NULL,
				GA_Disabled, TRUE, TAG_END);
		}
	}
	settings_update_save_gate(win, &live_session, FALSE);

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
							if (imsgCode < ZZCFG_VCAP_PROFILE_COUNT &&
								zzcfg_profile_supported(imsgCode, fw_capabilities)) {
								if (live_session.preview_valid &&
									!settings_profile_matches(&live_session,
										imsgCode, settings_vals.videocap_sample)) {
									if (!settings_live_restore(&live_session,
										ZZ_VCAP_ANCHOR_SETTINGS)) {
										GT_SetGadgetAttrs(sgads[SGAD_VIDEOCAP], win,
											NULL, GTCY_Active,
											settings_vals.videocap_profile, TAG_END);
										settings_set_status(win,
											"Profile change waiting for live restore; retry");
										break;
									}
									live_session.preview_valid = FALSE;
								}
								settings_vals.videocap_profile = imsgCode;
								if (settings_update_save_gate(win,
									&live_session, TRUE))
									settings_set_status(win,
										"Native output changed - Save, then power-cycle");
							}
							break;
						case SGAD_VCAP_ADVANCED:
							if (settings_video_advanced_window(mysc, vi, mainlayout,
								&live_session))
								settings_set_status(win,
									"Advanced video staged - Save to persist");
							settings_update_save_gate(win, &live_session, TRUE);
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
						case SGAD_OFFSCREEN:
							settings_vals.offscreen_bitmaps = imsgCode;
							break;
						case SGAD_OVERLAY:
							settings_vals.video_overlay = imsgCode;
							break;
						case SGAD_BTN_SAVE:
							if (!settings_custom_save_allowed(&live_session)) {
								settings_update_save_gate(win, &live_session, TRUE);
							} else if (settings_save(win)) {
								if (live_session.preview_valid) {
									settings_live_refresh(&live_session);
									zz_vcap_anchor_store(&live_session.anchors,
										ZZ_VCAP_ANCHOR_SETTINGS,
										&live_session.current);
								} else if (settings_live_refresh(&live_session)) {
									zz_vcap_anchor_store(&live_session.anchors,
										ZZ_VCAP_ANCHOR_SETTINGS,
										&live_session.current);
								}
								live_session.preview_valid = FALSE;
							}
							break;
						case SGAD_BTN_RELOAD:
							if (settings_live_restore(&live_session,
								ZZ_VCAP_ANCHOR_SETTINGS)) {
								live_session.preview_valid = FALSE;
								settings_populate(win, fw_capabilities);
								settings_update_save_gate(win, &live_session, FALSE);
							} else {
								settings_set_status(win,
									"Reload waiting for acknowledged live restore; retry");
							}
							break;
					}
					break;
				case IDCMP_CLOSEWINDOW:
					if (settings_live_restore(&live_session,
						ZZ_VCAP_ANCHOR_SETTINGS)) {
						done = TRUE;
					} else {
						settings_set_status(win,
							"Close waiting for acknowledged live restore; retry or cold boot");
					}
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

/* ---- Audio control plane (the Audio window, plan R13/R18) ----
 *
 * ZZTop is a client of the firmware-authoritative control plane over
 * zz9k.library, exactly like the AHI/MHI drivers: scene selection and
 * editing, the operator baseline, metering and saving are mailbox
 * opcodes. The master-chain register path this tool used for its LPF
 * slider is gone -- every write commits through the staged scene-write
 * opcode (R2/R4), so the firmware stays the single arbiter.
 */

#define ZZTOP_AUDIO_METER_PERIOD_SECS 1
#define ZZTOP_AUDIO_LPF_MIN 1
#define ZZTOP_AUDIO_LPF_MAX 23900
#define ZZTOP_AUDIO_RANGE_MAX 100
#define ZZTOP_AUDIO_LEVEL_MAX 255
#define ZZTOP_AUDIO_CEILING_MIN 1
#define ZZTOP_AUDIO_CEILING_MAX 4095

static BOOL audio_control_capped = FALSE;
static BOOL audio_metering_capped = FALSE;

static BOOL zztop_audio_surface_available(void)
{
	/* R18 grey-out idiom: the button always exists, but the window
	 * behind it needs the AX codec and a firmware that advertises
	 * ZZ9K_CAP_AUDIO_CONTROL (R16). Until the hardware verification
	 * session advertises the capability (KTD6), every matched set
	 * shows the disabled button -- discoverable, not hidden. */
	return (zz_get_ax_present() != 0) && audio_control_capped;
}

/* ---- Audio debug logging (Project menu toggle) ----
 * Every control-plane mailbox call, its reply, and the decoded
 * readbacks land in T:zztop-audio.log and on the serial debug port
 * (KPrintF; visible in Sashimi). Session-scoped: toggling off closes
 * the file, toggling on starts a fresh one. */
static int audio_log_on;
static BPTR audio_log_fh;

static void audio_log(const char *fmt, ...)
{
	static char buf[200];
	va_list ap;

	if (!audio_log_on)
		return;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (audio_log_fh)
		Write(audio_log_fh, buf, strlen(buf));
	KPrintF((CONST_STRPTR)"ZZTop %s", buf);
}

static void audio_log_toggle(struct Window *win)
{
	struct MenuItem *item;

	if (audio_log_on) {
		audio_log_on = FALSE;
		if (audio_log_fh) {
			Close(audio_log_fh);
			audio_log_fh = (BPTR)0;
		}
		KPrintF((CONST_STRPTR)"ZZTop audio log OFF\n");
	} else {
		audio_log_fh = Open((CONST_STRPTR)"T:zztop-audio.log",
			MODE_NEWFILE);
		audio_log_on = 1;
		audio_log("log opened (fh=%ld)\n",
			(long)(audio_log_fh ? 1 : 0));
	}
	/* Flip the menu checkmark so the toggle state is visible. */
	if (win && zztop_menustrip) {
		item = ItemAddress(zztop_menustrip,
			FULLMENUNUM(0, 5, NOSUB));
		if (item) {
			if (audio_log_on)
				item->Flags |= CHECKED;
			else
				item->Flags &= ~CHECKED;
		}
	}
}

/* One mailbox round-trip: build the typed request, send it, wait for
 * the completion. Runs from the GUI event loop (no Forbid held), so
 * blocking on the completion is the same trade the Settings window
 * makes for its SD-card reads. */
static int audio_mb_call(uint16_t opcode, const void *payload,
	uint16_t payload_len, ZZ9KMailboxEntry *reply)
{
	ZZ9KRequest request;
	const uint32_t *rw = (const uint32_t *)payload;
	const uint32_t *pw = (const uint32_t *)reply->payload.inline_data;
	int status;

	zz9k_request_init(&request, opcode);
	request.entry.payload_len = payload_len;
	memcpy(request.entry.payload.inline_data, payload, payload_len);
	status = ZZ9KCall(&request, reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
	audio_log("op=%04x st=%d req=%08lx,%08lx,%08lx rsp=%08lx,%08lx,%08lx\n",
		(unsigned)opcode, status,
		(unsigned long)zz9k_get_be32((const uint8_t *)&rw[0]),
		(unsigned long)zz9k_get_be32((const uint8_t *)&rw[1]),
		(unsigned long)zz9k_get_be32((const uint8_t *)&rw[2]),
		(unsigned long)zz9k_get_be32((const uint8_t *)&pw[0]),
		(unsigned long)zz9k_get_be32((const uint8_t *)&pw[1]),
		(unsigned long)zz9k_get_be32((const uint8_t *)&pw[2]));
	return status;
}

struct zztop_audio_state {
	UWORD active_scene;
	UWORD scene_count;
	uint32_t baseline; /* ABI packed pair: ch1 = Paula, ch2 = AX */
	uint32_t trim;     /* last applied mixer legs, same packing */
	uint32_t ceiling;  /* derived AX-equivalent policy boundary */
	UWORD ceiling_paula;
	UWORD ceiling_ax;
	UWORD trim_bounded;
	uint32_t save_status; /* ABI save word: QUEUED while saving, else
	                       * the last settled outcome */
};

/* Non-blocking save statuses ride the same staged-header refresh;
 * value-faithful fallbacks keep ZZTop building against either header
 * generation. */
#ifndef ZZ9K_AUDIO_SCENE_SAVE_QUEUED
#define ZZ9K_AUDIO_SCENE_SAVE_QUEUED 3U
#endif
#ifndef ZZ9K_AUDIO_SCENE_SAVE_BUSY
#define ZZ9K_AUDIO_SCENE_SAVE_BUSY 4U
#endif

/* save_status is the append-only tail word (offset 44) of the
 * CONTROL_STATE_GET reply; reading it from the raw bytes works against
 * either header generation for the same reason. */
#define ZZTOP_CONTROL_STATE_SAVE_STATUS_OFF 44

struct zztop_audio_meter {
	uint32_t identity;
	uint32_t clip_count;
	uint32_t underrun_count;
	uint32_t overrun_count;
	uint32_t gain_reduction_events;
	uint32_t peak_hold_ch1;
	uint32_t peak_hold_ch2;
};

static BOOL audio_control_state_get(struct zztop_audio_state *out);

static int audio_scene_select_call(UWORD scene)
{
	ZZ9KAudioSceneSelectPayload payload;
	ZZ9KMailboxEntry reply;

	memset(&payload, 0, sizeof(payload));
	zz9k_put_be32(payload.scene, scene);
	if (audio_mb_call(ZZ9K_OP_AUDIO_SCENE_SELECT, &payload,
			sizeof(payload), &reply) == ZZ9K_STATUS_OK) {
		struct zztop_audio_state st;
		if (audio_control_state_get(&st))
			audio_log("select %u ok; fw scene=%u base=%u/%u trim=%u/%u\n",
				(unsigned)scene + 1U, (unsigned)st.active_scene + 1U,
				(unsigned)ZZ9K_AUDIO_BALANCE_CH1(st.baseline),
				(unsigned)ZZ9K_AUDIO_BALANCE_CH2(st.baseline),
				(unsigned)ZZ9K_AUDIO_BALANCE_CH1(st.trim),
				(unsigned)ZZ9K_AUDIO_BALANCE_CH2(st.trim));
		return ZZ9K_STATUS_OK;
	}
	return ZZ9K_STATUS_TIMEOUT;
}

/* Scene-name parameter (value = two chars: bits 15..8 first, 7..0
 * second; 0x0000 terminates). The ABI constant rides the staged-header
 * refresh from the SDK checkout; keep a value-faithful fallback so
 * ZZTop builds against either header generation. */
#ifndef ZZ9K_AUDIO_SCENE_PARAM_NAME
#define ZZ9K_AUDIO_SCENE_PARAM_NAME 16U
#endif
#ifndef ZZ9K_AUDIO_SCENE_PARAM_CALIBRATION
#define ZZ9K_AUDIO_SCENE_PARAM_CALIBRATION 17U
#define ZZ9K_AUDIO_CALIBRATION_PACK(paula, ax) \
	((uint32_t)((uint32_t)(UWORD)(paula) | \
	 ((uint32_t)(UWORD)(ax) << 16)))
#endif

/* One staged SCENE_WRITE with explicit flags: the rename path stages
 * several chunks before asking for the commit (see below); everything
 * else commits immediately. */
static int audio_scene_write_stage(UWORD scene, uint32_t param,
	uint32_t value, uint32_t flags)
{
	ZZ9KAudioSceneWritePayload payload;
	ZZ9KMailboxEntry reply;

	memset(&payload, 0, sizeof(payload));
	zz9k_put_be32(payload.scene, scene);
	zz9k_put_be32(payload.param, param);
	zz9k_put_be32(payload.value, value);
	zz9k_put_be32(payload.flags, flags);
	audio_log("write scene=%u param=%u val=%u flags=%lx\n",
		(unsigned)scene + 1U, (unsigned)param, (unsigned)value,
		(unsigned long)flags);
	return audio_mb_call(ZZ9K_OP_AUDIO_SCENE_WRITE, &payload,
		sizeof(payload), &reply);
}

/* One staged parameter plus COMMIT (F3/KTD7): the firmware accumulates
 * the stage and commits the group atomically through its fade path, so
 * a release never writes a half master-chain. */
static int audio_scene_write_commit(UWORD scene, uint32_t param,
	uint32_t value)
{
	return audio_scene_write_stage(scene, param, value,
		ZZ9K_AUDIO_SCENE_WRITE_FLAG_COMMIT);
}

/* Scene rename over the staged-write path. The name is eight two-char
 * chunks, zero-padded to the full eight so every send has the same
 * shape. A commit consumes the staging draft, and the first NAME chunk
 * of a fresh draft reopens the name accumulator -- so COMMIT must ride
 * on the LAST chunk only (committing per chunk would collapse the name
 * to its final two characters). The leading 0x0000 guard chunk closes
 * whatever partial name an earlier failed burst left staged, making
 * retries self-healing. A name-only commit is zero DSP writes, so the
 * burst applies instantly. Returns the final (committing) call's
 * status; ZZ9K_STATUS_TIMEOUT means applied-or-coalesced as usual. */
static int audio_scene_rename_call(UWORD scene, const char *name)
{
	size_t len = strlen(name);
	int st = ZZ9K_STATUS_OK;
	int i;

	for (i = 0; i <= ZZTOP_AUDIO_NAME_CHUNKS; i++) {
		uint32_t chunk = 0;
		uint32_t flags = 0;

		if (i > 0) {
			size_t p = 2 * (size_t)(i - 1);

			if (p < len)
				chunk |= (uint32_t)(uint8_t)name[p] << 8;
			if (p + 1 < len)
				chunk |= (uint32_t)(uint8_t)name[p + 1];
		}
		if (i == ZZTOP_AUDIO_NAME_CHUNKS)
			flags = ZZ9K_AUDIO_SCENE_WRITE_FLAG_COMMIT;
		st = audio_scene_write_stage(scene,
			ZZ9K_AUDIO_SCENE_PARAM_NAME, chunk, flags);
		if (st != ZZ9K_STATUS_OK && st != ZZ9K_STATUS_TIMEOUT)
			return st; /* hard error: partial chunks stay staged */
	}
	return st;
}

static BOOL audio_control_state_get(struct zztop_audio_state *out)
{
	ZZ9KAudioControlStateGetPayload payload;
	ZZ9KAudioControlStateResultPayload result;
	ZZ9KMailboxEntry reply;

	memset(&payload, 0, sizeof(payload));
	if (audio_mb_call(ZZ9K_OP_AUDIO_CONTROL_STATE_GET, &payload,
			sizeof(payload), &reply) != ZZ9K_STATUS_OK)
		return FALSE;
	/* The reply payload is byte storage; copy it out before reading
	 * typed fields (the SDK reply-extraction convention). */
	memcpy(&result, reply.payload.inline_data, sizeof(result));
	out->active_scene = (UWORD)zz9k_get_be32(result.active_scene);
	out->scene_count = (UWORD)zz9k_get_be32(result.scene_count);
	out->baseline = zz9k_get_be32(result.baseline);
	out->trim = zz9k_get_be32(result.trim);
	out->trim_bounded = (UWORD)zz9k_get_be32(result.flags);
	out->ceiling = zz9k_get_be32(result.ceiling);
	out->ceiling_paula = (UWORD)zz9k_get_be32(result.ceiling_paula);
	out->ceiling_ax = (UWORD)zz9k_get_be32(result.ceiling_ax);
	out->save_status = zz9k_get_be32(
		&reply.payload.inline_data[ZZTOP_CONTROL_STATE_SAVE_STATUS_OFF]);
	return TRUE;
}

/* One framed meter snapshot (R9). The HOLD_RESET request flag opts into
 * read-and-clear peak-hold, so each poll shows the peak since the
 * previous one without leaving a stale hold for anyone else (R8); a
 * reply that is not a complete single frame is refused rather than
 * half-displayed. */
static BOOL audio_meter_read(uint32_t direction,
	struct zztop_audio_meter *out)
{
	ZZ9KAudioMeterReadPayload payload;
	ZZ9KAudioMeterResultPayload result;
	ZZ9KMailboxEntry reply;

	memset(&payload, 0, sizeof(payload));
	zz9k_put_be32(payload.direction, direction);
	zz9k_put_be32(payload.flags, ZZ9K_AUDIO_METER_RESULT_HOLD_RESET);
	if (audio_mb_call(ZZ9K_OP_AUDIO_METER_READ, &payload,
			sizeof(payload), &reply) != ZZ9K_STATUS_OK)
		return FALSE;
	memcpy(&result, reply.payload.inline_data, sizeof(result));
	if (zz9k_get_be32(result.direction) != direction)
		return FALSE;
	if (zz9k_get_be32(result.frame_count) != 1U ||
			zz9k_get_be32(result.frame) != 0U)
		return FALSE; /* torn or a future multi-frame layout */
	out->identity = zz9k_get_be32(result.identity);
	out->clip_count = zz9k_get_be32(result.clip_count);
	out->underrun_count = zz9k_get_be32(result.underrun_count);
	out->overrun_count = zz9k_get_be32(result.overrun_count);
	out->gain_reduction_events = zz9k_get_be32(result.gain_reduction_events);
	out->peak_hold_ch1 = zz9k_get_be32(result.peak_hold_ch1);
	out->peak_hold_ch2 = zz9k_get_be32(result.peak_hold_ch2);
	audio_log("meter dir=%u peak=%u/%u clip=%u und=%u ovr=%u gr=%u id=%u\n",
		(unsigned)direction, (unsigned)out->peak_hold_ch1,
		(unsigned)out->peak_hold_ch2, (unsigned)out->clip_count,
		(unsigned)out->underrun_count, (unsigned)out->overrun_count,
		(unsigned)out->gain_reduction_events, (unsigned)out->identity);
	return TRUE;
}

static BOOL audio_scene_save_call(UWORD scene, uint32_t *save_status)
{
	ZZ9KAudioSceneSavePayload payload;
	ZZ9KAudioSceneSaveResultPayload result;
	ZZ9KMailboxEntry reply;

	memset(&payload, 0, sizeof(payload));
	zz9k_put_be32(payload.scene, scene);
	if (audio_mb_call(ZZ9K_OP_AUDIO_SCENE_SAVE, &payload,
			sizeof(payload), &reply) != ZZ9K_STATUS_OK)
		return FALSE;
	memcpy(&result, reply.payload.inline_data, sizeof(result));
	*save_status = zz9k_get_be32(result.status);
	return TRUE;
}

/* Editor seed state. The control-plane ABI is append-only and has no
 * scene-read opcode, so the editor seeds from the last saved
 * ZZ9000.CFG (what a reboot restores) overlaid with this session's
 * edits; the live firmware state stays the authority, and because each
 * staged write carries exactly the touched parameter, display drift
 * can never corrupt firmware state. */
struct zztop_audio_scene_ui {
	UWORD lpf;
	UWORD eq[10];
	UWORD prefactor;
	UWORD volume;
	UWORD pan;
	/* Operator-assigned label; "Scene N" until the first rename.
	 * NUL-terminated, printable ASCII, firmware cap 16 chars. */
	char name[ZZTOP_AUDIO_NAME_CHARS + 1];
};

/* Mirror of the firmware's built-in scene defaults (audio_scene.c):
 * display seed only. Same slot count as ZZCFG_AUDIO_SCENES. */
static const struct zztop_audio_scene_ui zztop_audio_scene_defaults[8] = {
	{ 23900, { 50, 50, 50, 50, 50, 50, 50, 50, 50, 50 }, 50, 100, 50, "" },
	{ 23900, { 50, 50, 50, 50, 50, 50, 50, 50, 50, 50 }, 50, 80, 50, "" },
	{ 16000, { 55, 55, 50, 50, 50, 50, 50, 50, 50, 50 }, 50, 75, 50, "" },
	{ 18000, { 50, 50, 50, 50, 50, 50, 55, 55, 50, 50 }, 50, 75, 50, "" },
	{ 23900, { 50, 50, 50, 50, 50, 50, 50, 50, 50, 50 }, 60, 70, 50, "" },
	{ 12000, { 45, 45, 50, 50, 50, 50, 50, 50, 45, 45 }, 50, 90, 50, "" },
	{ 23900, { 50, 50, 50, 50, 50, 50, 50, 50, 50, 50 }, 55, 75, 50, "" },
	{ 23900, { 50, 50, 50, 50, 50, 50, 50, 50, 50, 50 }, 50, 60, 50, "" },
};

static struct zztop_audio_scene_ui audio_scenes[ZZCFG_AUDIO_SCENES];
static UWORD audio_baseline_paula;
static UWORD audio_baseline_ax;
static UWORD audio_ceiling_paula;
static UWORD audio_ceiling_ax;
static BOOL audio_ui_seeded = FALSE;
/* Unsaved-changes contract (R15): edits persist in firmware RAM, so
 * "dirty" is card-wide state that outlives the window; only a
 * successful SCENE_SAVE (or a reboot) clears it. */
static BOOL audio_dirty = FALSE;
/* A QUEUED non-blocking save is in flight in firmware; the 1 s tick
 * settles it through the control-state report's save_status. The
 * generation pair preserves edits made after the firmware serialized
 * the queued snapshot. */
static BOOL audio_save_pending = FALSE;
static ULONG audio_edit_generation;
static ULONG audio_save_generation;

static void audio_mark_dirty(void)
{
	audio_dirty = TRUE;
	audio_edit_generation++;
}

static void audio_scene_default_name(char *buf, size_t size, UWORD scene)
{
	snprintf(buf, size, "Scene %u", (unsigned)(scene + 1));
}

/* Reduce operator input to what the firmware accepts: printable
 * ASCII, trailing spaces trimmed, at most 16 chars. An empty result
 * makes the caller fall back to the default label. */
static void audio_scene_name_clean(const char *in, char *out, size_t size)
{
	size_t n = 0;

	while (*in && n + 1 < size) {
		if (*in >= 0x20 && *in <= 0x7e)
			out[n++] = *in;
		in++;
	}
	out[n] = '\0';
	while (n > 0 && out[n - 1] == ' ')
		out[--n] = '\0';
}

#ifdef ZZCFG_AUDIO_SCENE_NM_CHUNKS
/* Unpack the CFG's packed name chunks (audio_sceneN_nm1..8) into buf.
 * Returns FALSE when the file carries no name keys so the caller
 * keeps the default label; anything non-printable ends the name, so a
 * hand-edited file cannot seed a bad string. */
static BOOL audio_scene_name_from_cfg(const struct zzcfg_values *sv,
	UWORD scene, char *buf)
{
	UWORD mask = sv->audio_scene_mask[scene];
	int n = 0;
	int k;

	for (k = 0; k < ZZTOP_AUDIO_NAME_CHUNKS &&
			k < (int)ZZCFG_AUDIO_SCENE_NM_CHUNKS; k++) {
		UWORD chunk = sv->audio_scene_nm[scene][k];
		char c1, c2;

		if (!(mask & (UWORD)(1u << (8 + k))) || chunk == 0)
			break; /* absent key or terminator chunk */
		c1 = (char)(chunk >> 8);
		if (c1 < 0x20 || c1 > 0x7e)
			break;
		c2 = (char)(chunk & 0xff);
		buf[n++] = c1;
		if (c2 == 0)
			break; /* odd-length name, zero-padded tail */
		if (c2 < 0x20 || c2 > 0x7e)
			break;
		buf[n++] = c2;
	}
	buf[n] = '\0';
	return n > 0;
}
#endif

static void audio_editor_defaults(void)
{
	int i;

	memcpy(audio_scenes, zztop_audio_scene_defaults,
		sizeof(audio_scenes));
	audio_baseline_paula = 128;
	audio_baseline_ax = 64;
	audio_ceiling_paula = 256;
	audio_ceiling_ax = 256;
	for (i = 0; i < ZZCFG_AUDIO_SCENES; i++)
		audio_scene_default_name(audio_scenes[i].name,
			sizeof(audio_scenes[i].name), (UWORD)i);
}

static BOOL audio_seed_editor_state(void)
{
	struct zzcfg_values sv;
	UWORD rawlen = 0;
	UWORD read_status;
	int i, k;


	/* Overlay the saved CFG exactly like the firmware's cold-boot fold
	 * (R10): absent keys keep the built-in defaults. settings_cfg_text
	 * is shared scratch -- the windows are modal, so the Settings
	 * window is never mid-read while this runs. */
	if (zz_get_reg16(REG_ZZ_FW_VERSION) < 0x0203)
		return FALSE; /* no config-file interface to read */
	read_status = zzcfg_read_raw((ULONG)zz_regs, settings_cfg_text,
		ZZCFG_MAX_SIZE, &rawlen);
	audio_log("cfg read st=%u len=%u\n",
		(unsigned)read_status, (unsigned)rawlen);
	if (read_status != ZZ_CFG_FILE_OK)
		return FALSE;
	audio_editor_defaults();

	memset(&sv, 0, sizeof(sv));
	zzcfg_parse_text(settings_cfg_text, rawlen, &sv);
	audio_log("cfg parsed base=%u present=%u active=%u present=%u "
		"s0mask=%04x nm1=%u\n",
		(unsigned)sv.audio_baseline,
		(unsigned)sv.audio_baseline_present,
		(unsigned)sv.audio_active,
		(unsigned)sv.audio_active_present,
		(unsigned)sv.audio_scene_mask[0],
		(unsigned)sv.audio_scene_nm[0][0]);
	for (i = 0; i < ZZCFG_AUDIO_SCENES; i++) {
		UWORD mask = sv.audio_scene_mask[i];

		if (mask & (UWORD)(1u << 0))
			audio_scenes[i].lpf = sv.audio_scene_lpf[i];
		for (k = 0; k < 5; k++) {
			if (mask & (UWORD)(1u << (1 + k))) {
				UWORD packed = sv.audio_scene_eq[i][k];

				audio_scenes[i].eq[2 * k] = packed / 128;
				audio_scenes[i].eq[2 * k + 1] = packed % 128;
			}
		}
		if (mask & (UWORD)(1u << 6)) {
			audio_scenes[i].prefactor = sv.audio_scene_out[i] / 128;
			audio_scenes[i].volume = sv.audio_scene_out[i] % 128;
		}
		if (mask & (UWORD)(1u << 7))
			audio_scenes[i].pan = sv.audio_scene_pan[i];
#ifdef ZZCFG_AUDIO_SCENE_NM_CHUNKS
		{
			char nm[ZZTOP_AUDIO_NAME_CHARS + 1];

			if (audio_scene_name_from_cfg(&sv, (UWORD)i, nm))
				snprintf(audio_scenes[i].name,
					sizeof(audio_scenes[i].name), "%s", nm);
		}
#endif
	}
	if (sv.audio_baseline_present) {
		audio_baseline_paula = sv.audio_baseline >> 8;
		audio_baseline_ax = sv.audio_baseline & 0xff;
	}
	if (sv.audio_ceiling_paula_present &&
			sv.audio_ceiling_ax_present) {
		audio_ceiling_paula = sv.audio_ceiling_paula;
		audio_ceiling_ax = sv.audio_ceiling_ax;
	}
	if (audio_ceiling_paula < ZZTOP_AUDIO_CEILING_MIN ||
			audio_ceiling_paula > ZZTOP_AUDIO_CEILING_MAX)
		audio_ceiling_paula = 256;
	if (audio_ceiling_ax < ZZTOP_AUDIO_CEILING_MIN ||
			audio_ceiling_ax > ZZTOP_AUDIO_CEILING_MAX)
		audio_ceiling_ax = 256;

	/* Clamp to the ranges the firmware enforces on write, so a hand
	 * edited file seeds usable slider positions. */
	for (i = 0; i < ZZCFG_AUDIO_SCENES; i++) {
		if (audio_scenes[i].lpf < ZZTOP_AUDIO_LPF_MIN)
			audio_scenes[i].lpf = ZZTOP_AUDIO_LPF_MIN;
		if (audio_scenes[i].lpf > ZZTOP_AUDIO_LPF_MAX)
			audio_scenes[i].lpf = ZZTOP_AUDIO_LPF_MAX;
		if (audio_scenes[i].prefactor > ZZTOP_AUDIO_RANGE_MAX)
			audio_scenes[i].prefactor = ZZTOP_AUDIO_RANGE_MAX;
		if (audio_scenes[i].volume > ZZTOP_AUDIO_RANGE_MAX)
			audio_scenes[i].volume = ZZTOP_AUDIO_RANGE_MAX;
		if (audio_scenes[i].pan > ZZTOP_AUDIO_RANGE_MAX)
			audio_scenes[i].pan = ZZTOP_AUDIO_RANGE_MAX;
		for (k = 0; k < 10; k++)
			if (audio_scenes[i].eq[k] > ZZTOP_AUDIO_RANGE_MAX)
				audio_scenes[i].eq[k] = ZZTOP_AUDIO_RANGE_MAX;
	}
	return TRUE;
}

static struct Gadget *audgads[AUDGAD_COUNT];
static char audio_status_buf[96];
static void audio_scene_cycle_refresh(struct Window *win, UWORD scene);

static void audio_set_status(struct Window *win, const char *text)
{
	/* callers may pass audio_status_buf itself - don't self-copy */
	if (text != audio_status_buf) {
		snprintf(audio_status_buf, sizeof(audio_status_buf), "%s", text);
	}
	if (win && audgads[AUDGAD_STATUS]) {
		GT_SetGadgetAttrs(audgads[AUDGAD_STATUS], win, NULL,
			GTTX_Text, audio_status_buf, TAG_END);
	}
}

static void audio_reload_saved_state(struct Window *win, UWORD scene)
{
	if (audio_seed_editor_state()) {
		audio_scene_cycle_refresh(win, scene);
		GT_SetGadgetAttrs(audgads[AUDGAD_BASE_PAULA], win, NULL,
			GTSL_Level, audio_baseline_paula, TAG_END);
		GT_SetGadgetAttrs(audgads[AUDGAD_BASE_AX], win, NULL,
			GTSL_Level, audio_baseline_ax, TAG_END);
		GT_SetGadgetAttrs(audgads[AUDGAD_CEIL_PAULA], win, NULL,
			GTIN_Number, audio_ceiling_paula, TAG_END);
		GT_SetGadgetAttrs(audgads[AUDGAD_CEIL_AX], win, NULL,
			GTIN_Number, audio_ceiling_ax, TAG_END);
		audio_set_status(win,
			"Saved - survives power-cycle; controls show saved values");
	} else {
		audio_set_status(win,
			"Saved - readback unavailable; current values kept");
	}
}

static void audio_update_save_gate(struct Window *win)
{
	GT_SetGadgetAttrs(audgads[AUDGAD_BTN_SAVE], win, NULL,
		GA_Disabled, !audio_dirty || audio_save_pending, TAG_END);
}

/* Settle a pending non-blocking save: the firmware machine steps in
 * its service loop, so the outcome arrives through the control-state
 * report. Called from the 1 s meter/poll tick; a still-QUEUED status
 * (or a transient read failure) keeps the pending state and retries
 * on the next tick. */
static void audio_save_settle(struct Window *win)
{
	struct zztop_audio_state st;
	uint32_t save_status;

	if (!audio_save_pending)
		return;
	if (!audio_control_state_get(&st))
		return;
	save_status = st.save_status;
	if (save_status == ZZ9K_AUDIO_SCENE_SAVE_QUEUED)
		return; /* the machine is still running */

	audio_save_pending = FALSE;
	if (save_status == ZZ9K_AUDIO_SCENE_SAVE_OK) {
		if (audio_edit_generation == audio_save_generation) {
			audio_dirty = FALSE;
			/* A fresh read may update the local model. On failure,
			 * keep the committed live values instead of replacing
			 * them with defaults. */
			audio_reload_saved_state(win,
				st.active_scene < ZZCFG_AUDIO_SCENES ?
				(UWORD)st.active_scene : 0);
		} else {
			audio_set_status(win,
				"Saved snapshot; newer edits still need Save");
		}
	} else if (save_status == ZZ9K_AUDIO_SCENE_SAVE_REJECTED) {
		audio_set_status(win,
			"Save rejected: level over boundary");
	} else {
		audio_set_status(win, "Save failed: SD card write error");
	}
	audio_update_save_gate(win);
}

static const char *audio_identity_name(uint32_t identity)
{
	switch (identity) {
	case ZZ9K_AUDIO_METER_IDENTITY_AHI:
		return "AHI";
	case ZZ9K_AUDIO_METER_IDENTITY_MEDIA:
		return "MHI";
	case ZZ9K_AUDIO_METER_IDENTITY_SDK_STREAM:
		return "SDK";
	default:
		return "legacy";
	}
}

/* Peaks are unsigned 16.16 with 0x10000 = digital full scale; show
 * dBFS like every other meter the operator has used. */
static void audio_format_peak(char *buf, size_t size, uint32_t peak)
{
	double db;

	if (peak == 0) {
		snprintf(buf, size, "-oo");
		return;
	}
	db = 20.0 * log10((double)peak / 65536.0);
	if (db <= -99.9) {
		snprintf(buf, size, "-oo");
		return;
	}
	snprintf(buf, size, "%.1f", db);
}

/* Gain-reduction latch (R7/R8): the indicator is re-evaluated only by
 * a meter read and holds its state between reads; a fresh event also
 * echoes into the status line, where it survives until the next
 * operator action overwrites it. */
static char audio_peak_bufs[2][24];
static char audio_counts_bufs[2][32];
static char audio_gr_buf[24];
static uint32_t audio_gr_seen[2];
static BOOL audio_gr_seen_valid[2];

static void audio_refresh_meters(struct Window *win)
{
	static const uint32_t directions[2] = {
		ZZ9K_AUDIO_METER_DIRECTION_OUTPUT,
		ZZ9K_AUDIO_METER_DIRECTION_CAPTURE
	};
	static const UWORD peak_gads[2] = { AUDGAD_OUT_PEAK, AUDGAD_IN_PEAK };
	static const UWORD counts_gads[2] = {
		AUDGAD_OUT_COUNTS, AUDGAD_IN_COUNTS
	};
	uint32_t gr_new = 0;
	int d;

	for (d = 0; d < 2; d++) {
		struct zztop_audio_meter m;
		char lbuf[12], rbuf[12];

		if (!audio_meter_read(directions[d], &m)) {
			snprintf(audio_peak_bufs[d], sizeof(audio_peak_bufs[d]),
				"- / - dB");
			snprintf(audio_counts_bufs[d], sizeof(audio_counts_bufs[d]),
				"n/a");
			continue;
		}

		audio_format_peak(lbuf, sizeof(lbuf), m.peak_hold_ch1);
		audio_format_peak(rbuf, sizeof(rbuf), m.peak_hold_ch2);
		snprintf(audio_peak_bufs[d], sizeof(audio_peak_bufs[d]),
			"%s / %s dB", lbuf, rbuf);
		snprintf(audio_counts_bufs[d], sizeof(audio_counts_bufs[d]),
			"%lu / %lu  %s",
			(unsigned long)m.clip_count,
			(unsigned long)(d == 0 ? m.underrun_count
				: m.overrun_count),
			audio_identity_name(m.identity));

		/* Counters are cumulative and saturating (R8), so a lower
		 * value than the previous read means saturation, not a
		 * reset; count it as one fresh event instead of wrapping. */
		if (!audio_gr_seen_valid[d]) {
			/* First read after the window opened: baseline the
			 * counter so only events watched by this operator
			 * light the indicator. */
			audio_gr_seen_valid[d] = TRUE;
		} else if (m.gain_reduction_events >= audio_gr_seen[d]) {
			gr_new += m.gain_reduction_events - audio_gr_seen[d];
		} else if (m.gain_reduction_events > 0) {
			gr_new++;
		}
		audio_gr_seen[d] = m.gain_reduction_events;
	}

	if (gr_new > 0) {
		snprintf(audio_gr_buf, sizeof(audio_gr_buf), "%lu new",
			(unsigned long)gr_new);
		audio_set_status(win,
			"Gain reduction: scene policy lowered the level");
	} else {
		snprintf(audio_gr_buf, sizeof(audio_gr_buf), "none");
	}

	if (win) {
		for (d = 0; d < 2; d++) {
			GT_SetGadgetAttrs(audgads[peak_gads[d]], win, NULL,
				GTTX_Text, audio_peak_bufs[d], TAG_END);
			GT_SetGadgetAttrs(audgads[counts_gads[d]], win, NULL,
				GTTX_Text, audio_counts_bufs[d], TAG_END);
		}
		GT_SetGadgetAttrs(audgads[AUDGAD_GAIN_RED], win, NULL,
			GTTX_Text, audio_gr_buf, TAG_END);
	}
}

static void audio_arm_meter_timer(struct timerequest *io)
{
	io->tr_node.io_Command = TR_ADDREQUEST;
	io->tr_time.tv_secs = ZZTOP_AUDIO_METER_PERIOD_SECS;
	io->tr_time.tv_micro = 0;
	SendIO((struct IORequest *)io);
}

/* Meter gadgets show n/a when the firmware does not advertise the
 * metering capability; the audio window still serves the control plane
 * (and the non-blocking save poll rides the same tick). */
static void audio_meters_unavailable(struct Window *win)
{
	snprintf(audio_peak_bufs[0], sizeof(audio_peak_bufs[0]), "n/a");
	snprintf(audio_counts_bufs[0], sizeof(audio_counts_bufs[0]), "n/a");
	snprintf(audio_peak_bufs[1], sizeof(audio_peak_bufs[1]), "n/a");
	snprintf(audio_counts_bufs[1], sizeof(audio_counts_bufs[1]), "n/a");
	snprintf(audio_gr_buf, sizeof(audio_gr_buf), "n/a");
	GT_SetGadgetAttrs(audgads[AUDGAD_OUT_PEAK], win, NULL,
		GTTX_Text, audio_peak_bufs[0], TAG_END);
	GT_SetGadgetAttrs(audgads[AUDGAD_OUT_COUNTS], win, NULL,
		GTTX_Text, audio_counts_bufs[0], TAG_END);
	GT_SetGadgetAttrs(audgads[AUDGAD_IN_PEAK], win, NULL,
		GTTX_Text, audio_peak_bufs[1], TAG_END);
	GT_SetGadgetAttrs(audgads[AUDGAD_IN_COUNTS], win, NULL,
		GTTX_Text, audio_counts_bufs[1], TAG_END);
	GT_SetGadgetAttrs(audgads[AUDGAD_GAIN_RED], win, NULL,
		GTTX_Text, audio_gr_buf, TAG_END);
}

static STRPTR audio_scene_labels[ZZCFG_AUDIO_SCENES + 1];

static void audio_scene_labels_bind(void)
{
	int i;

	for (i = 0; i < ZZCFG_AUDIO_SCENES; i++)
		audio_scene_labels[i] = (STRPTR)audio_scenes[i].name;
	audio_scene_labels[ZZCFG_AUDIO_SCENES] = NULL;
}

static void audio_scene_cycle_refresh(struct Window *win, UWORD scene)
{

	if (win && audgads[AUDGAD_SCENE]) {
		GT_SetGadgetAttrs(audgads[AUDGAD_SCENE], win, NULL,
			GTCY_Labels, audio_scene_labels,
			GTCY_Active, scene, TAG_END);
	}
}

static CONST_STRPTR audio_label_samples[] = {
	(CONST_STRPTR)LABEL_AUDIO_SCENE,
	(CONST_STRPTR)LABEL_AUD_OUT_PEAK,
	(CONST_STRPTR)LABEL_AUD_OUT_CNT,
	(CONST_STRPTR)LABEL_AUD_IN_PEAK,
	(CONST_STRPTR)LABEL_AUD_IN_CNT,
	(CONST_STRPTR)LABEL_AUD_GR,
	(CONST_STRPTR)LABEL_AUD_PAULA,
	(CONST_STRPTR)LABEL_AUD_AX,
	(CONST_STRPTR)LABEL_AUD_CEIL_PAULA,
	(CONST_STRPTR)LABEL_AUD_CEIL_AX,
	NULL
};

static CONST_STRPTR audio_value_samples[] = {
	(CONST_STRPTR)"Scene 8",
	(CONST_STRPTR)"-12.0 / -12.0 dB",
	(CONST_STRPTR)"65535 / 65535  legacy",
	(CONST_STRPTR)"32767 new",
	(CONST_STRPTR)"255",
	NULL
};

static CONST_STRPTR audio_button_samples[] = {
	(CONST_STRPTR)LABEL_BTN_EDITSCN,
	(CONST_STRPTR)LABEL_BTN_SAVE,
	(CONST_STRPTR)LABEL_BTN_BALANCE,
	NULL
};

static struct Gadget *audio_create_text(struct Gadget *gad,
	struct NewGadget *ng, UWORD gadget_id, STRPTR label,
	const char *initial)
{
	ng->ng_GadgetID = gadget_id;
	ng->ng_GadgetText = label;

	return CreateGadget(TEXT_KIND, gad, ng,
		GTTX_Text, (STRPTR)initial,
		GTTX_Border, TRUE,
		TAG_END);
}

static struct Gadget *audio_create_gadgets(struct Gadget **glistptr,
	void *vi, const struct ZZTopLayout *mainlayout, UWORD scene,
	WORD *out_w, WORD *out_h)
{
	struct ZZTopLayout l = *mainlayout;
	struct NewGadget ng;
	struct Gadget *gad;
	WORD label_width, value_width, button_width, content_right, y, i;

	/* Same font metrics as the main window, own column widths. */
	{
		struct RastPort *rp = zztop_screen ? &zztop_screen->RastPort : NULL;

		label_width = zztop_max_text_width(rp, audio_label_samples, 8);
		value_width = zztop_max_text_width(rp, audio_value_samples, 8);
		button_width = zztop_max_word(90,
			zztop_max_text_width(rp, audio_button_samples, 8) + 32);
	}
	l.gadget_left = l.margin_x + label_width + l.label_gap;
	l.gadget_width = zztop_max_word(200, value_width + 48);
	content_right = l.gadget_left + l.gadget_width;

	gad = CreateContext(glistptr);
	for (i = 0; i < AUDGAD_COUNT; i++) audgads[i] = NULL;

	y = l.topborder + l.margin_y;

	ng.ng_LeftEdge = l.gadget_left;
	ng.ng_TopEdge = y;
	ng.ng_Width = l.gadget_width;
	ng.ng_Height = l.gadget_height;
	ng.ng_TextAttr = l.text_attr;
	ng.ng_VisualInfo = vi;
	ng.ng_Flags = PLACETEXT_LEFT;

	ng.ng_GadgetID = AUDGAD_SCENE;
	ng.ng_GadgetText = (STRPTR)LABEL_AUDIO_SCENE;
	audgads[AUDGAD_SCENE] = gad = CreateGadget(CYCLE_KIND, gad, &ng,
		GTCY_Labels, audio_scene_labels, GTCY_Active, scene, TAG_END);
	y += l.row_step;

	/* Edit and Rename share the gadget column (AGAD Done/Cancel
	 * precedent); the text gadgets below inherit ng_LeftEdge/ng_Width,
	 * so restore both after the half-width pair. */
	ng.ng_TopEdge = y;
	ng.ng_Width = (l.gadget_width - 16) / 2;
	ng.ng_GadgetID = AUDGAD_BTN_EDIT;
	ng.ng_GadgetText = (STRPTR)LABEL_BTN_EDITSCN;
	ng.ng_Flags = PLACETEXT_IN;
	audgads[AUDGAD_BTN_EDIT] = gad = CreateGadget(BUTTON_KIND, gad, &ng,
		TAG_END);
	ng.ng_LeftEdge = l.gadget_left + ng.ng_Width + 16;
	ng.ng_GadgetID = AUDGAD_BTN_RENAME;
	ng.ng_GadgetText = (STRPTR)LABEL_BTN_RENAME;
	audgads[AUDGAD_BTN_RENAME] = gad = CreateGadget(BUTTON_KIND, gad, &ng,
		TAG_END);
	ng.ng_LeftEdge = l.gadget_left;
	ng.ng_Width = l.gadget_width;
	ng.ng_Flags = PLACETEXT_LEFT;
	y += l.row_step + l.section_gap;

	ng.ng_TopEdge = y;
	audgads[AUDGAD_OUT_PEAK] = gad = audio_create_text(gad, &ng,
		AUDGAD_OUT_PEAK, (STRPTR)LABEL_AUD_OUT_PEAK, "- / - dB");
	y += l.row_step;

	ng.ng_TopEdge = y;
	audgads[AUDGAD_OUT_COUNTS] = gad = audio_create_text(gad, &ng,
		AUDGAD_OUT_COUNTS, (STRPTR)LABEL_AUD_OUT_CNT, "- / -");
	y += l.row_step;

	ng.ng_TopEdge = y;
	audgads[AUDGAD_IN_PEAK] = gad = audio_create_text(gad, &ng,
		AUDGAD_IN_PEAK, (STRPTR)LABEL_AUD_IN_PEAK, "- / - dB");
	y += l.row_step;

	ng.ng_TopEdge = y;
	audgads[AUDGAD_IN_COUNTS] = gad = audio_create_text(gad, &ng,
		AUDGAD_IN_COUNTS, (STRPTR)LABEL_AUD_IN_CNT, "- / -");
	y += l.row_step;

	ng.ng_TopEdge = y;
	audgads[AUDGAD_GAIN_RED] = gad = audio_create_text(gad, &ng,
		AUDGAD_GAIN_RED, (STRPTR)LABEL_AUD_GR, "-");
	y += l.row_step + l.section_gap;

	/* Operator baseline balance (R17), replacing ZZ9K_MIX_LEVELS: a
	 * packed 0..255 pair written through the scene-write path. The
	 * level text sits right of the slider, so keep the reserve. */
	ng.ng_TopEdge = y;
	ng.ng_Width = l.gadget_width - 56;
	ng.ng_GadgetID = AUDGAD_BASE_PAULA;
	ng.ng_GadgetText = (STRPTR)LABEL_AUD_PAULA;
	audgads[AUDGAD_BASE_PAULA] = gad = CreateGadget(SLIDER_KIND, gad, &ng,
		GTSL_Min, 0, GTSL_Max, ZZTOP_AUDIO_LEVEL_MAX,
		GTSL_Level, audio_baseline_paula,
		GTSL_LevelFormat, (STRPTR)"%ld", GTSL_MaxLevelLen, 4,
		GTSL_LevelPlace, PLACETEXT_RIGHT,
		TAG_END);
	y += l.row_step;

	ng.ng_TopEdge = y;
	ng.ng_GadgetID = AUDGAD_BASE_AX;
	ng.ng_GadgetText = (STRPTR)LABEL_AUD_AX;
	audgads[AUDGAD_BASE_AX] = gad = CreateGadget(SLIDER_KIND, gad, &ng,
		GTSL_Min, 0, GTSL_Max, ZZTOP_AUDIO_LEVEL_MAX,
		GTSL_Level, audio_baseline_ax,
		GTSL_LevelFormat, (STRPTR)"%ld", GTSL_MaxLevelLen, 4,
		GTSL_LevelPlace, PLACETEXT_RIGHT,
		TAG_END);
	y += l.row_step + l.section_gap;

	ng.ng_Width = l.gadget_width;
	ng.ng_TopEdge = y;
	ng.ng_GadgetID = AUDGAD_CEIL_PAULA;
	ng.ng_GadgetText = (STRPTR)LABEL_AUD_CEIL_PAULA;
	audgads[AUDGAD_CEIL_PAULA] = gad = CreateGadget(
		INTEGER_KIND, gad, &ng,
		GTIN_Number, audio_ceiling_paula,
		GTIN_MaxChars, 4,
		TAG_END);
	y += l.row_step;

	ng.ng_TopEdge = y;
	ng.ng_GadgetID = AUDGAD_CEIL_AX;
	ng.ng_GadgetText = (STRPTR)LABEL_AUD_CEIL_AX;
	audgads[AUDGAD_CEIL_AX] = gad = CreateGadget(
		INTEGER_KIND, gad, &ng,
		GTIN_Number, audio_ceiling_ax,
		GTIN_MaxChars, 4,
		TAG_END);
	y += l.row_step + l.section_gap;

	/* Status line spans the whole row (no side label) so messages get
	 * the label column's width too instead of widening the window. */
	ng.ng_LeftEdge = l.margin_x;
	ng.ng_TopEdge = y;
	ng.ng_Width = content_right - l.margin_x;
	ng.ng_GadgetID = AUDGAD_STATUS;
	ng.ng_GadgetText = NULL;
	audgads[AUDGAD_STATUS] = gad = CreateGadget(TEXT_KIND, gad, &ng,
		GTTX_Text, audio_status_buf, GTTX_Border, TRUE, TAG_END);
	y += l.row_step + l.section_gap;

	ng.ng_LeftEdge = l.margin_x;
	ng.ng_TopEdge = y;
	ng.ng_Width = button_width;
	ng.ng_GadgetID = AUDGAD_BTN_SAVE;
	ng.ng_GadgetText = (STRPTR)LABEL_BTN_SAVE;
	ng.ng_Flags = PLACETEXT_IN;
	audgads[AUDGAD_BTN_SAVE] = gad = CreateGadget(BUTTON_KIND, gad, &ng,
		GA_Disabled, TRUE, TAG_END);

	/* One-click equal-loudness balance: both legs at the same
	 * fraction (3/8) of their measured ceilings, so Paula and AX
	 * contribute identically in AX-equivalent units on calibrated
	 * cards and keep an equal ratio uncalibrated. 3/8 each, because
	 * the save validator bounds the SUM of both legs to the enforced
	 * boundary (3/4 of the AX ceiling): two equal legs share it, and
	 * the pair composes exactly at the boundary like scene 1. The
	 * ceilings are hardware measurements, not preferences --
	 * deliberately not touched here. */
	ng.ng_LeftEdge = l.margin_x + button_width + l.label_gap;
	ng.ng_TopEdge = y;
	ng.ng_Width = button_width;
	ng.ng_GadgetID = AUDGAD_BTN_BALANCE;
	ng.ng_GadgetText = (STRPTR)LABEL_BTN_BALANCE;
	ng.ng_Flags = PLACETEXT_IN;
	audgads[AUDGAD_BTN_BALANCE] = gad = CreateGadget(BUTTON_KIND, gad,
		&ng, TAG_END);
	y += l.row_step;

	*out_w = content_right + l.margin_x;
	/* Same WA_InnerHeight convention as the main window. */
	*out_h = y + l.gadget_height + (l.margin_y / 2) - l.topborder;

	for (i = 0; i < AUDGAD_COUNT; i++) {
		if (!audgads[i]) return NULL;
	}

	return gad;
}

static BOOL audio_scene_editor_window(struct Screen *mysc, void *vi,
	const struct ZZTopLayout *mainlayout, UWORD scene);

static int audio_scene_rename_window(struct Screen *mysc, void *vi,
	const struct ZZTopLayout *mainlayout, UWORD scene);

enum audio_rename_result {
	AUDIO_RENAME_CANCELLED = 0,
	AUDIO_RENAME_COMMITTED,
	AUDIO_RENAME_COMMITTING, /* final commit timed out: probably done */
	AUDIO_RENAME_FAILED
};

static VOID audio_window(struct Screen *mysc, void *vi,
	const struct ZZTopLayout *mainlayout)
{
	struct Window *win;
	struct Gadget *glist = NULL;
	struct IntuiMessage *imsg;
	struct Gadget *gad;
	struct MsgPort *meter_port = NULL;
	struct timerequest *meter_io = NULL;
	BOOL meter_dev_open = FALSE;
	BOOL meter_pending = FALSE;
	BOOL done = FALSE;
	ULONG imsgClass;
	UWORD imsgCode;
	struct zztop_audio_state state;
	UWORD scene = 0;
	WORD w = 0, h = 0;

	if (!zztop_audio_surface_available()) {
		errorMessage("Audio: AX codec or control-plane capability absent");
		return;
	}

	if (!audio_ui_seeded) {
		if (!audio_seed_editor_state())
			audio_editor_defaults();
		audio_ui_seeded = TRUE;
	}
	/* Live state wins where the ABI permits it: scene, baseline and
	 * calibration may carry unsaved edits from an earlier run. */
	if (audio_control_state_get(&state)) {
		if (state.active_scene < ZZCFG_AUDIO_SCENES)
			scene = state.active_scene;
		audio_baseline_paula =
			(UWORD)ZZ9K_AUDIO_BALANCE_CH1(state.baseline);
		audio_baseline_ax =
			(UWORD)ZZ9K_AUDIO_BALANCE_CH2(state.baseline);
		audio_ceiling_paula = state.ceiling_paula;
		audio_ceiling_ax = state.ceiling_ax;
	}
	audio_scene_labels_bind();

	if (NULL == audio_create_gadgets(&glist, vi, mainlayout, scene,
			&w, &h)) {
		if (glist) FreeGadgets(glist);
		errorMessage("Audio: gadget creation failed");
		return;
	}

	win = OpenWindowTags(NULL,
		WA_Title,        "Audio",
		WA_Gadgets,      glist,   WA_AutoAdjust,    TRUE,
		WA_Width,        w,       WA_MinWidth,      w,
		WA_InnerHeight,  h,       WA_MinHeight,     h,
		WA_DragBar,      TRUE,    WA_DepthGadget,   TRUE,
		WA_Activate,     TRUE,    WA_CloseGadget,   TRUE,
		WA_SizeGadget,   FALSE,   WA_SimpleRefresh, TRUE,
		WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW |
			BUTTONIDCMP | CYCLEIDCMP | SLIDERIDCMP,
		WA_PubScreen, mysc,
		TAG_END);
	if (!win) {
		FreeGadgets(glist);
		errorMessage("Audio: OpenWindow() failed");
		return;
	}

	audio_update_save_gate(win);
	audio_set_status(win, audio_dirty
		? "Unsaved changes - Save to persist"
		: "Edits apply live; reboot reverts to the last Save");

	/* Own timer request, not the shared timerio: the main window may
	 * have a refresh pending on it, and a timerequest cannot be shared
	 * between two outstanding operations. The 1 s tick refreshes the
	 * meters (metering cap) and settles a pending non-blocking save
	 * (control cap), so it runs when either capability is present. */
	if (audio_metering_capped || audio_control_capped) {
		meter_port = CreateMsgPort();
		if (meter_port) {
			meter_io = (struct timerequest *)CreateIORequest(meter_port,
				sizeof(struct timerequest));
			if (meter_io && OpenDevice((STRPTR)TIMERNAME, UNIT_MICROHZ,
					(struct IORequest *)meter_io, 0) == 0) {
				meter_dev_open = TRUE;
			}
		}
		if (audio_metering_capped) {
			audio_gr_seen_valid[0] = FALSE;
			audio_gr_seen_valid[1] = FALSE;
			audio_refresh_meters(win);
		} else {
			audio_meters_unavailable(win);
		}
		if (meter_dev_open) {
			audio_arm_meter_timer(meter_io);
			meter_pending = TRUE;
		}
	} else {
		audio_meters_unavailable(win);
	}

	GT_RefreshWindow(win, NULL);

	while (!done) {
		ULONG user_sig = 1UL << win->UserPort->mp_SigBit;
		ULONG meter_sig = meter_port ? (1UL << meter_port->mp_SigBit) : 0;
		ULONG signals = Wait(user_sig | meter_sig);

		if (meter_sig && (signals & meter_sig) && meter_pending &&
			CheckIO((struct IORequest *)meter_io)) {
			WaitIO((struct IORequest *)meter_io);
			meter_pending = FALSE;
			if (audio_metering_capped)
				audio_refresh_meters(win);
			audio_save_settle(win);
			audio_arm_meter_timer(meter_io);
			meter_pending = TRUE;
		}

		while ((!done) && (imsg = GT_GetIMsg(win->UserPort))) {
			gad = (struct Gadget *)imsg->IAddress;
			imsgClass = imsg->Class;
			imsgCode = imsg->Code;
			GT_ReplyIMsg(imsg);
			audio_log("audio msg class=%08lx id=%ld code=%ld\n",
				(unsigned long)imsgClass,
				gad ? (long)gad->GadgetID : -1L,
				(long)imsgCode);

			switch (imsgClass) {
				case IDCMP_MOUSEMOVE:
					/* Baseline sliders never emit GADGETUP; treat moves
					 * as live commits and let firmware coalesce them. */
					if (gad && (gad->GadgetID == AUDGAD_BASE_PAULA ||
							gad->GadgetID == AUDGAD_BASE_AX))
						imsgClass = IDCMP_GADGETUP;
					else
						break;
					/* FALLTHRU to the GADGETUP dispatch */
				case IDCMP_GADGETUP:
					if (!gad) break;
					switch (gad->GadgetID) {
						case AUDGAD_SCENE:
							if (imsgCode < ZZCFG_AUDIO_SCENES &&
								audio_scene_select_call(imsgCode) ==
									ZZ9K_STATUS_OK) {
								scene = imsgCode;
								audio_mark_dirty();
								audio_set_status(win,
									"Scene switched - Save to persist across reboot");
							} else {
								GT_SetGadgetAttrs(audgads[AUDGAD_SCENE],
									win, NULL, GTCY_Active, scene, TAG_END);
								audio_set_status(win,
									"Scene switch failed (busy) - retry");
							}
							audio_update_save_gate(win);
							break;
						case AUDGAD_BTN_EDIT:
							if (audio_scene_editor_window(mysc, vi,
								mainlayout, scene)) {
								audio_set_status(win,
									"Scene edited - Save to persist across reboot");
							}
							audio_update_save_gate(win);
							break;
						case AUDGAD_BTN_RENAME:
							switch (audio_scene_rename_window(mysc, vi,
								mainlayout, scene)) {
							case AUDIO_RENAME_COMMITTED: {
								char msg[96];

								audio_scene_cycle_refresh(win, scene);
								snprintf(msg, sizeof(msg),
									"Scene %u renamed to %s - Save to persist",
									(unsigned)(scene + 1),
									audio_scenes[scene].name);
								audio_set_status(win, msg);
								break;
							}
							case AUDIO_RENAME_COMMITTING:
								audio_scene_cycle_refresh(win, scene);
								audio_set_status(win,
									"Rename committing - Save to persist");
								break;
							case AUDIO_RENAME_FAILED:
								audio_set_status(win,
									"Rename failed (busy) - retry");
								break;
							default:
								break; /* cancelled: keep the status quo */
							}
							audio_update_save_gate(win);
							break;
						case AUDGAD_BASE_PAULA:
						case AUDGAD_BASE_AX: {
							UWORD old_paula = audio_baseline_paula;
							UWORD old_ax = audio_baseline_ax;
							UWORD new_paula = old_paula;
							UWORD new_ax = old_ax;

							if (gad->GadgetID == AUDGAD_BASE_PAULA)
								new_paula = (UWORD)imsgCode;
							else
								new_ax = (UWORD)imsgCode;
							{
								int bst = audio_scene_write_commit(scene,
									ZZ9K_AUDIO_SCENE_PARAM_BASELINE,
									ZZ9K_AUDIO_BALANCE_PACK(new_paula,
										new_ax));
								if (bst == ZZ9K_STATUS_OK ||
										bst == ZZ9K_STATUS_TIMEOUT) {
									/* Timeout = machine mid-step, reply slow;
									 * the write applied or coalesced. Keep
									 * the user's value; no snap-back. */
									audio_baseline_paula = new_paula;
									audio_baseline_ax = new_ax;
									audio_mark_dirty();
									audio_set_status(win,
										(bst == ZZ9K_STATUS_OK)
										?	"Baseline committed - Save to persist"
										:	"Baseline committing - Save to persist");
								} else {
									/* Hard error: restore. */
									GT_SetGadgetAttrs(gad, win, NULL,
										GTSL_Level, (gad->GadgetID ==
											AUDGAD_BASE_PAULA) ? old_paula
											: old_ax, TAG_END);
									audio_scene_write_commit(scene,
										ZZ9K_AUDIO_SCENE_PARAM_BASELINE,
										ZZ9K_AUDIO_BALANCE_PACK(
											old_paula, old_ax));
									audio_set_status(win,
										"Baseline write failed - retry");
								}
							}
							audio_update_save_gate(win);
							break;
						}
						case AUDGAD_CEIL_PAULA:
						case AUDGAD_CEIL_AX: {
							UWORD old_paula = audio_ceiling_paula;
							UWORD old_ax = audio_ceiling_ax;
							UWORD new_paula = old_paula;
							UWORD new_ax = old_ax;
							LONG entered =
								((struct StringInfo *)gad->SpecialInfo)->LongInt;
							int cst;

							if (entered < ZZTOP_AUDIO_CEILING_MIN ||
									entered > ZZTOP_AUDIO_CEILING_MAX) {
								GT_SetGadgetAttrs(gad, win, NULL,
									GTIN_Number,
									(gad->GadgetID == AUDGAD_CEIL_PAULA)
									? old_paula : old_ax, TAG_END);
								audio_set_status(win,
									"Calibration must be 1..4095");
								break;
							}
							if (gad->GadgetID == AUDGAD_CEIL_PAULA)
								new_paula = (UWORD)entered;
							else
								new_ax = (UWORD)entered;
							cst = audio_scene_write_commit(scene,
								ZZ9K_AUDIO_SCENE_PARAM_CALIBRATION,
								ZZ9K_AUDIO_CALIBRATION_PACK(
									new_paula, new_ax));
							if (cst == ZZ9K_STATUS_OK ||
									cst == ZZ9K_STATUS_TIMEOUT) {
								audio_ceiling_paula = new_paula;
								audio_ceiling_ax = new_ax;
								audio_mark_dirty();
								audio_set_status(win,
									(cst == ZZ9K_STATUS_OK)
									? "Calibration committed - Save to persist"
									: "Calibration committing - Save to persist");
							} else {
								GT_SetGadgetAttrs(gad, win, NULL,
									GTIN_Number, (gad->GadgetID ==
										AUDGAD_CEIL_PAULA) ? old_paula :
										old_ax, TAG_END);
								audio_scene_write_commit(scene,
									ZZ9K_AUDIO_SCENE_PARAM_CALIBRATION,
									ZZ9K_AUDIO_CALIBRATION_PACK(
										old_paula, old_ax));
								audio_set_status(win,
									"Calibration write failed - retry");
							}
							audio_update_save_gate(win);
							break;
						}
						case AUDGAD_BTN_BALANCE: {
							UWORD bal_paula = (UWORD)(
								(3UL * audio_ceiling_paula) / 8UL);
							UWORD bal_ax = (UWORD)(
								(3UL * audio_ceiling_ax) / 8UL);
							int bst = audio_scene_write_commit(
								scene,
								ZZ9K_AUDIO_SCENE_PARAM_BASELINE,
								ZZ9K_AUDIO_BALANCE_PACK(bal_paula,
									bal_ax));
							if (bst == ZZ9K_STATUS_OK ||
									bst == ZZ9K_STATUS_TIMEOUT) {
								audio_baseline_paula = bal_paula;
								audio_baseline_ax = bal_ax;
								audio_mark_dirty();
								GT_SetGadgetAttrs(
									audgads[AUDGAD_BASE_PAULA],
									win, NULL,
									GTSL_Level, bal_paula, TAG_END);
								GT_SetGadgetAttrs(
									audgads[AUDGAD_BASE_AX],
									win, NULL,
									GTSL_Level, bal_ax, TAG_END);
								audio_set_status(win,
									(bst == ZZ9K_STATUS_OK)
									?	"Balance committed - Save to persist"
									:	"Balance committing...");
							} else {
								audio_set_status(win,
									"Balance failed - retry");
							}
							audio_update_save_gate(win);
							break;
						}
						case AUDGAD_BTN_SAVE: {
							uint32_t save_status;

							if (!audio_scene_save_call(scene,
									&save_status)) {
								audio_set_status(win,
									"Save failed: no firmware reply");
								break;
							}
							if (save_status ==
									ZZ9K_AUDIO_SCENE_SAVE_QUEUED) {
								/* The firmware serialized this
								 * generation before returning.
								 * Later edits remain dirty when
								 * this snapshot settles. */
								audio_save_generation =
									audio_edit_generation;
								audio_save_pending = TRUE;
								audio_set_status(win, "Saving...");
							} else if (save_status ==
									ZZ9K_AUDIO_SCENE_SAVE_BUSY) {
								/* This request was refused; do not
								 * treat somebody else's in-flight
								 * snapshot as saving our edits. */
								audio_set_status(win,
									"Save busy - retry");
							} else if (save_status ==
									ZZ9K_AUDIO_SCENE_SAVE_OK) {
								audio_dirty = FALSE;
								audio_reload_saved_state(win, scene);
							} else if (save_status ==
									ZZ9K_AUDIO_SCENE_SAVE_REJECTED) {
								audio_set_status(win,
									"Save rejected: level over boundary");
							} else {
								audio_set_status(win,
									"Save failed: SD card write error");
							}
							audio_update_save_gate(win);
							break;
						}
					}
					break;
				case IDCMP_CLOSEWINDOW:
					/* Closing does not discard unsaved edits: they
					 * persist in firmware RAM until the card is
					 * power-cycled back to the last Save (R15). */
					done = TRUE;
					break;
				case IDCMP_REFRESHWINDOW:
					GT_BeginRefresh(win);
					GT_EndRefresh(win, TRUE);
					break;
			}
		}
	}

	if (meter_pending) {
		if (!CheckIO((struct IORequest *)meter_io))
			AbortIO((struct IORequest *)meter_io);
		WaitIO((struct IORequest *)meter_io);
	}
	if (meter_dev_open)
		CloseDevice((struct IORequest *)meter_io);
	if (meter_io)
		DeleteIORequest((struct IORequest *)meter_io);
	if (meter_port)
		DeleteMsgPort(meter_port);

	CloseWindow(win);
	FreeGadgets(glist);
}

/* Scene editor (sub-window, the settings_video_advanced_window
 * precedent): every master-chain parameter of one scene as a slider.
 * Edits commit on gadget release through the staged scene-write path
 * (F3/KTD7) -- there is deliberately no per-mousemove writing. */
static BOOL audio_scene_editor_window(struct Screen *mysc, void *vi,
	const struct ZZTopLayout *mainlayout, UWORD scene)
{
	static CONST_STRPTR label_samples[] = {
		(CONST_STRPTR)LABEL_AUD_LPF,
		(CONST_STRPTR)LABEL_AUD_PREF,
		(CONST_STRPTR)LABEL_AUD_VOL,
		(CONST_STRPTR)LABEL_AUD_PAN,
		NULL
	};
	static CONST_STRPTR eq_label_samples[] = {
		(CONST_STRPTR)"EQ 1",
		(CONST_STRPTR)"EQ 10",
		NULL
	};
	static char status[96];
	static char eq_labels[10][8] = {
		"EQ 1", "EQ 2", "EQ 3", "EQ 4", "EQ 5",
		"EQ 6", "EQ 7", "EQ 8", "EQ 9", "EQ 10"
	};
	struct ZZTopLayout l = *mainlayout;
	struct NewGadget ng;
	struct Gadget *glist = NULL;
	struct Gadget *gad;
	struct Gadget *segads[SEGAD_COUNT];
	struct Window *win;
	struct IntuiMessage *imsg;
	struct zztop_audio_scene_ui *sc = &audio_scenes[scene];
	ULONG imsg_class;
	UWORD imsg_code;
	WORD label_width, eq_label_width, left, eq_left, slider_width;
	WORD eq_slider_width, level_reserve, eq_level_reserve, col_gap;
	WORD content_right, button_width, y, w, h, i;
	BOOL done = FALSE;
	BOOL edited = FALSE;
	char title[32];

	/* The window carries the operator-assigned name (or the default
	 * label) so the operator always sees which scene is edited. */
	snprintf(title, sizeof(title), "Editing %s", sc->name);
	snprintf(status, sizeof(status),
		"Editing %s - each control commits on release", sc->name);

	label_width = zztop_max_text_width(
		zztop_screen ? &zztop_screen->RastPort : NULL, label_samples, 8);
	eq_label_width = zztop_max_text_width(
		zztop_screen ? &zztop_screen->RastPort : NULL, eq_label_samples, 8);
	slider_width = 150;
	eq_slider_width = 110;
	level_reserve = 56;  /* "%ld Hz" plus padding */
	eq_level_reserve = 32;
	col_gap = 24;
	button_width = 88;

	left = l.margin_x + label_width + l.label_gap;
	eq_left = left + slider_width + level_reserve + col_gap +
		eq_label_width + l.label_gap;
	content_right = eq_left + eq_slider_width + eq_level_reserve;

	gad = CreateContext(&glist);
	for (i = 0; i < SEGAD_COUNT; i++) segads[i] = NULL;

	ng.ng_TextAttr = l.text_attr;
	ng.ng_VisualInfo = vi;
	ng.ng_Flags = PLACETEXT_LEFT;
	ng.ng_Height = l.gadget_height;
	ng.ng_GadgetText = (STRPTR)LABEL_AUD_LPF;

	y = l.topborder + l.margin_y;

	/* Left column: the non-EQ master chain. */
	ng.ng_LeftEdge = left;
	ng.ng_TopEdge = y;
	ng.ng_Width = slider_width;
	ng.ng_GadgetID = SEGAD_LPF;
	segads[SEGAD_LPF] = gad = CreateGadget(SLIDER_KIND, gad, &ng,
		GTSL_Min, ZZTOP_AUDIO_LPF_MIN, GTSL_Max, ZZTOP_AUDIO_LPF_MAX,
		GTSL_Level, sc->lpf,
		GTSL_LevelFormat, (STRPTR)"%ld Hz", GTSL_MaxLevelLen, 8,
		GTSL_LevelPlace, PLACETEXT_RIGHT,
		TAG_END);

	ng.ng_TopEdge = y + l.row_step;
	ng.ng_GadgetID = SEGAD_PREFACTOR;
	ng.ng_GadgetText = (STRPTR)LABEL_AUD_PREF;
	segads[SEGAD_PREFACTOR] = gad = CreateGadget(SLIDER_KIND, gad, &ng,
		GTSL_Min, 0, GTSL_Max, ZZTOP_AUDIO_RANGE_MAX,
		GTSL_Level, sc->prefactor,
		GTSL_LevelFormat, (STRPTR)"%ld", GTSL_MaxLevelLen, 4,
		GTSL_LevelPlace, PLACETEXT_RIGHT,
		TAG_END);

	ng.ng_TopEdge = y + 2 * l.row_step;
	ng.ng_GadgetID = SEGAD_VOLUME;
	ng.ng_GadgetText = (STRPTR)LABEL_AUD_VOL;
	segads[SEGAD_VOLUME] = gad = CreateGadget(SLIDER_KIND, gad, &ng,
		GTSL_Min, 0, GTSL_Max, ZZTOP_AUDIO_RANGE_MAX,
		GTSL_Level, sc->volume,
		GTSL_LevelFormat, (STRPTR)"%ld", GTSL_MaxLevelLen, 4,
		GTSL_LevelPlace, PLACETEXT_RIGHT,
		TAG_END);

	ng.ng_TopEdge = y + 3 * l.row_step;
	ng.ng_GadgetID = SEGAD_PAN;
	ng.ng_GadgetText = (STRPTR)LABEL_AUD_PAN;
	segads[SEGAD_PAN] = gad = CreateGadget(SLIDER_KIND, gad, &ng,
		GTSL_Min, 0, GTSL_Max, ZZTOP_AUDIO_RANGE_MAX,
		GTSL_Level, sc->pan,
		GTSL_LevelFormat, (STRPTR)"%ld", GTSL_MaxLevelLen, 4,
		GTSL_LevelPlace, PLACETEXT_RIGHT,
		TAG_END);

	/* Right column: the ten EQ bands, stacked at row pitch so the
	 * window stays usable on short screens. */
	for (i = 0; i < 10; i++) {
		ng.ng_LeftEdge = eq_left;
		ng.ng_TopEdge = y + (WORD)i * l.row_step;
		ng.ng_Width = eq_slider_width;
		ng.ng_GadgetID = SEGAD_EQ_BASE + i;
		ng.ng_GadgetText = (STRPTR)eq_labels[i];
		segads[SEGAD_EQ_BASE + i] = gad = CreateGadget(SLIDER_KIND, gad,
			&ng,
			GTSL_Min, 0, GTSL_Max, ZZTOP_AUDIO_RANGE_MAX,
			GTSL_Level, sc->eq[i],
			GTSL_LevelFormat, (STRPTR)"%ld", GTSL_MaxLevelLen, 4,
			GTSL_LevelPlace, PLACETEXT_RIGHT,
			TAG_END);
	}
	y += 10 * l.row_step + l.section_gap;

	ng.ng_LeftEdge = l.margin_x;
	ng.ng_TopEdge = y;
	ng.ng_Width = content_right - l.margin_x;
	ng.ng_GadgetID = SEGAD_STATUS;
	ng.ng_GadgetText = NULL;
	segads[SEGAD_STATUS] = gad = CreateGadget(TEXT_KIND, gad, &ng,
		GTTX_Text, status, GTTX_Border, TRUE, TAG_END);
	y += l.row_step + l.section_gap;

	ng.ng_TopEdge = y;
	ng.ng_Width = button_width;
	ng.ng_GadgetID = SEGAD_BTN_DONE;
	ng.ng_GadgetText = (STRPTR)LABEL_AUD_DONE;
	ng.ng_Flags = PLACETEXT_IN;
	segads[SEGAD_BTN_DONE] = gad = CreateGadget(BUTTON_KIND, gad, &ng,
		TAG_END);
	y += l.row_step;

	for (i = 0; i < SEGAD_COUNT; i++) {
		if (!segads[i]) {
			if (glist) FreeGadgets(glist);
			errorMessage("Audio Scene Editor: gadget creation failed");
			return FALSE;
		}
	}

	w = content_right + l.margin_x;
	h = y + l.gadget_height + (l.margin_y / 2) - l.topborder;
	win = OpenWindowTags(NULL,
		WA_Title, title,
		WA_Gadgets, glist, WA_AutoAdjust, TRUE,
		WA_Width, w, WA_MinWidth, w,
		WA_InnerHeight, h, WA_MinHeight, h,
		WA_DragBar, TRUE, WA_DepthGadget, TRUE,
		WA_Activate, TRUE, WA_CloseGadget, TRUE,
		WA_SizeGadget, FALSE, WA_SimpleRefresh, TRUE,
		WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW |
			BUTTONIDCMP | SLIDERIDCMP,
		WA_PubScreen, mysc,
		TAG_END);
	if (!win) {
		FreeGadgets(glist);
		errorMessage("Audio Scene Editor: OpenWindow() failed");
		return FALSE;
	}

	GT_RefreshWindow(win, NULL);
	while (!done) {
		Wait(1UL << win->UserPort->mp_SigBit);

		while ((!done) && (imsg = GT_GetIMsg(win->UserPort))) {
			gad = (struct Gadget *)imsg->IAddress;
			imsg_class = imsg->Class;
			imsg_code = imsg->Code;
			GT_ReplyIMsg(imsg);
			audio_log("editor msg class=%08lx id=%ld code=%ld\n",
				(unsigned long)imsg_class,
				gad ? (long)gad->GadgetID : -1L,
				(long)imsg_code);
			if (imsg_class == IDCMP_CLOSEWINDOW ||
					(imsg_class == IDCMP_GADGETUP && gad &&
					 gad->GadgetID == SEGAD_BTN_DONE)) {
				done = TRUE;
			} else if (imsg_class == IDCMP_REFRESHWINDOW) {
				GT_BeginRefresh(win);
				GT_EndRefresh(win, TRUE);
			} else if ((imsg_class == IDCMP_GADGETUP ||
					imsg_class == IDCMP_MOUSEMOVE) && gad) {
				/* GadTools sliders here report live drags as MOUSEMOVE
				 * and never emit GADGETUP on release (the original LPF
				 * slider committed on MOUSEMOVE for this reason). Commit
				 * every move; the firmware's coalescing commit machine
				 * collapses a drag into a couple of machine runs. */
				int is_move = (imsg_class == IDCMP_MOUSEMOVE);
				UWORD id = gad->GadgetID;
				UWORD *field;
				uint32_t param;
				const char *name;
				UWORD old;

				field = NULL;
				name = NULL;
				param = 0;
				if (id == SEGAD_LPF) {
					field = &sc->lpf;
					param = ZZ9K_AUDIO_SCENE_PARAM_LPF;
					name = "LPF";
				} else if (id == SEGAD_PREFACTOR) {
					field = &sc->prefactor;
					param = ZZ9K_AUDIO_SCENE_PARAM_PREFACTOR;
					name = "Prefactor";
				} else if (id == SEGAD_VOLUME) {
					field = &sc->volume;
					param = ZZ9K_AUDIO_SCENE_PARAM_VOLUME;
					name = "Volume";
				} else if (id == SEGAD_PAN) {
					field = &sc->pan;
					param = ZZ9K_AUDIO_SCENE_PARAM_PAN;
					name = "Pan";
				} else if (id >= SEGAD_EQ_BASE &&
						id < SEGAD_EQ_BASE + 10) {
					field = &sc->eq[id - SEGAD_EQ_BASE];
					param = ZZ9K_AUDIO_SCENE_PARAM_EQ_BAND_1 +
						(id - SEGAD_EQ_BASE);
					name = "EQ band";
				}
				if (field) {
					int cst;
					old = *field;
					cst = audio_scene_write_commit(scene, param,
						imsg_code);
					if (cst == ZZ9K_STATUS_OK) {
						*field = (UWORD)imsg_code;
						audio_mark_dirty();
						edited = TRUE;
						snprintf(status, sizeof(status),
								"%s committed to %s - Save to persist",
								name, sc->name);
					} else if (cst == ZZ9K_STATUS_TIMEOUT) {
						/* Timeout is not rejection: the commit machine
						 * was mid-step and the reply was slow; the
						 * firmware applied or coalesced the write.
						 * Keep the user's value; never fight the
						 * drag with a restore. */
						*field = (UWORD)imsg_code;
						audio_mark_dirty();
						edited = TRUE;
						snprintf(status, sizeof(status),
								"%s committing - Save to persist", name);
					} else if (audio_scene_write_commit(scene, param,
							old) == ZZ9K_STATUS_OK) {
						/* Hard error (bad value): restore. */
						GT_SetGadgetAttrs(segads[id], win, NULL,
							GTSL_Level, old, TAG_END);
						snprintf(status, sizeof(status),
								"%s commit failed - retry", name);
					} else {
						GT_SetGadgetAttrs(segads[id], win, NULL,
							GTSL_Level, old, TAG_END);
						audio_mark_dirty();
						snprintf(status, sizeof(status),
								"%s commit failed; firmware may hold the attempted value - Save",
								name);
					}

					GT_SetGadgetAttrs(segads[SEGAD_STATUS], win, NULL,
						GTTX_Text, status, TAG_END);
				}
			}
		}
	}

	CloseWindow(win);
	FreeGadgets(glist);
	return edited;
}

/* Scene rename (sub-window, the editor's idiom): one string gadget
 * pre-filled with the scene's name, OK/Cancel, Return = OK, Esc or
 * the close gadget = Cancel. OK sends the whole name as one staged
 * chunk burst and only then touches the editor seed, so a failed
 * burst leaves the displayed name truthful. The name reaches
 * ZZ9000.CFG through the usual Save. */
static int audio_scene_rename_window(struct Screen *mysc, void *vi,
	const struct ZZTopLayout *mainlayout, UWORD scene)
{
	static CONST_STRPTR label_samples[] = {
		(CONST_STRPTR)LABEL_AUDIO_SCENE,
		NULL
	};
	static CONST_STRPTR value_samples[] = {
		(CONST_STRPTR)"WWWWWWWWWWWWWWWW",
		NULL
	};
	struct ZZTopLayout l = *mainlayout;
	struct NewGadget ng;
	struct Gadget *glist = NULL;
	struct Gadget *gad;
	struct Gadget *rngads[RNGAD_COUNT];
	struct Window *win;
	struct IntuiMessage *imsg;
	struct StringInfo *si;
	struct zztop_audio_scene_ui *sc = &audio_scenes[scene];
	ULONG imsg_class;
	UWORD imsg_code;
	WORD label_width, value_width, button_width, button_gap, y, w, h, i;
	BOOL done = FALSE;
	char entry[ZZTOP_AUDIO_NAME_CHARS + 1];
	int result = AUDIO_RENAME_CANCELLED;

	label_width = zztop_max_text_width(
		zztop_screen ? &zztop_screen->RastPort : NULL, label_samples, 8);
	value_width = zztop_max_text_width(
		zztop_screen ? &zztop_screen->RastPort : NULL, value_samples, 8);
	l.gadget_left = l.margin_x + label_width + l.label_gap;
	l.gadget_width = zztop_max_word(152, value_width + 24);
	button_width = 88;
	button_gap = 16;

	gad = CreateContext(&glist);
	for (i = 0; i < RNGAD_COUNT; i++) rngads[i] = NULL;
	y = l.topborder + l.margin_y;

	ng.ng_LeftEdge = l.gadget_left;
	ng.ng_TopEdge = y;
	ng.ng_Width = l.gadget_width;
	ng.ng_Height = l.gadget_height;
	ng.ng_TextAttr = l.text_attr;
	ng.ng_VisualInfo = vi;
	ng.ng_Flags = PLACETEXT_LEFT;
	ng.ng_GadgetID = RNGAD_NAME;
	ng.ng_GadgetText = (STRPTR)LABEL_AUDIO_SCENE;
	/* MaxChars includes the trailing NUL (settings MAC precedent). */
	rngads[RNGAD_NAME] = gad = CreateGadget(STRING_KIND, gad, &ng,
		GTST_MaxChars, ZZTOP_AUDIO_NAME_CHARS + 1,
		GTST_String, sc->name,
		TAG_END);
	y += l.row_step + l.section_gap;

	ng.ng_TopEdge = y;
	ng.ng_Width = button_width;
	ng.ng_GadgetID = RNGAD_BTN_OK;
	ng.ng_GadgetText = (STRPTR)"OK";
	ng.ng_Flags = PLACETEXT_IN;
	rngads[RNGAD_BTN_OK] = gad = CreateGadget(BUTTON_KIND, gad, &ng,
		TAG_END);
	ng.ng_LeftEdge = l.gadget_left + button_width + button_gap;
	ng.ng_GadgetID = RNGAD_BTN_CANCEL;
	ng.ng_GadgetText = (STRPTR)"Cancel";
	rngads[RNGAD_BTN_CANCEL] = gad = CreateGadget(BUTTON_KIND, gad, &ng,
		TAG_END);
	y += l.row_step;

	for (i = 0; i < RNGAD_COUNT; i++) {
		if (!rngads[i]) {
			if (glist) FreeGadgets(glist);
			errorMessage("Rename Scene: gadget creation failed");
			return AUDIO_RENAME_CANCELLED;
		}
	}

	w = zztop_max_word(l.gadget_left + l.gadget_width + l.margin_x,
		l.gadget_left + 2 * button_width + button_gap + l.margin_x);
	h = y + l.gadget_height + (l.margin_y / 2) - l.topborder;
	win = OpenWindowTags(NULL,
		WA_Title, "Rename Scene",
		WA_Gadgets, glist, WA_AutoAdjust, TRUE,
		WA_Width, w, WA_MinWidth, w,
		WA_InnerHeight, h, WA_MinHeight, h,
		WA_DragBar, TRUE, WA_DepthGadget, TRUE,
		WA_Activate, TRUE, WA_CloseGadget, TRUE,
		WA_SizeGadget, FALSE, WA_SimpleRefresh, TRUE,
		WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW |
			IDCMP_RAWKEY | BUTTONIDCMP | STRINGIDCMP,
		WA_PubScreen, mysc,
		TAG_END);
	if (!win) {
		FreeGadgets(glist);
		errorMessage("Rename Scene: OpenWindow() failed");
		return AUDIO_RENAME_CANCELLED;
	}

	ActivateGadget(rngads[RNGAD_NAME], win, NULL);
	GT_RefreshWindow(win, NULL);
	while (!done) {
		Wait(1UL << win->UserPort->mp_SigBit);

		while ((!done) && (imsg = GT_GetIMsg(win->UserPort))) {
			BOOL accept = FALSE;

			gad = (struct Gadget *)imsg->IAddress;
			imsg_class = imsg->Class;
			imsg_code = imsg->Code;
			GT_ReplyIMsg(imsg);
			audio_log("rename msg class=%08lx code=%ld\n",
				(unsigned long)imsg_class, (long)imsg_code);

			if (imsg_class == IDCMP_CLOSEWINDOW) {
				done = TRUE;
			} else if (imsg_class == IDCMP_REFRESHWINDOW) {
				GT_BeginRefresh(win);
				GT_EndRefresh(win, TRUE);
			} else if (imsg_class == IDCMP_RAWKEY &&
				imsg_code == VCAP_RAWKEY_ESCAPE) {
				done = TRUE;
			} else if (imsg_class == IDCMP_RAWKEY &&
				(imsg_code == VCAP_RAWKEY_RETURN ||
					imsg_code == VCAP_RAWKEY_KEYPAD_ENTER)) {
				accept = TRUE;
			} else if (imsg_class == IDCMP_GADGETUP && gad) {
				if (gad->GadgetID == RNGAD_BTN_CANCEL) {
					done = TRUE;
				} else if (gad->GadgetID == RNGAD_BTN_OK ||
					gad->GadgetID == RNGAD_NAME) {
					/* Return in the string gadget releases it. */
					accept = TRUE;
				}
			}

			if (accept) {
				int st;

				si = (struct StringInfo *)
					rngads[RNGAD_NAME]->SpecialInfo;
				audio_scene_name_clean((const char *)si->Buffer,
					entry, sizeof(entry));
				if (!entry[0])
					audio_scene_default_name(entry, sizeof(entry),
						scene);
				st = audio_scene_rename_call(scene, entry);
				if (st == ZZ9K_STATUS_OK ||
						st == ZZ9K_STATUS_TIMEOUT) {
					/* Timeout = commit machine mid-step: the name
					 * applied or coalesced; keep it, no snap-back. */
					snprintf(sc->name, sizeof(sc->name), "%s", entry);
					audio_mark_dirty();
					result = (st == ZZ9K_STATUS_OK)
						? AUDIO_RENAME_COMMITTED
						: AUDIO_RENAME_COMMITTING;
				} else {
					/* Hard error: re-send the previous name so the
					 * partial chunks cannot ride a later commit. */
					audio_scene_rename_call(scene, sc->name);
					result = AUDIO_RENAME_FAILED;
				}
				done = TRUE;
			}
		}
	}

	CloseWindow(win);
	FreeGadgets(glist);
	return result;
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
		case MYGAD_BTN_AUDIO: {
			/* R13: every audio control lives behind this window; the
			 * main window is otherwise unchanged. */
			audio_window(zztop_screen, zztop_vi, &zztop_layout);
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

	/* R13/R18: opens the Audio window; greyed out (not hidden) when
	 * the AX codec or the advertised audio-control capability is
	 * absent. */
	ng.ng_LeftEdge	= layout->button_col3;
	ng.ng_GadgetID	 = MYGAD_BTN_AUDIO;
	ng.ng_GadgetText = (STRPTR)LABEL_BTN_AUDIO;

	gads[MYGAD_BTN_AUDIO] = gad = CreateGadget(BUTTON_KIND, gad, &ng,
											GA_Disabled,
											!zztop_audio_surface_available(),
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
							case MENU_ID_FWUPDATE:
								do_fw_update(mywin);
								break;
							case MENU_ID_FWRESTORE:
								do_fw_restore(mywin);
								break;
							case MENU_ID_QUIT:
								terminated = TRUE;
								break;
							case MENU_ID_AUDIOLOG:
								audio_log_toggle(mywin);
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
	if (audio_log_on)
		audio_log_toggle(NULL);
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
							IDCMP_VANILLAKEY | IDCMP_MENUPICK |
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

	if (!(ExpansionBase = (struct ExpansionBase *)
		OpenLibrary((CONST_STRPTR)"expansion.library",0L))) {
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
		CloseLibrary((struct Library *)ExpansionBase);
			CloseLibrary(IntuitionBase);
			return 0;
		}
	}

	zz_regs = (UBYTE*)zz_cd->cd_BoardAddr;
	CloseLibrary((struct Library *)ExpansionBase);

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
			/* zz9k.library is optional like asl.library: without it,
			 * or without the firmware advertising the audio-control
			 * capability, the Audio button stays disabled and every
			 * other feature is unaffected (R16). The caps are read
			 * once here, the way the Settings window reads the
			 * firmware capability register at setup. */
			ZZ9KBase = OpenLibrary((STRPTR)"zz9k.library", 0);
			if (ZZ9KBase) {
				ZZ9KCaps caps;

				if (ZZ9KQueryCaps(&caps) == ZZ9K_STATUS_OK) {
					audio_control_capped =
						(caps.capability_bits & ZZ9K_CAP_AUDIO_CONTROL)
							? TRUE : FALSE;
					audio_metering_capped =
						(caps.capability_bits & ZZ9K_CAP_AUDIO_METERING)
							? TRUE : FALSE;
				}
			}
			gadtoolsWindow();
			if (ZZ9KBase) {
				CloseLibrary(ZZ9KBase);
				ZZ9KBase = NULL;
			}
			if (AslBase) CloseLibrary(AslBase);
			CloseLibrary(GadToolsBase);
		}
		CloseLibrary(GfxBase);
	}
	CloseLibrary(IntuitionBase);

	return 0;
}
