/*
 * MNT ZZ9000 Amiga Graphics Card Driver (ZZ9000.card)
 *
 * Copyright (C) 2016-2026, Lucie L. Hartmann <lucie@mntre.com>
 *													MNT Research GmbH, Berlin
 *													https://mntre.com
 * Copyright (C) 2021,			Bjorn Astrom <beeanyew@gmail.com>
 * Copyright (C) 2026,			Dimitris Panokostas <midwan@gmail.com>
 *
 * More Info: https://mntre.com/zz9000
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * GNU General Public License v3.0 or later
 *
 * https://spdx.org/licenses/GPL-3.0-or-later.html
 */

#include <proto/exec.h>
#include <proto/expansion.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/input.h>
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/libraries.h>
#include <exec/execbase.h>
#include <exec/resident.h>
#include <exec/initializers.h>
#include <clib/debug_protos.h>
#include <devices/inputevent.h>
#include <string.h>
#include <stdint.h>

#include "mntgfx-gcc.h"
#include "zz9000.h"
#include "zzcfg_query.h"
#include "blitter_cache.h"
#include "offscreen_bitmap.h"
#include "overlay_feature.h"

#define STR(s) #s
#define XSTR(s) STR(s)

struct GFXBase {
	struct Library libNode;
	UBYTE Flags;
	UBYTE pad;
	struct ExecBase *ExecBase;
	struct ExpansionBase *ExpansionBase;
	BPTR segList;
	char *Name;
};

struct DOSBase;

#ifndef DEBUG
#define KPrintF(...)
#endif
#define __saveds__

#define DEVICE_VERSION 2
#define DEVICE_REVISION 6
#define REQUIRED_FW_VERSION_MAJOR 2
#define REQUIRED_FW_VERSION_MINOR 0
#define DEVICE_PRIORITY 0
#define DEVICE_ID_STRING "$VER: ZZ9000.card+blitter " XSTR(DEVICE_VERSION) "." XSTR(DEVICE_REVISION) " " DEVICE_DATE
#define DEVICE_NAME "ZZ9000.card"
#define DEVICE_DATE "(19.05.2026)"

int __attribute__((no_reorder)) _start()
{
		return -1;
}

#ifdef DEBUG
void exit(int hmm) {
	while(1);
}
#endif

asm("romtag:\n"
		"				dc.w		"XSTR(RTC_MATCHWORD)"		\n"
		"				dc.l		romtag									\n"
		"				dc.l		endcode									\n"
		"				dc.b		"XSTR(RTF_AUTOINIT)"		\n"
		"				dc.b		"XSTR(DEVICE_VERSION)"	\n"
		"				dc.b		"XSTR(NT_LIBRARY)"			\n"
		"				dc.b		"XSTR(DEVICE_PRIORITY)" \n"
		"				dc.l		_device_name						\n"
		"				dc.l		_device_id_string				\n"
		"				dc.l		_auto_init_tables				\n"
		"endcode:																\n");

char device_name[] = DEVICE_NAME;
char device_id_string[] = DEVICE_ID_STRING;

__saveds struct GFXBase* OpenLib(__REGA6(struct GFXBase *gfxbase));
BPTR __saveds CloseLib(__REGA6(struct GFXBase *gfxbase));
BPTR __saveds ExpungeLib(__REGA6(struct GFXBase *exb));
ULONG ExtFuncLib(void);
__saveds struct GFXBase* InitLib(__REGA6(struct ExecBase *sysbase),
																 __REGA0(BPTR seglist),
																 __REGD0(struct GFXBase *exb));

#define CLOCK_HZ 100000000

static struct GFXBase *_gfxbase;
char *gfxname = "ZZ9000";
char dummies[128];

#define CARDFLAG_ZORRO_3 1
// Place scratch area right after framebuffer? Might be a horrible idea.
#define Z3_GFXDATA_ADDR	(0x3200000 - 0x10000)
#define Z3_TEMPLATE_ADDR (0x3210000 - 0x10000)
#define ZZVMODE_800x600 1
#define ZZVMODE_720x576 6
#define ZZ_CARD_DATA_GFXDATA 0
#define ZZ_CARD_DATA_SCANDBL_800X600 1
#define ZZ_CARD_DATA_NSVSYNC 2
#define ZZ_CARD_DATA_MONITOR_SWITCH 3
#define ZZ_CARD_DATA_DISPLAY_ENABLED 4
#define ZZ_CARD_DATA_SECONDARY_PALETTE 5
#define ZZ_CARD_DATA_OFFSCREEN_BITMAPS 6
#define ZZ_CARD_DATA_VIDEO_OVERLAY 7
/* first firmware whose surface allocator really frees (major<<8|minor) */
#define OFFSCREEN_BITMAPS_MIN_FWREV 0x0204
/* first firmware with OP_VIDEO_OVERLAY + the shadow-scanout compositor */
#define VIDEO_OVERLAY_MIN_FWREV 0x0206
#define MNT_MANUFACTURER 0x6d6e
#define ZZ9000_PRODUCT_Z2 0x3
#define ZZ9000_PRODUCT_Z3 0x4
#define RGBFMT_BIT(format) (1UL << (format))
#define ZZ_RGBFMT_NONE     0
#define ZZ_RGBFMT_CLUT     1
#define ZZ_RGBFMT_BGRA     9
#define ZZ_RGBFMT_R5G6B5   10
#define ZZ_RGBFMT_R5G5B5   11

/*
 * Only advertise formats the firmware/video formatter interprets natively.
 * RGBA and the PC/BGR 15-bit variants use different byte or component order,
 * but the firmware only has one 32-bit mode and one RGB555 mode.
 */
#define ZZ_SUPPORTED_RGB_FORMATS \
	(RGBFMT_BIT(ZZ_RGBFMT_NONE) | \
	 RGBFMT_BIT(ZZ_RGBFMT_CLUT) | \
	 RGBFMT_BIT(ZZ_RGBFMT_BGRA) | \
	 RGBFMT_BIT(ZZ_RGBFMT_R5G6B5) | \
	 RGBFMT_BIT(ZZ_RGBFMT_R5G5B5))

#ifndef CDF_CONFIGME
#define CDF_CONFIGME (1 << 1)
#endif

struct ExecBase *SysBase;
static struct ConfigDev *reserved_cd = NULL;
static struct BlitterRegisterCache blitter_register_cache;
static BOOL zz_overlay_hooks_enabled = FALSE;
/* Card memory addressable from MemoryBase (autoconfig window minus the
 * 64 KB register space). b->MemorySize is SMALLER on Z3: it only caps
 * P96's own VRAM allocator, while the firmware surface heap for
 * off-screen bitmaps sits deliberately ABOVE it (board ~0x3310000+),
 * still inside the autoconfig window. Ownership/on-board checks must
 * bound Planes[0] against this, never against b->MemorySize. */
static ULONG zz_card_window_size = 0;
/* rtg.library's own AllocBitMap/FreeBitMap, captured from the
 * BoardInfo before InitCard installs the ZZ hooks. The PIP source
 * bitmap must come from THIS constructor when available: it returns a
 * bitmap P96 fully manages, placed inside the board window
 * [MemoryBase..MemorySize] that P96's bitmap bookkeeping (board
 * lookups in the LockVLayer/PIP paths) can attribute to this board.
 * Our surface-heap bitmaps sit above that window, which P96's
 * internals cannot attribute to any board. This is the PicassoIV
 * contract: its CreateFeature calls bi->AllocBitMap - the library
 * default, since it installs no hook of its own. */
static struct BitMap * ASM (*zz_p96_alloc_bitmap)(__REGA0(struct BoardInfo *),
	__REGD0(ULONG), __REGD1(ULONG), __REGA1(struct TagItem *)) = NULL;
static BOOL ASM (*zz_p96_free_bitmap)(__REGA0(struct BoardInfo *),
	__REGA1(struct BitMap *), __REGA2(struct TagItem *)) = NULL;

static inline volatile struct GFXData *zz_gfxdata(struct BoardInfo *b) {
	return (volatile struct GFXData *)b->CardData[ZZ_CARD_DATA_GFXDATA];
}

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

static const uint16_t rtg_to_mnt[] = {
	MNTVA_COLOR_8BIT,		// 0x00 -- None
	MNTVA_COLOR_8BIT,		// 0x01 -- 8BPP CLUT
	MNTVA_COLOR_NO_USE,		// 0x02 -- 24BPP RGB
	MNTVA_COLOR_NO_USE,		// 0x03 -- 24BPP BGR
	MNTVA_COLOR_NO_USE,		// 0x04 -- 16BPP R5G6B5PC
	MNTVA_COLOR_NO_USE,		// 0x05 -- 15BPP R5G5B5PC
	MNTVA_COLOR_NO_USE,		// 0x06 -- 32BPP ARGB
	MNTVA_COLOR_NO_USE,		// 0x07 -- 32BPP ABGR
	MNTVA_COLOR_NO_USE,		// 0x08 -- 32BPP RGBA
	MNTVA_COLOR_32BIT,		// 0x09 -- 32BPP BGRA
	MNTVA_COLOR_16BIT565,	// 0x0A -- 16BPP R5G6B5
	MNTVA_COLOR_15BIT,		// 0x0B -- 15BPP R5G5B5
	MNTVA_COLOR_NO_USE,		// 0x0C -- 16BPP B5G6R5PC
	MNTVA_COLOR_NO_USE,		// 0x0D -- 15BPP B5G5R5PC
	MNTVA_COLOR_NO_USE,		// 0x0E -- YUV 4:2:2
	MNTVA_COLOR_NO_USE,		// 0x0F -- YUV 4:1:1
	MNTVA_COLOR_NO_USE,		// 0x10 -- YUV 4:1:1PC
	MNTVA_COLOR_NO_USE,		// 0x11 -- YUV 4:2:2 (Duplicate for some reason)
	MNTVA_COLOR_NO_USE,		// 0x12 -- YUV 4:2:2PC
	MNTVA_COLOR_NO_USE,		// 0x13 -- YUV 4:2:2 Planar
	MNTVA_COLOR_NO_USE,		// 0x14 -- YUV 4:2:2PC Planar
};

static inline uint16_t mnt_colormode(UWORD format) {
	if (format < ARRAY_SIZE(rtg_to_mnt))
		return rtg_to_mnt[format];

	return MNTVA_COLOR_NO_USE;
}

static inline uint16_t panning_colormode(uint16_t colormode) {
	return (colormode == MNTVA_COLOR_15BIT) ? MNTVA_COLOR_16BIT565 : colormode;
}

static inline UBYTE direct_color_mask(uint16_t colormode, UBYTE mask) {
	return (colormode == MNTVA_COLOR_32BIT) ? 0xFF : mask;
}

static inline BOOL supported_rgb_format(RGBFTYPE format) {
	if (format >= 32)
		return FALSE;

	return (ZZ_SUPPORTED_RGB_FORMATS & (1UL << format)) != 0;
}

static BOOL chars_equal_ci(char a, char b) {
	if (a >= 'a' && a <= 'z')
		a -= ('a' - 'A');
	if (b >= 'a' && b <= 'z')
		b -= ('a' - 'A');

	return a == b;
}

static BOOL string_equal_ci(const char *a, const char *b) {
	if (!a || !b)
		return FALSE;

	while (*a && *b) {
		if (!chars_equal_ci(*a, *b))
			return FALSE;
		a++;
		b++;
	}

	return *a == *b;
}

static BOOL tooltype_name_matches(const char *entry, const char *name) {
	const char *p = entry;

	if (!entry || !name)
		return FALSE;

	while (*name && *p && *p != '=') {
		if (!chars_equal_ci(*p, *name))
			return FALSE;
		p++;
		name++;
	}

	return *name == 0 && (*p == 0 || *p == '=');
}

static const char *tooltype_value(char **tool_types, const char *name) {
	if (!tool_types)
		return NULL;

	while (*tool_types) {
		const char *entry = *tool_types;
		if (tooltype_name_matches(entry, name)) {
			while (*entry && *entry != '=')
				entry++;
			return (*entry == '=') ? entry + 1 : "";
		}
		tool_types++;
	}

	return NULL;
}

static BOOL value_is_false(const char *value) {
	return value &&
		(string_equal_ci(value, "NO") ||
		 string_equal_ci(value, "FALSE") ||
		 string_equal_ci(value, "OFF") ||
		 string_equal_ci(value, "0"));
}

static BOOL value_is_true(const char *value) {
	return !value || *value == 0 ||
		string_equal_ci(value, "YES") ||
		string_equal_ci(value, "TRUE") ||
		string_equal_ci(value, "ON") ||
		string_equal_ci(value, "1");
}

static BOOL env_flag_exists(struct DOSBase *DOSBase, const char *name) {
	BPTR f = Open((APTR)name, MODE_OLDFILE);
	if (f) {
		Close(f);
		return TRUE;
	}

	return FALSE;
}

static LONG tooltype_vcap_800x600(char **tool_types, LONG current) {
	const char *value = tooltype_value(tool_types, "ZZ9000-VCAP-800x600");
	if (value)
		current = !value_is_false(value);

	value = tooltype_value(tool_types, "VCAP");
	if (!value)
		return current;

	if (string_equal_ci(value, "800x600") ||
		string_equal_ci(value, "60") ||
		string_equal_ci(value, "60HZ") ||
		value_is_true(value)) {
		return 1;
	}

	if (string_equal_ci(value, "720x576") ||
		string_equal_ci(value, "50") ||
		string_equal_ci(value, "50HZ") ||
		value_is_false(value)) {
		return 0;
	}

	return current;
}

static LONG tooltype_nonstandard_vsync(char **tool_types, LONG current) {
	const char *value = tooltype_value(tool_types, "ZZ9000-NS-VSYNC");
	if (value)
		current = value_is_false(value) ? 0 : 1;

	value = tooltype_value(tool_types, "ZZ9000-NS-VSYNC-NTSC");
	if (value)
		current = value_is_false(value) ? 0 : 2;

	value = tooltype_value(tool_types, "NSVSYNC");
	if (!value)
		return current;

	if (string_equal_ci(value, "NTSC"))
		return 2;
	if (string_equal_ci(value, "PAL") || value_is_true(value))
		return 1;
	if (value_is_false(value))
		return 0;

	return current;
}

static inline void zzwrite16(volatile uint16_t* reg, uint16_t value);

static struct ConfigDev *find_unconfigured_configdev(struct ExpansionBase *ExpansionBase, UWORD manufacturer, UBYTE product) {
	struct ConfigDev *cd = NULL;

	while ((cd = (struct ConfigDev *)FindConfigDev(cd, manufacturer, product))) {
		if ((cd->cd_Flags & CDF_CONFIGME) && cd != reserved_cd)
			return cd;
	}

	return NULL;
}

static void apply_vcap_settings(MNTZZ9KRegs *regs, LONG scandoubler_800x600) {
	if (scandoubler_800x600) {
		KPrintF("ZZ9000.card: 800x600 60hz scandoubler mode.\n");
		regs->videocap_vmode = ZZVMODE_800x600;
	} else {
		KPrintF("ZZ9000.card: 720x576 50hz scandoubler mode.\n");
		regs->videocap_vmode = ZZVMODE_720x576;
	}
}

static void apply_nonstandard_vsync_settings(MNTZZ9KRegs *regs, LONG nonstandard_vsync_mode) {
	zzwrite16(&regs->blitter_user1, CARD_FEATURE_NONSTANDARD_VSYNC);
	zzwrite16(&regs->set_feature_status, (UWORD)nonstandard_vsync_mode);
}

static void apply_card_settings(struct BoardInfo *b, char **tool_types) {
	if (!b || !b->RegisterBase)
		return;

	MNTZZ9KRegs *regs = (MNTZZ9KRegs *)b->RegisterBase;

	b->CardData[ZZ_CARD_DATA_SCANDBL_800X600] = tooltype_vcap_800x600(
		tool_types, (LONG)b->CardData[ZZ_CARD_DATA_SCANDBL_800X600]);
	b->CardData[ZZ_CARD_DATA_NSVSYNC] = tooltype_nonstandard_vsync(
		tool_types, (LONG)b->CardData[ZZ_CARD_DATA_NSVSYNC]);

	apply_vcap_settings(regs, (LONG)b->CardData[ZZ_CARD_DATA_SCANDBL_800X600]);
	apply_nonstandard_vsync_settings(regs, (LONG)b->CardData[ZZ_CARD_DATA_NSVSYNC]);
}

static inline UWORD abs_word(WORD value) {
	return (value < 0) ? (UWORD)(-((LONG)value)) : (UWORD)value;
}

static inline UWORD line_length(const struct Line *line) {
	if (line->Length)
		return line->Length;

	UWORD dx = abs_word(line->dX);
	UWORD dy = abs_word(line->dY);
	return (dx >= dy) ? dx : dy;
}

static inline uint16_t planar_line_bytes(SHORT x, SHORT w) {
	return ((((UWORD)x) & 0x07) + (UWORD)w + 7) >> 3;
}

static inline uint16_t planar_line_bytes_padded(SHORT w) {
	// P2C keeps the historical two-byte padding; the firmware's
	// source byte wrap check otherwise fires at the end of most scanlines.
	return (((UWORD)w) >> 3) + 2;
}

static inline void zzwrite16(volatile uint16_t* reg, uint16_t value) {
	*reg = value;
}

static inline void zzwrite32(volatile uint16_t* reg, uint32_t value) {
	uint16_t *v = (uint16_t *)&value;
	reg[0] = v[0];
	reg[1] = v[1];
}

// Assuming that it takes longer to write the same value through slow ZorroII register access again
// than comparing it with a cached value in FAST RAM, these routines should speed up things a lot.
static inline void writeBlitterSrcOffset(MNTZZ9KRegs* registers, ULONG offset) {
	static ULONG old = 0;
	if (offset != old) {
		zzwrite32(&registers->blitter_src_hi, offset);
		old = offset;
	}
}

static inline void writeBlitterDstOffset(MNTZZ9KRegs* registers, ULONG offset) {
	static ULONG old = 0;
	if (offset != old) {
		zzwrite32(&registers->blitter_dst_hi, offset);
		old = offset;
	}
}

static inline void writeBlitterRGB(MNTZZ9KRegs* registers, ULONG color) {
	static ULONG old = 0;
	if (color != old) {
		zzwrite32(&registers->blitter_rgb_hi, color);
		old = color;
	}
}

static inline void writeBlitterSrcPitch(MNTZZ9KRegs* registers, UWORD srcpitch) {
	if (blitter_cache_src_pitch_write_needed(&blitter_register_cache,
			registers, srcpitch)) {
		zzwrite16(&registers->blitter_src_pitch, srcpitch);
	}
}

static inline void writeBlitterRGB2(MNTZZ9KRegs* registers, ULONG color) {
	if (blitter_cache_write32_needed(&blitter_register_cache, registers,
			BLITTER_CACHE_RGB2, &blitter_register_cache.rgb2, color)) {
		zzwrite32(&registers->blitter_rgb2_hi, color);
	}
}

static inline void writeBlitterUser1(MNTZZ9KRegs* registers, UWORD value) {
	if (blitter_cache_write16_needed(&blitter_register_cache, registers,
			BLITTER_CACHE_USER1, &blitter_register_cache.user1, value)) {
		zzwrite16(&registers->blitter_user1, value);
	}
}

static inline void writeBlitterUser2(MNTZZ9KRegs* registers, UWORD value) {
	if (blitter_cache_write16_needed(&blitter_register_cache, registers,
			BLITTER_CACHE_USER2, &blitter_register_cache.user2, value)) {
		zzwrite16(&registers->blitter_user2, value);
	}
}

/* The line work-variable seed changes almost every line, so caching it would
 * never hit; write it through directly. */
static inline void writeBlitterUser3(MNTZZ9KRegs* registers, UWORD value) {
	zzwrite16(&registers->blitter_user3, value);
}

static inline void writeBlitterDstPitch(MNTZZ9KRegs* registers, UWORD dstpitch) {
	static UWORD old = 0;
	if (dstpitch != old) {
		zzwrite16(&registers->blitter_row_pitch, dstpitch);
		old = dstpitch;
	}
}

static inline void writeBlitterColorMode(MNTZZ9KRegs* registers, UWORD colormode) {
	static UWORD old = MNTVA_COLOR_32BIT;
	if (colormode != old) {
		zzwrite16(&registers->blitter_colormode, colormode);
		old = colormode;
	}
}

static inline void writeBlitterX1(MNTZZ9KRegs* registers, UWORD x) {
	static UWORD old = 0;
	if (x != old) {
		zzwrite16(&registers->blitter_x1, x);
		old = x;
	}
}

static inline void writeBlitterY1(MNTZZ9KRegs* registers, UWORD y) {
	static UWORD old = 0;
	if (y != old) {
		zzwrite16(&registers->blitter_y1, y);
		old = y;
	}
}

static inline void writeBlitterX2(MNTZZ9KRegs* registers, UWORD x) {
	static UWORD old = 0;
	if (x != old) {
		zzwrite16(&registers->blitter_x2, x);
		old = x;
	}
}

static inline void writeBlitterY2(MNTZZ9KRegs* registers, UWORD y) {
	static UWORD old = 0;
	if (y != old) {
		zzwrite16(&registers->blitter_y2, y);
		old = y;
	}
}

static inline void writeBlitterX3(MNTZZ9KRegs* registers, UWORD x) {
	static UWORD old = 0;
	if (x != old) {
		zzwrite16(&registers->blitter_x3, x);
		old = x;
	}
}

static inline void writeBlitterY3(MNTZZ9KRegs* registers, UWORD y) {
	static UWORD old = 0;
	if (y != old) {
		zzwrite16(&registers->blitter_y3, y);
		old = y;
	}
}

#define ZZWRITE16(a, b) *a = b;

#define ZZWRITE32(b, c) \
	zzwrite16(b##_hi, ((uint16_t *)&c)[0]); \
	zzwrite16(b##_lo, ((uint16_t *)&c)[1]);

// useful for debugging
void waitclick() {
#define CIAAPRA ((volatile uint8_t*)0xbfe001)
	// bfe001 http://amigadev.elowar.com/read/ADCD_2.1/Hardware_Manual_guide/node012E.html
	while (!(*CIAAPRA & (1<<6))) {
		// wait for left mouse button pressed
	}
	while ((*CIAAPRA & (1<<6))) {
		// wait for left mouse button released
	}
}

__saveds struct GFXBase* __attribute__((used)) InitLib(__REGA6(struct ExecBase *sysbase),
														 __REGA0(BPTR seglist),
														 __REGD0(struct GFXBase *exb))
{
	_gfxbase = exb;
	_gfxbase->libNode.lib_Node.ln_Name = device_name;
	_gfxbase->libNode.lib_Version = DEVICE_VERSION;
	_gfxbase->libNode.lib_Revision = DEVICE_REVISION;
	_gfxbase->libNode.lib_IdString = device_id_string;
	_gfxbase->Name = gfxname;
	SysBase = *(struct ExecBase **)4L;
	return _gfxbase;
}

__saveds struct GFXBase* __attribute__((used)) OpenLib(__REGA6(struct GFXBase *gfxbase))
{
	gfxbase->libNode.lib_OpenCnt++;
	gfxbase->libNode.lib_Flags &= ~LIBF_DELEXP;

	return gfxbase;
}

BPTR __saveds __attribute__((used)) CloseLib(__REGA6(struct GFXBase *gfxbase))
{
	gfxbase->libNode.lib_OpenCnt--;

	if (!gfxbase->libNode.lib_OpenCnt) {
		if (gfxbase->libNode.lib_Flags & LIBF_DELEXP) {
			return (ExpungeLib(gfxbase));
		}
	}
	return 0;
}

BPTR __saveds __attribute__((used)) ExpungeLib(__REGA6(struct GFXBase *exb))
{
	BPTR seglist;
	struct ExecBase *SysBase = *(struct ExecBase **)4L;

	if(!exb->libNode.lib_OpenCnt) {
		ULONG negsize, possize, fullsize;
		UBYTE *negptr = (UBYTE *)exb;

		seglist = exb->segList;

		Remove((struct Node *)exb);

		negsize	 = exb->libNode.lib_NegSize;
		possize	 = exb->libNode.lib_PosSize;
		fullsize = negsize + possize;
		negptr	-= negsize;

		FreeMem(negptr, fullsize);
		return(seglist);
	}

	exb->libNode.lib_Flags |= LIBF_DELEXP;
	return 0;
}

ULONG ExtFuncLib(void)
{
	return 0;
}

#define LOADLIB(a, b) if ((a = (struct a*)OpenLibrary((STRPTR)b,0L))==NULL) { \
		KPrintF((STRPTR)"ZZ9000.card: Failed to load %s.\n", b); \
		goto cleanup; \
	} \


int __attribute__((used)) FindCard(__REGA0(struct BoardInfo* b)) {
	struct ConfigDev* cd = NULL;
	struct ExpansionBase *ExpansionBase = NULL;
	struct DOSBase *DOSBase = NULL;
	struct IntuitionBase *IntuitionBase = NULL;
	struct ExecBase *SysBase = *(struct ExecBase **)4L;
	LONG zorro_version = 0;
	LONG fwrev_major = 0;
	LONG fwrev_minor = 0;
	LONG fwrev = 0;
	int result = 0;
#ifdef DEBUG
	LONG hwrev = 0;
#endif

	LOADLIB(ExpansionBase, "expansion.library");
	LOADLIB(DOSBase, "dos.library");
	LOADLIB(IntuitionBase, "intuition.library");

	zorro_version = 0;
	b->CardFlags = 0;
	b->CardData[ZZ_CARD_DATA_GFXDATA] = 0;
	b->CardData[ZZ_CARD_DATA_SCANDBL_800X600] = 0;
	b->CardData[ZZ_CARD_DATA_NSVSYNC] = 0;
	b->CardData[ZZ_CARD_DATA_MONITOR_SWITCH] = 1;
	b->CardData[ZZ_CARD_DATA_DISPLAY_ENABLED] = 1;
	b->CardData[ZZ_CARD_DATA_SECONDARY_PALETTE] = 0;
	b->CardData[ZZ_CARD_DATA_OFFSCREEN_BITMAPS] = 1;
	b->CardData[ZZ_CARD_DATA_VIDEO_OVERLAY] = 1;
	if ((cd = find_unconfigured_configdev(ExpansionBase, MNT_MANUFACTURER, ZZ9000_PRODUCT_Z3))) zorro_version = 3;
	else if ((cd = find_unconfigured_configdev(ExpansionBase, MNT_MANUFACTURER, ZZ9000_PRODUCT_Z2))) zorro_version = 2;

	// Find Z3 or Z2 model
	if (zorro_version>=2) {

		b->MemoryBase = (uint8_t*)(cd->cd_BoardAddr)+0x10000;
		zz_card_window_size = (ULONG)cd->cd_BoardSize - 0x10000;
		if (zorro_version == 2) {
			// Top-of-window carve on Z2 (board offsets, 4 MB window):
			// VRAM ends 0x3D0000, template scratch (zz_template_addr =
			// MemorySize, so it slides with this constant) 0x3D0000..
			// 0x3E0000, firmware SDK host-window heap 0x3E0000..
			// 0x3F0000, AHI/MHI audio scratch 0x3F0000..0x400000. The
			// heap slot is what lets zz9k.library map SDK buffers on
			// Z2 (MHI/mpega MP3 playback); an older RTG driver with
			// new firmware would blit templates over it.
			b->MemorySize = cd->cd_BoardSize-0x40000;
		} else {
			// 13.8 MB for Z3 (safety, will be expanded later)
			// one full HD screen @8bit ~ 2MB
			b->MemorySize = 0x3000000 - 0x10000;
			b->CardFlags |= CARDFLAG_ZORRO_3;
			volatile struct GFXData *gd = (struct GFXData*)(((uint32_t)b->MemoryBase) + (uint32_t)Z3_GFXDATA_ADDR);
			b->CardData[ZZ_CARD_DATA_GFXDATA] = (ULONG)gd;
			memset((void *)gd, 0x00, sizeof(struct GFXData));
		}
		b->MemorySpaceBase = b->MemoryBase;
		b->MemorySpaceSize = b->MemorySize;
		b->RegisterBase = (void *)(cd->cd_BoardAddr);
	#ifdef DEBUG
		hwrev = ((uint16_t*)b->RegisterBase)[0];
	#endif
		fwrev = ((uint16_t*)b->RegisterBase)[0xC0/2];
		fwrev_major = fwrev >> 8;
		fwrev_minor = fwrev & 0xFF;

		KPrintF(device_id_string);
		KPrintF("ZZ9000.card: MNT ZZ9000 found. Zorro version %ld.\n", zorro_version);
#ifdef DEBUG
		KPrintF("ZZ9000.card: HW Revision: %ld.\n", hwrev);
#endif
		KPrintF("ZZ9000.card: FW Revision Major: %ld.\n", fwrev_major);
		KPrintF("ZZ9000.card: FW Revision Minor: %ld.\n", fwrev_minor);

		if (fwrev_major < REQUIRED_FW_VERSION_MAJOR ||
			(fwrev_major == REQUIRED_FW_VERSION_MAJOR && fwrev_minor < REQUIRED_FW_VERSION_MINOR)) {
			char *alert = "\x00\x14\x14ZZ9000.card 2.0 needs at least firmware (BOOT.bin) 2.0.\x00\x00";
			DisplayAlert(RECOVERY_ALERT, (APTR)alert, 52);
			goto cleanup;
		}

		MNTZZ9KRegs* registers = (MNTZZ9KRegs *)b->RegisterBase;
		UWORD cfg_present = 0;
		UWORD cfg_value;

		/* Precedence: ENV variable, then ZZ9000.CFG (firmware 2.3+;
		 * the query reports "absent" on older firmware), then the
		 * built-in default. Without this the unconditional re-apply
		 * below would clobber the config file's cold-boot values.
		 *
		 * The default with neither ENV nor config is now 800x600
		 * 60 Hz - the monitor-compatible choice, and what the
		 * firmware, ZZTop and a freshly generated ZZ9000.CFG all
		 * default to. Older ZZ9000.card versions forced 720x576
		 * 50 Hz here; PAL-capable setups select it explicitly with
		 * `videocap_mode = pal` (ZZTop Settings) or the tooltype. */
		if (env_flag_exists(DOSBase, "ENV:ZZ9000-VCAP-800x600")) {
			b->CardData[ZZ_CARD_DATA_SCANDBL_800X600] = 1;
		} else {
			cfg_value = zzcfg_query((ULONG)b->RegisterBase,
				ZZ_CFG_KEY_VIDEOCAP_MODE, &cfg_present);
			b->CardData[ZZ_CARD_DATA_SCANDBL_800X600] =
				(cfg_present && cfg_value == ZZ_VMODE_720x576) ? 0 : 1;
		}

		if (env_flag_exists(DOSBase, "ENV:ZZ9000-NS-VSYNC")) {
			b->CardData[ZZ_CARD_DATA_NSVSYNC] = 1;
		} else if (env_flag_exists(DOSBase, "ENV:ZZ9000-NS-VSYNC-NTSC")) {
			b->CardData[ZZ_CARD_DATA_NSVSYNC] = 2;
		} else {
			cfg_value = zzcfg_query((ULONG)b->RegisterBase,
				ZZ_CFG_KEY_NS_VSYNC, &cfg_present);
			if (cfg_present && cfg_value <= 2)
				b->CardData[ZZ_CARD_DATA_NSVSYNC] = cfg_value;
		}

		if (env_flag_exists(DOSBase, "ENV:ZZ9000-NO-OFFSCREEN")) {
			b->CardData[ZZ_CARD_DATA_OFFSCREEN_BITMAPS] = 0;
		} else {
			cfg_value = zzcfg_query((ULONG)b->RegisterBase,
				ZZ_CFG_KEY_OFFSCREEN_BITMAPS, &cfg_present);
			if (cfg_present)
				b->CardData[ZZ_CARD_DATA_OFFSCREEN_BITMAPS] = (cfg_value != 0);
		}

		if (env_flag_exists(DOSBase, "ENV:ZZ9000-NO-PIP")) {
			b->CardData[ZZ_CARD_DATA_VIDEO_OVERLAY] = 0;
		} else {
			cfg_value = zzcfg_query((ULONG)b->RegisterBase,
				ZZ_CFG_KEY_VIDEO_OVERLAY, &cfg_present);
			if (cfg_present)
				b->CardData[ZZ_CARD_DATA_VIDEO_OVERLAY] = (cfg_value != 0);
		}

		apply_vcap_settings(registers, (LONG)b->CardData[ZZ_CARD_DATA_SCANDBL_800X600]);
		apply_nonstandard_vsync_settings(registers, (LONG)b->CardData[ZZ_CARD_DATA_NSVSYNC]);

		cd->cd_Flags &= ~CDF_CONFIGME;
		reserved_cd = cd;

		result = 1;
		goto cleanup;
	} else {
		KPrintF("ZZ9000.card: MNT ZZ9000 not found!\n");
	}

cleanup:
	if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
	if (DOSBase) CloseLibrary((struct Library *)DOSBase);
	if (ExpansionBase) CloseLibrary((struct Library *)ExpansionBase);

	return result;
}

#define gfxdata zz_gfxdata(b)

int __attribute__((used)) InitCard(__REGA0(struct BoardInfo* b), __REGA1(char **tool_types)) {
	int i;

	b->CardBase = (struct CardBase *)_gfxbase;
	b->ExecBase = SysBase;
	b->BoardName = "ZZ9000";
	b->BoardType = BT_MNT_ZZ9000;
	b->PaletteChipType = PCT_MNT_ZZ9000;
	b->GraphicsControllerType = GCT_MNT_ZZ9000;

	b->Flags |= BIF_GRANTDIRECTACCESS | BIF_HARDWARESPRITE | BIF_FLICKERFIXER | BIF_VGASCREENSPLIT | BIF_PALETTESWITCH | BIF_BLITTER | BIF_VIDEOCAPTURE;
	if (b->CardFlags & CARDFLAG_ZORRO_3)
		b->Flags |= BIF_CACHEMODECHANGE;

	b->MoniSwitch = (UWORD)b->CardData[ZZ_CARD_DATA_MONITOR_SWITCH];
	b->RGBFormats = ZZ_SUPPORTED_RGB_FORMATS;
	b->SoftSpriteFlags = 0;
	b->BitsPerCannon = 8;

	for(i = 0; i < MAXMODES; i++) {
		b->MaxHorValue[i] = 8192;
		b->MaxVerValue[i] = 8192;
		b->MaxHorResolution[i] = 8192;
		b->MaxVerResolution[i] = 8192;
		b->PixelClockCount[i] = 1;
	}

	b->MemoryClock = CLOCK_HZ;

	//b->AllocCardMem = (void *)NULL;
	//b->FreeCardMem = (void *)NULL;
	b->SetSwitch = (void *)SetSwitch;
	b->SetColorArray = (void *)SetColorArray;
	b->SetDAC = (void *)SetDAC;
	b->SetGC = (void *)SetGC;
	b->SetPanning = (void *)SetPanning;
	b->CalculateBytesPerRow = (void *)CalculateBytesPerRow;
	b->CalculateMemory = (void *)CalculateMemory;
	b->GetCompatibleFormats = (void *)GetCompatibleFormats;
	b->SetDisplay = (void *)SetDisplay;

	b->ResolvePixelClock = (void *)ResolvePixelClock;
	b->GetPixelClock = (void *)GetPixelClock;
	b->SetClock = (void *)SetClock;

	b->SetMemoryMode = (void *)SetMemoryMode;
	b->SetWriteMask = (void *)SetWriteMask;
	b->SetClearMask = (void *)SetClearMask;
	b->SetReadPlane = (void *)SetReadPlane;

	b->WaitVerticalSync = (void *)WaitVerticalSync;

	b->WaitBlitter = (void *)WaitBlitter;

	//b->ScrollPlanar = (void *)NULL;
	//b->UpdatePlanar = (void *)NULL;

	b->BlitPlanar2Chunky = (void *)BlitPlanar2Chunky;
	b->BlitPlanar2Direct = (void *)BlitPlanar2Direct;

	b->FillRect = (void *)FillRect;
	b->InvertRect = (void *)InvertRect;
	b->BlitRect = (void *)BlitRect;
	b->BlitTemplate = (void *)BlitTemplate;
	b->BlitPattern = (void *)BlitPattern;
	b->DrawLine = (void *)DrawLine;
	b->BlitRectNoMaskComplete = (void *)BlitRectNoMaskComplete;
	//b->EnableSoftSprite = (void *)NULL;

	//b->AllocCardMemAbs = (void *)NULL;
	b->SetSplitPosition = (void *)SetSplitPosition;
	//b->ReInitMemory = (void *)NULL;
	/* Stage A contract probe: logs + delegates to WriteYUVRectDefault
	 * (pure pass-through in release builds). See ZZ_WriteYUVRect. */
	b->WriteYUVRect = (void *)ZZ_WriteYUVRect;
	b->GetVSyncState = (void *)GetVSyncState;
	//b->GetVBeamPos = (void *)NULL;
	//b->SetDPMSLevel = (void *)NULL;
	//b->ResetChip = (void *)NULL;
	/* Off-screen bitmaps in card VRAM (Z3 only; ZZ_AllocBitMap refuses
	 * on Z2). Requires firmware 2.4+: its surface allocator actually
	 * frees, while older firmware bump-allocates with a no-op free and
	 * would leak VRAM on every bitmap free. Kill switch on capable
	 * firmware: ENV:ZZ9000-NO-OFFSCREEN, the ZZ9000.CFG
	 * offscreen_bitmaps key (both read in FindCard), or the
	 * OFFSCREENBITMAPS tooltype, which wins over both. */
	{
		UWORD fwrev = ((volatile uint16_t*)b->RegisterBase)[0xC0/2];
		BOOL offscreen = (BOOL)b->CardData[ZZ_CARD_DATA_OFFSCREEN_BITMAPS];
		const char *value = tooltype_value(tool_types, "OFFSCREENBITMAPS");
		if (value)
			offscreen = !value_is_false(value);
		/* capture the library defaults (NULL if rtg.library fills
		 * them only after InitCard) before installing our hooks;
		 * ZZ_CreateFeature prefers the saved constructor */
		zz_p96_alloc_bitmap = b->AllocBitMap;
		zz_p96_free_bitmap = b->FreeBitMap;
#ifdef DEBUG
		KPrintF("ZZ9000: InitCard P96 AllocBitMap=%08lx FreeBitMap=%08lx\n",
			(ULONG)zz_p96_alloc_bitmap, (ULONG)zz_p96_free_bitmap);
#endif
		if (offscreen && fwrev >= OFFSCREEN_BITMAPS_MIN_FWREV &&
				(b->CardFlags & CARDFLAG_ZORRO_3)) {
			b->AllocBitMap = (void *)ZZ_AllocBitMap;
			b->FreeBitMap = (void *)ZZ_FreeBitMap;
			b->GetBitMapAttr = (void *)ZZ_GetBitMapAttr;
		}
	}

	b->SetSprite = (void *)SetSprite;
	b->SetSpritePosition = (void *)SetSpritePosition;
	b->SetSpriteImage = (void *)SetSpriteImage;
	b->SetSpriteColor = (void *)SetSpriteColor;

	/* P96 video window (PIP): Features API + BIF_VIDEOWINDOW. Requires
	 * firmware 2.6+ (OP_VIDEO_OVERLAY compositor) and the off-screen
	 * bitmap hooks (CreateFeature allocates the YUV source through
	 * AllocBitMap). Kill switch: ENV:ZZ9000-NO-PIP, the ZZ9000.CFG
	 * video_overlay key (both read in FindCard), or the VIDEOPIP
	 * tooltype, which wins over both. */
	{
		UWORD fwrev = ((volatile uint16_t*)b->RegisterBase)[0xC0/2];
		BOOL pip = (BOOL)b->CardData[ZZ_CARD_DATA_VIDEO_OVERLAY];
		const char *value = tooltype_value(tool_types, "VIDEOPIP");
		if (value)
			pip = !value_is_false(value);
		if (pip && fwrev >= VIDEO_OVERLAY_MIN_FWREV &&
				(b->CardFlags & CARDFLAG_ZORRO_3) && b->AllocBitMap) {
			b->Flags |= BIF_VIDEOWINDOW;
			b->GetFeatureAttrs = (void *)ZZ_GetFeatureAttrs;
			b->CreateFeature = (void *)ZZ_CreateFeature;
			b->SetFeatureAttrs = (void *)ZZ_SetFeatureAttrs;
			b->DeleteFeature = (void *)ZZ_DeleteFeature;
			zz_overlay_hooks_enabled = TRUE;
		}
	}

	apply_card_settings(b, tool_types);
	blitter_cache_reset(&blitter_register_cache);

	return 1;
}

// None of these five really have to do anything.
void SetDAC (__REGA0(struct BoardInfo *b), __REGD7(RGBFTYPE format)) { }
void WaitBlitter (__REGA0(struct BoardInfo *b)) {
	if (!b || !b->RegisterBase)
		return;

	MNTZZ9KRegs *regs = (MNTZZ9KRegs *)b->RegisterBase;
	// Firmware register writes dispatch the blitter synchronously; this gives
	// P96's wait hook a register-read fence without inventing a fake busy bit.
	volatile UWORD fence = regs->blitter_dma_op;
	fence = regs->blitter_acc_op;
	(void)fence;
}
void SetClock (__REGA0(struct BoardInfo *b)) { }
void SetMemoryMode (__REGA0(struct BoardInfo *b), __REGD7(RGBFTYPE format)) { }
void SetWriteMask (__REGA0(struct BoardInfo *b), __REGD0(UBYTE mask)) { }
void SetClearMask (__REGA0(struct BoardInfo *b), __REGD0(UBYTE mask)) { }
void SetReadPlane (__REGA0(struct BoardInfo *b), __REGD0(UBYTE plane)) { }

// Optional dummy read for tricking the 68k cache on processors with occasional garbage output on screen
#ifdef DUMMY_CACHE_READ
	#define dmy_cache memcpy(dummies, (uint32_t *)(uint32_t)0x7F00000, 4);
#else
	#define dmy_cache
#endif

static BOOL modeline_id(uint16_t w, uint16_t h, uint16_t *mode) {
	if (w == 1280 && h == 720) {
		*mode = 0;
	} else if (w == 800 && h == 600) {
		*mode = 1;
	} else if (w == 640 && h == 480) {
		*mode = 2;
	} else if (w == 640 && h == 400) {
		*mode = 16;
	} else if (w == 1024 && h == 768) {
		*mode = 3;
	} else if (w == 1280 && h == 1024) {
		*mode = 4;
	} else if (w == 1920 && h == 1080) {
		*mode = 5;
	} else if (w == 720 && h == 576) {
		*mode = 6;
	} else if (w == 720 && h == 480) {
		*mode = 8;
	} else if (w == 640 && h == 512) {
		*mode = 9;
	} else if (w == 1600 && h == 1200) {
		*mode = 10;
	} else if (w == 2560 && h == 1440) {
		*mode = 11;
	} else if (w == 1920 && h == 800) {
		*mode = 17;
	} else {
		return FALSE;
	}

	return TRUE;
}

static BOOL adjusted_mode_dimensions(struct ModeInfo *mode_info, uint16_t *w, uint16_t *h, uint16_t *scale) {
	uint16_t mode;

	if (!mode_info || mode_info->Width < 320 || mode_info->Height < 200)
		return FALSE;

	if (mode_info->Height >= 480 || mode_info->Width >= 640) {
		*scale = 0;
		*w = mode_info->Width;
		*h = mode_info->Height;
	} else {
		// small doublescan modes are scaled 2x
		// and output as 640x480 wrapped in 800x600 sync
		*scale = 3;
		*w = 2 * mode_info->Width;
		*h = 2 * mode_info->Height;
	}

	return modeline_id(*w, *h, &mode);
}

static BOOL init_modeline(MNTZZ9KRegs* registers, uint16_t w, uint16_t h, uint8_t colormode, uint8_t scalemode) {
	uint16_t mode;

	if (!modeline_id(w, h, &mode))
		return FALSE;

	zzwrite16(&registers->mode, mode|(colormode<<8)|(scalemode<<12));
	return TRUE;
}

static inline void sanitize_mode_flags(struct ModeInfo *mode_info) {
	mode_info->Flags &= (UBYTE)~(GMF_INTERLACE | GMF_DOUBLECLOCK);
}

void SetGC(__REGA0(struct BoardInfo *b), __REGA1(struct ModeInfo *mode_info), __REGD0(BOOL border)) {
	uint16_t scale = 0;
	uint16_t w;
	uint16_t h;
	uint16_t colormode;

	if (!b || !mode_info)
		return;

	MNTZZ9KRegs* registers = (MNTZZ9KRegs *)b->RegisterBase;

	b->ModeInfo = mode_info;
	b->Border = border;
	sanitize_mode_flags(mode_info);

	if (mode_info->Width < 320 || mode_info->Height < 200)
		return;

	colormode = mnt_colormode(b->RGBFormat);
	if (colormode == MNTVA_COLOR_NO_USE)
		return;

	if (!adjusted_mode_dimensions(mode_info, &w, &h, &scale))
		return;

	if (!init_modeline(registers, w, h, colormode, scale)) {
		KPrintF("ZZ9000.card: unsupported mode %ldx%ld.\n", (LONG)w, (LONG)h);
	}
}

UWORD SetSwitch(__REGA0(struct BoardInfo *b), __REGD0(UWORD enabled)) {
	UWORD old = b ? (UWORD)b->CardData[ZZ_CARD_DATA_MONITOR_SWITCH] : 1;

	if (!b || !b->RegisterBase)
		return old;

	b->CardData[ZZ_CARD_DATA_MONITOR_SWITCH] = enabled ? 1 : 0;
	b->MoniSwitch = (UWORD)b->CardData[ZZ_CARD_DATA_MONITOR_SWITCH];
	MNTZZ9KRegs* registers = (MNTZZ9KRegs *)b->RegisterBase;

	if (enabled == 0) {
		// capture 24 bit amiga video to 0xe00000

		if (b->CardData[ZZ_CARD_DATA_SCANDBL_800X600]) {
			// slightly adjusted centering
			zzwrite16(&registers->pan_ptr_hi, 0x00df);
			zzwrite16(&registers->pan_ptr_lo, 0xf2f8);
		} else {
			zzwrite16(&registers->pan_ptr_hi, 0x00e0);
			zzwrite16(&registers->pan_ptr_lo, 0x0000);
		}

		// firmware will detect that we are capturing and viewing the capture area
		// and switch to the appropriate video mode (VCAP_MODE)
		*(volatile uint16_t*)((uint32_t)registers + 0x1006) = 1; // capture mode
	} else {
		// rtg mode
		*(volatile uint16_t*)((uint32_t)registers + 0x1006) = 0; // capture mode

		if (b->ModeInfo)
			SetGC(b, b->ModeInfo, b->Border);
	}

	return old;
}

void SetPanning(__REGA0(struct BoardInfo *b), __REGA1(UBYTE *addr), __REGD0(UWORD width), __REGD1(WORD x_offset), __REGD2(WORD y_offset), __REGD4(UWORD height), __REGD7(RGBFTYPE format)) {
	if (!b) return;
	b->XOffset = x_offset;
	b->YOffset = y_offset;
	MNTZZ9KRegs* registers = (MNTZZ9KRegs *)b->RegisterBase;

	if (b->CardFlags & CARDFLAG_ZORRO_3) {
		dmy_cache
		gfxdata->offset[0] = ((uint32_t)addr - (uint32_t)b->MemoryBase);

		gfxdata->x[0] = x_offset;
		gfxdata->y[0] = y_offset;
		gfxdata->x[1] = width;
		gfxdata->u8_user[GFXDATA_U8_COLORMODE] = (uint8)panning_colormode(mnt_colormode(format & 0xFF));
		zzwrite16(&registers->blitter_dma_op, OP_PAN);
	} else {
		uint32_t offset = ((uint32_t)addr - (uint32_t)b->MemoryBase);

		writeBlitterX1(registers, x_offset);
		writeBlitterY1(registers, y_offset);
		writeBlitterX2(registers, width);
		writeBlitterColorMode(registers, panning_colormode(mnt_colormode(format & 0xFF)));
		zzwrite32(&registers->pan_ptr_hi, offset);
	}
}

void SetColorArray(__REGA0(struct BoardInfo *b), __REGD0(UWORD start), __REGD1(UWORD num)) {
	if (!b || !num)
		return;

	MNTZZ9KRegs* registers = (MNTZZ9KRegs *)b->RegisterBase;
	struct CLUTEntry *clut = (start >= 256) ? b->SecondaryCLUT : b->CLUT;
	UWORD count = (num > 256) ? 256 : num;

	if (start >= 256) {
		if (!b->CardData[ZZ_CARD_DATA_SECONDARY_PALETTE]) {
			writeBlitterUser1(registers, CARD_FEATURE_SECONDARY_PALETTE);
			zzwrite16(&registers->set_feature_status, 1);
			b->CardData[ZZ_CARD_DATA_SECONDARY_PALETTE] = 1;
		}
	}

	if (b->CardFlags & CARDFLAG_ZORRO_3) {
		dmy_cache
		for (int i = 0; i < count; i++) {
			int ci = (start + i) & 0xFF;
			gfxdata->clut1[i * 3]     = clut[ci].Red;
			gfxdata->clut1[i * 3 + 1] = clut[ci].Green;
			gfxdata->clut1[i * 3 + 2] = clut[ci].Blue;
		}
		gfxdata->user[0] = start;
		gfxdata->user[1] = count;
		gfxdata->u8_user[0] = (start >= 256) ? 1 : 0;
		zzwrite16(&registers->blitter_dma_op, OP_SET_PALETTE);
	} else {
		int op = (start >= 256) ? 19 : 3;
		for (int i = 0; i < count; i++) {
			int ci = (start + i) & 0xFF;
			unsigned long xrgb = ((uint32_t)ci << 24) | ((uint32_t)clut[ci].Red << 16) | ((uint32_t)clut[ci].Green << 8) | ((uint32_t)clut[ci].Blue);

			*(volatile uint16_t*)((uint32_t)registers + 0x1000) = xrgb >> 16;
			*(volatile uint16_t*)((uint32_t)registers + 0x1002) = xrgb & 0xFFFF;
			*(volatile uint16_t*)((uint32_t)registers + 0x1004) = op;
			*(volatile uint16_t*)((uint32_t)registers + 0x1004) = 0;
		}
	}
}

uint16_t calc_pitch_bytes(uint16_t w, uint16_t colormode) {
	switch (colormode) {
		case MNTVA_COLOR_8BIT:
			return w;
		case MNTVA_COLOR_16BIT565:
		case MNTVA_COLOR_15BIT:
			return w << 1;
		case MNTVA_COLOR_32BIT:
			return w << 2;
	}

	return 0;
}

uint16_t pitch_to_shift(uint16_t p) {
	if (p == 8192) return 13;
	if (p == 4096) return 12;
	if (p == 2048) return 11;
	if (p == 1024) return 10;
	if (p == 512) return 9;
	if (p == 256)	return 8;
	return 0;
}

UWORD CalculateBytesPerRow(__REGA0(struct BoardInfo *b), __REGA1(struct ModeInfo *mode_info), __REGD0(UWORD width), __REGD1(UWORD height), __REGD7(RGBFTYPE format)) {
	if (!b)
		return 0;
	if (!supported_rgb_format(format))
		return 0;

	return calc_pitch_bytes(width, mnt_colormode(format));
}

APTR CalculateMemory(__REGA0(struct BoardInfo *b), __REGA1(APTR addr), __REGD0(struct RenderInfo *r), __REGD7(RGBFTYPE format)) {
	if (!b || !supported_rgb_format(format))
		return NULL;

	return addr;
}

ULONG GetCompatibleFormats(__REGA0(struct BoardInfo *b), __REGD7(RGBFTYPE format)) {
	ULONG mask = b ? b->RGBFormats : ZZ_SUPPORTED_RGB_FORMATS;

	/* the packed YUV formats are valid PIP overlay SOURCES (never
	 * framebuffer formats; b->RGBFormats stays untouched) */
	if (zz_overlay_hooks_enabled)
		mask |= ZZ_OVERLAY_SOURCE_FORMATS;

	if (format >= 32 || !(mask & (1UL << format)))
		return 0;

	return mask;
}

UWORD SetDisplay(__REGA0(struct BoardInfo *b), __REGD0(UWORD enabled)) {
	UWORD old = b ? (UWORD)b->CardData[ZZ_CARD_DATA_DISPLAY_ENABLED] : 1;

	if (b)
		b->CardData[ZZ_CARD_DATA_DISPLAY_ENABLED] = enabled ? 1 : 0;
	// No firmware display-blanking register is exposed; keep sync and report
	// the previous logical state as required by the P96 callback contract.
	return old;
}

LONG ResolvePixelClock(__REGA0(struct BoardInfo *b), __REGA1(struct ModeInfo *mode_info), __REGD0(ULONG pixel_clock), __REGD7(RGBFTYPE format)) {
	uint16_t w;
	uint16_t h;
	uint16_t scale;

	if (!mode_info || !supported_rgb_format(format))
		return -1;
	sanitize_mode_flags(mode_info);
	if (!adjusted_mode_dimensions(mode_info, &w, &h, &scale))
		return -1;

	mode_info->PixelClock = CLOCK_HZ;
	mode_info->pll1.Clock = 0;
	mode_info->pll2.ClockDivide = 1;

	return 0;
}

ULONG GetPixelClock(__REGA0(struct BoardInfo *b), __REGA1(struct ModeInfo *mode_info), __REGD0(ULONG index), __REGD7(RGBFTYPE format)) {
	uint16_t w;
	uint16_t h;
	uint16_t scale;

	if (index != 0 || !mode_info || !supported_rgb_format(format))
		return 0;
	if (!adjusted_mode_dimensions(mode_info, &w, &h, &scale))
		return 0;

	return CLOCK_HZ;
}

#define VBLANK_WAIT_LIMIT 200000UL

static inline BOOL vblank_state(struct BoardInfo *b) {
	MNTZZ9KRegs *regs = (MNTZZ9KRegs *)b->RegisterBase;
	return regs->vblank_status != 0;
}

void WaitVerticalSync(__REGA0(struct BoardInfo *b), __REGD0(BOOL end)) {
	ULONG guard = VBLANK_WAIT_LIMIT;

	if (!b || !b->RegisterBase)
		return;

	if (end) {
		while (vblank_state(b) && --guard) { }
	} else {
		while (!vblank_state(b) && --guard) { }
	}
}

BOOL GetVSyncState(__REGA0(struct BoardInfo *b), __REGD0(BOOL expected)) {
	if (!b || !b->RegisterBase)
		return expected;

	return vblank_state(b);
}


// Direct horizontal line draw for Z2: bypasses register setup overhead.
static inline void draw_hline_z2(uint8_t *fb, WORD x, WORD y, WORD w,
	ULONG color, UWORD bpr, uint16_t colormode)
{
	uint8_t *row = fb + (UWORD)y * bpr;
	switch (colormode) {
	case MNTVA_COLOR_8BIT:
		memset(row + x, (uint8_t)(color >> 24), w);
		break;
	case MNTVA_COLOR_16BIT565:
	case MNTVA_COLOR_15BIT: {
		uint16_t *p = (uint16_t *)row + x;
		uint16_t c16 = (uint16_t)color;
		WORD n = w;
		while (n--) *p++ = c16;
		break;
	}
	case MNTVA_COLOR_32BIT: {
		uint32_t *p = (uint32_t *)row + x;
		WORD n = w;
		while (n--) *p++ = color;
		break;
	}
	}
}

// Direct vertical line draw for Z2: bypasses register setup overhead.
static inline void draw_vline_z2(uint8_t *fb, WORD x, WORD y, WORD h,
	ULONG color, UWORD bpr, uint16_t colormode)
{
	switch (colormode) {
	case MNTVA_COLOR_8BIT: {
		uint8_t *p = fb + (UWORD)y * bpr + x;
		uint8_t c8 = (uint8_t)(color >> 24);
		while (h >= 4) {
			p[0] = c8; p[bpr] = c8; p[bpr*2] = c8; p[bpr*3] = c8;
			p += bpr * 4; h -= 4;
		}
		while (h--) { *p = c8; p += bpr; }
		break;
	}
	case MNTVA_COLOR_16BIT565:
	case MNTVA_COLOR_15BIT: {
		uint16_t *p = (uint16_t *)(fb + (UWORD)y * bpr) + x;
		uint16_t c16 = (uint16_t)color;
		UWORD step = bpr >> 1;
		while (h >= 4) {
			p[0] = c16; p[step] = c16; p[step*2] = c16; p[step*3] = c16;
			p += step * 4; h -= 4;
		}
		while (h--) { *p = c16; p += step; }
		break;
	}
	case MNTVA_COLOR_32BIT: {
		uint32_t *p = (uint32_t *)(fb + (UWORD)y * bpr) + x;
		UWORD step = bpr >> 2;
		while (h >= 4) {
			p[0] = color; p[step] = color; p[step*2] = color; p[step*3] = color;
			p += step * 4; h -= 4;
		}
		while (h--) { *p = color; p += step; }
		break;
	}
	}
}

static inline void fill_rect_accel(struct BoardInfo *b, struct RenderInfo *r,
	WORD x, WORD y, WORD w, WORD h, ULONG color, UBYTE mask,
	RGBFTYPE format, uint16_t colormode)
{
	MNTZZ9KRegs* registers = (MNTZZ9KRegs *)b->RegisterBase;

	if (b->CardFlags & CARDFLAG_ZORRO_3) {
		dmy_cache
		gfxdata->offset[GFXDATA_DST] = ((uint32_t)r->Memory - (uint32_t)b->MemoryBase);
		gfxdata->pitch[GFXDATA_DST] = (r->BytesPerRow >> 2);

		gfxdata->u8_user[GFXDATA_U8_COLORMODE] = (uint8_t)colormode;
		gfxdata->mask = mask;

		gfxdata->rgb[0] = color;
		gfxdata->x[0] = x;
		gfxdata->x[1] = w;
		gfxdata->y[0] = y;
		gfxdata->y[1] = h;

		zzwrite16(&registers->blitter_dma_op, OP_FILLRECT);
	} else {
		// Don't waste ~11 ZorroII write cycles to draw a very small rectangle.
		// Use min dimension so that thin lines (w=1 or h=1) always use the
		// blitter when their length exceeds the threshold.
		WORD min_dim = (w < h) ? w : h;
		WORD max_dim = (w > h) ? w : h;
		if (min_dim < 6 && max_dim < 32) {
			b->FillRectDefault(b, r, x, y, w, h, color, mask, format);
			return;
		}
		uint32_t offset = ((uint32_t)r->Memory - (uint32_t)b->MemoryBase);

		writeBlitterDstOffset(registers, offset);
		writeBlitterRGB(registers, color);
		writeBlitterDstPitch(registers, r->BytesPerRow >> 2);
		writeBlitterColorMode(registers, colormode);
		writeBlitterX1(registers, x);
		writeBlitterY1(registers, y);
		writeBlitterX2(registers, w);
		writeBlitterY2(registers, h);
		zzwrite16(&registers->blitter_op_fillrect, mask);
	}
}

void FillRect(__REGA0(struct BoardInfo *b), __REGA1(struct RenderInfo *r), __REGD0(WORD x), __REGD1(WORD y), __REGD2(WORD w), __REGD3(WORD h), __REGD4(ULONG color), __REGD5(UBYTE mask), __REGD7(RGBFTYPE format)) {
	if (!b || !r) return;
	if (w<1 || h<1) return;

	uint16_t colormode = mnt_colormode(format);
	if (colormode == MNTVA_COLOR_NO_USE) {
		if (b->FillRectDefault)
			b->FillRectDefault(b, r, x, y, w, h, color, mask, format);
		return;
	}
	mask = direct_color_mask(colormode, mask);

	fill_rect_accel(b, r, x, y, w, h, color, mask, format, colormode);
}

void InvertRect(__REGA0(struct BoardInfo *b), __REGA1(struct RenderInfo *r), __REGD0(WORD x), __REGD1(WORD y), __REGD2(WORD w), __REGD3(WORD h), __REGD4(UBYTE mask), __REGD7(RGBFTYPE format)) {
	if (!b || !r)
		return;

	uint16_t colormode = mnt_colormode(format);
	if (colormode == MNTVA_COLOR_NO_USE) {
		if (b->InvertRectDefault)
			b->InvertRectDefault(b, r, x, y, w, h, mask, format);
		return;
	}
	mask = direct_color_mask(colormode, mask);
	MNTZZ9KRegs* registers = (MNTZZ9KRegs *)b->RegisterBase;

	if (b->CardFlags & CARDFLAG_ZORRO_3) {
		dmy_cache
		gfxdata->offset[GFXDATA_DST] = (uint32_t)r->Memory - (uint32_t)b->MemoryBase;
		gfxdata->pitch[GFXDATA_DST] = (r->BytesPerRow >> 2);

		gfxdata->u8_user[GFXDATA_U8_COLORMODE] = (uint8_t)colormode;
		gfxdata->mask = mask;

		gfxdata->x[0] = x;
		gfxdata->x[1] = w;
		gfxdata->y[0] = y;
		gfxdata->y[1] = h;

		zzwrite16(&registers->blitter_dma_op, OP_INVERTRECT);
	} else {
		// Don't waste ~9 ZorroII write cycles to invert a very small rectangle.
		WORD min_dim = (w < h) ? w : h;
		WORD max_dim = (w > h) ? w : h;
		if (min_dim < 6 && max_dim < 32) {
			b->InvertRectDefault(b, r, x, y, w, h, mask, format);
			return;
		}
		uint32_t offset = ((uint32_t)r->Memory - (uint32_t)b->MemoryBase);

		writeBlitterDstOffset(registers, offset);
		writeBlitterDstPitch(registers, r->BytesPerRow >> 2);
		writeBlitterColorMode(registers, colormode);

		writeBlitterX1(registers, x);
		writeBlitterY1(registers, y);
		writeBlitterX2(registers, w);
		writeBlitterY2(registers, h);

		zzwrite16(&registers->blitter_op_invertrect, mask);
	}
}

// This function shall copy a rectangular image region in the video RAM.
void BlitRect(__REGA0(struct BoardInfo *b), __REGA1(struct RenderInfo *r), __REGD0(WORD x), __REGD1(WORD y), __REGD2(WORD dx), __REGD3(WORD dy), __REGD4(WORD w), __REGD5(WORD h), __REGD6(UBYTE mask), __REGD7(RGBFTYPE format)) {
	if (w<1 || h<1) return;
	if (!b || !r) return;

	uint16_t colormode = mnt_colormode(format);
	if (colormode == MNTVA_COLOR_NO_USE) {
		if (b->BlitRectDefault)
			b->BlitRectDefault(b, r, x, y, dx, dy, w, h, mask, format);
		return;
	}
	MNTZZ9KRegs* registers = (MNTZZ9KRegs *)b->RegisterBase;

	if (b->CardFlags & CARDFLAG_ZORRO_3) {
		dmy_cache

		// RenderInfo describes the video RAM containing the source and target rectangle,
		gfxdata->offset[GFXDATA_DST] = ((uint32_t)r->Memory - (uint32_t)b->MemoryBase);
		gfxdata->offset[GFXDATA_SRC] = ((uint32_t)r->Memory - (uint32_t)b->MemoryBase);
		gfxdata->pitch[GFXDATA_DST] = (r->BytesPerRow >> 2);

		// x/y are the top-left edge of the source rectangle,
		gfxdata->x[2] = x;
		gfxdata->y[2] = y;

		// dx/dy the top-left edge of the destination rectangle,
		gfxdata->x[0] = dx;
		gfxdata->y[0] = dy;

		// and w/h the dimensions of the rectangle to copy.
		gfxdata->x[1] = w;
		gfxdata->y[1] = h;

		// RGBFormat is the format of the source (and destination); this format shall not be taken from the RenderInfo.
		gfxdata->u8_user[GFXDATA_U8_COLORMODE] = (uint8_t)colormode;

		// Mask is a bitmask that defines which (logical) planes are affected by the copy for planar or chunky bitmaps. It can be ignored for direct color modes.
		gfxdata->mask = mask;

		// Source and destination rectangle may be overlapping, a proper copy operation shall be performed in either case.
		zzwrite16(&registers->blitter_dma_op, OP_COPYRECT);
	} else {
		writeBlitterY1(registers, dy);
		writeBlitterY2(registers, h);
		writeBlitterY3(registers, y);

		writeBlitterX1(registers, dx);
		writeBlitterX2(registers, w);
		writeBlitterX3(registers, x);

		writeBlitterDstPitch(registers, r->BytesPerRow >> 2);
		writeBlitterColorMode(registers, colormode | (mask << 8));

		uint32_t offset = ((uint32_t)r->Memory - (uint32_t)b->MemoryBase);
		writeBlitterSrcOffset(registers, offset);
		writeBlitterDstOffset(registers, offset);

		zzwrite16(&registers->blitter_op_copyrect, 1);
	}
}

// This function shall copy one rectangle in video RAM to another rectangle in video RAM using a mode given in d6. A mask is not applied.
void BlitRectNoMaskComplete(__REGA0(struct BoardInfo *b), __REGA1(struct RenderInfo *rs), __REGA2(struct RenderInfo *rt), __REGD0(WORD x), __REGD1(WORD y), __REGD2(WORD dx), __REGD3(WORD dy), __REGD4(WORD w), __REGD5(WORD h), __REGD6(UBYTE minterm), __REGD7(RGBFTYPE format)) {
	if (w<1 || h<1) return;
	if (!b || !rs || !rt) return;

	uint16_t colormode = mnt_colormode(format);
	if (colormode == MNTVA_COLOR_NO_USE) {
		if (b->BlitRectNoMaskCompleteDefault)
			b->BlitRectNoMaskCompleteDefault(b, rs, rt, x, y, dx, dy, w, h, minterm, format);
		return;
	}
	MNTZZ9KRegs* registers = (MNTZZ9KRegs *)b->RegisterBase;

	// b->BlitRectNoMaskCompleteDefault(b, rs, rt, x, y, dx, dy, w, h, minterm, format);
	// return;

	if (b->CardFlags & CARDFLAG_ZORRO_3) {
		dmy_cache

		// The source region in video RAM is given by the source RenderInfo in a1 and a position within it in x and y.
		gfxdata->x[2] = x;
		gfxdata->y[2] = y;
		gfxdata->offset[GFXDATA_SRC] = ((uint32_t)rs->Memory - (uint32_t)b->MemoryBase);
		gfxdata->pitch[GFXDATA_SRC] = (rs->BytesPerRow >> 2);

		// The destination region in video RAM is given by the destinaton RenderInfo in a2 and a position within it in dx and dy.
		gfxdata->x[0] = dx;
		gfxdata->y[0] = dy;
		gfxdata->offset[GFXDATA_DST] = ((uint32_t)rt->Memory - (uint32_t)b->MemoryBase);
		gfxdata->pitch[GFXDATA_DST] = (rt->BytesPerRow >> 2);

		// The dimension of the rectangle to copy is in w and h.
		gfxdata->x[1] = w;
		gfxdata->y[1] = h;

		// The mode is in register d6, it uses the Amiga Blitter MinTerms encoding of the graphics.library.
		gfxdata->minterm = minterm;

		// The common RGBFormat of source and destination is in register d7, it shall not be taken from the source or destination RenderInfo.
		gfxdata->u8_user[GFXDATA_U8_COLORMODE] = (uint8_t)colormode;

		zzwrite16(&registers->blitter_dma_op, OP_COPYRECT_NOMASK);
	} else {
		writeBlitterY1(registers, dy);
		writeBlitterY2(registers, h);
		writeBlitterY3(registers, y);

		writeBlitterX1(registers, dx);
		writeBlitterX2(registers, w);
		writeBlitterX3(registers, x);

		writeBlitterColorMode(registers, colormode | (minterm << 8));

		writeBlitterSrcPitch(registers, rs->BytesPerRow >> 2);
		uint32_t offset = ((uint32_t)rs->Memory - (uint32_t)b->MemoryBase);
		writeBlitterSrcOffset(registers, offset);

		writeBlitterDstPitch(registers, rt->BytesPerRow >> 2);
		offset = ((uint32_t)rt->Memory - (uint32_t)b->MemoryBase);
		writeBlitterDstOffset(registers, offset);

		zzwrite16(&registers->blitter_op_copyrect, 2);
	}
}

void BlitTemplate(__REGA0(struct BoardInfo *b), __REGA1(struct RenderInfo *r), __REGA2(struct Template *t), __REGD0(WORD x), __REGD1(WORD y), __REGD2(WORD w), __REGD3(WORD h), __REGD4(UBYTE mask), __REGD7(RGBFTYPE format)) {
	if (!b || !r) return;
	if (w<1 || h<1) return;
	if (!t) return;

	uint16_t colormode = mnt_colormode(format);
	if (colormode == MNTVA_COLOR_NO_USE) {
		if (b->BlitTemplateDefault)
			b->BlitTemplateDefault(b, r, t, x, y, w, h, mask, format);
		return;
	}
	mask = direct_color_mask(colormode, mask);
	MNTZZ9KRegs* registers = (MNTZZ9KRegs *)b->RegisterBase;

	if (!(b->CardFlags & CARDFLAG_ZORRO_3)) {
		uint32_t offset = ((uint32_t)r->Memory - (uint32_t)b->MemoryBase);
		writeBlitterDstOffset(registers, offset);
	}

	uint32_t zz_template_addr = Z3_TEMPLATE_ADDR;
	if (!(b->CardFlags & CARDFLAG_ZORRO_3)) {
		zz_template_addr = b->MemorySize;
	}

	memcpy((uint8_t*)(((uint32_t)b->MemoryBase)+zz_template_addr), t->Memory, t->BytesPerRow * h);

	if (b->CardFlags & CARDFLAG_ZORRO_3) {
		dmy_cache
		gfxdata->x[0] = x;
		gfxdata->x[1] = w;
		gfxdata->x[2] = t->XOffset;
		gfxdata->y[0] = y;
		gfxdata->y[1] = h;

		gfxdata->offset[GFXDATA_DST] = ((uint32_t)r->Memory - (uint32_t)b->MemoryBase);
		gfxdata->offset[GFXDATA_SRC] = zz_template_addr;
		gfxdata->pitch[GFXDATA_DST] = r->BytesPerRow;
		gfxdata->pitch[GFXDATA_SRC] = t->BytesPerRow;

		gfxdata->rgb[0] = t->FgPen;
		gfxdata->rgb[1] = t->BgPen;

		gfxdata->u8_user[GFXDATA_U8_COLORMODE] = (uint8_t)colormode;
		gfxdata->u8_user[GFXDATA_U8_DRAWMODE] = t->DrawMode;
		gfxdata->mask = mask;

		zzwrite16(&registers->blitter_dma_op, OP_RECT_TEMPLATE);
	} else {
		writeBlitterSrcOffset(registers, zz_template_addr);

		writeBlitterRGB(registers, t->FgPen);
		writeBlitterRGB2(registers, t->BgPen);

		writeBlitterSrcPitch(registers, t->BytesPerRow);
		writeBlitterDstPitch(registers, r->BytesPerRow);
		writeBlitterColorMode(registers, colormode | (t->DrawMode << 8));
		writeBlitterX1(registers, x);
		writeBlitterY1(registers, y);
		writeBlitterX2(registers, w);
		writeBlitterY2(registers, h);
		writeBlitterX3(registers, t->XOffset);

		zzwrite16(&registers->blitter_op_filltemplate, mask);
	}
}

void BlitPattern(__REGA0(struct BoardInfo *b), __REGA1(struct RenderInfo *r), __REGA2(struct Pattern *pat), __REGD0(WORD x), __REGD1(WORD y), __REGD2(WORD w), __REGD3(WORD h), __REGD4(UBYTE mask), __REGD7(RGBFTYPE format)) {
	if (!b || !r) return;
	if (w<1 || h<1) return;
	if (!pat) return;

	uint16_t colormode = mnt_colormode(format);
	if (colormode == MNTVA_COLOR_NO_USE) {
		if (b->BlitPatternDefault)
			b->BlitPatternDefault(b, r, pat, x, y, w, h, mask, format);
		return;
	}
	mask = direct_color_mask(colormode, mask);
	MNTZZ9KRegs* registers = (MNTZZ9KRegs *)b->RegisterBase;

	if (!(b->CardFlags & CARDFLAG_ZORRO_3)) {
		uint32_t offset = ((uint32_t)r->Memory - (uint32_t)b->MemoryBase);
		writeBlitterDstOffset(registers, offset);
	}

	uint32_t zz_template_addr = Z3_TEMPLATE_ADDR;
	if (!(b->CardFlags & CARDFLAG_ZORRO_3)) {
		zz_template_addr = b->MemorySize;
	}

	if (pat->Size > 15) return;
	memcpy((uint8_t*)(((uint32_t)b->MemoryBase) + zz_template_addr), pat->Memory, 2 * (1 << pat->Size));

	if (b->CardFlags & CARDFLAG_ZORRO_3) {
		dmy_cache
		gfxdata->x[0] = x;
		gfxdata->x[1] = w;
		gfxdata->x[2] = pat->XOffset;
		gfxdata->y[0] = y;
		gfxdata->y[1] = h;
		gfxdata->y[2] = pat->YOffset;

		gfxdata->offset[GFXDATA_DST] = ((uint32_t)r->Memory - (uint32_t)b->MemoryBase);
		gfxdata->offset[GFXDATA_SRC] = zz_template_addr;
		gfxdata->pitch[GFXDATA_DST] = r->BytesPerRow;

		gfxdata->rgb[0] = pat->FgPen;
		gfxdata->rgb[1] = pat->BgPen;

		gfxdata->u8_user[GFXDATA_U8_COLORMODE] = (uint8_t)colormode;
		gfxdata->u8_user[GFXDATA_U8_DRAWMODE] = pat->DrawMode;
		gfxdata->user[0] = (1 << pat->Size);
		gfxdata->mask = mask;

		zzwrite16(&registers->blitter_dma_op, OP_RECT_PATTERN);
	} else {
		writeBlitterSrcOffset(registers, zz_template_addr);

		writeBlitterRGB(registers, pat->FgPen);
		writeBlitterRGB2(registers, pat->BgPen);

		writeBlitterUser1(registers, mask);
		writeBlitterDstPitch(registers, r->BytesPerRow);
		writeBlitterColorMode(registers, colormode | (pat->DrawMode << 8));
		writeBlitterX1(registers, x);
		writeBlitterY1(registers, y);
		writeBlitterX2(registers, w);
		writeBlitterY2(registers, h);
		writeBlitterX3(registers, pat->XOffset);
		writeBlitterY3(registers, pat->YOffset);

		zzwrite16(&registers->blitter_op_filltemplate, (1 << pat->Size) | 0x8000);
	}
}

void DrawLine(__REGA0(struct BoardInfo *b), __REGA1(struct RenderInfo *r), __REGA2(struct Line *l), __REGD0(UBYTE mask), __REGD7(RGBFTYPE format)) {
	if (!b || !l || !r)
		return;

	uint16_t colormode = mnt_colormode(format);
	if (colormode == MNTVA_COLOR_NO_USE) {
		if (b->DrawLineDefault)
			b->DrawLineDefault(b, r, l, mask, format);
		return;
	}

	/* P96 may hand us a clipped *segment* of a larger line: X,Y is the segment
	 * start, Length its major-axis pixel extent, dX,dY the FULL line delta, and
	 * Xorigin,Yorigin the entire line's start (see the iComp P96 Driver
	 * Development wiki). Length == 0 means the line is fully clipped away. */
	if (l->Length == 0)
		return;

	mask = direct_color_mask(colormode, mask);
	MNTZZ9KRegs* registers = (MNTZZ9KRegs*)b->RegisterBase;

	if (l->DrawMode == 0 && l->LinePtrn == 0xFFFF && mask == 0xFF && l->dY == 0) {
		UWORD len = line_length(l);
		WORD x = l->X;
		WORD y = l->Y;
		WORD w = (WORD)(len + 1);

		if (l->dX < 0)
			x -= len;

		if (!(b->CardFlags & CARDFLAG_ZORRO_3)) {
			draw_hline_z2(r->Memory, x, y, w, l->FgPen,
				r->BytesPerRow, colormode);
			return;
		}
		fill_rect_accel(b, r, x, y, w, 1, l->FgPen, mask, format, colormode);
		return;
	}

	if (l->DrawMode == 0 && l->LinePtrn == 0xFFFF && mask == 0xFF && l->dX == 0) {
		UWORD len = line_length(l);
		WORD x = l->X;
		WORD y = l->Y;
		WORD h = (WORD)(len + 1);

		if (l->dY < 0)
			y -= len;

		if (!(b->CardFlags & CARDFLAG_ZORRO_3)) {
			draw_vline_z2(r->Memory, x, y, h, l->FgPen,
				r->BytesPerRow, colormode);
			return;
		}
		fill_rect_accel(b, r, x, y, 1, h, l->FgPen, mask, format, colormode);
		return;
	}

	/* Round-half-up Bresenham accumulator at the segment start, computed from
	 * geometry in magnitudes so it is octant-independent. P96/DrawLineDefault
	 * places pixel i at minor offset floor((2*i*S + L) / (2*L)) == round(i*S/L)
	 * with ties up; the firmware reproduces it with e += S; if (e >= L)
	 * { e -= L; minor-step; } seeded at the half-pixel bias L/2. The accumulator
	 * at a segment that starts k major / m minor units from the origin is
	 * e = S*k - L*m + L/2 == (S*k + L/2) mod L (L/2 for an unclipped segment),
	 * so a clipped segment resumes P96's staircase exactly. In [0, L), fits
	 * 16 bits. (Verified on ZZ9000 hardware: truncation, seed 0, was 1px off;
	 * round-half-up matches DrawLineDefault pixel-for-pixel.) */
	int line_dxa = l->dX < 0 ? -l->dX : l->dX;
	int line_dya = l->dY < 0 ? -l->dY : l->dY;
	int line_major = line_dxa >= line_dya;
	int line_l = line_major ? line_dxa : line_dya;
	int line_s = line_major ? line_dya : line_dxa;
	/* Xorigin/Yorigin are UWORD, but P96 encodes a line clipped at the top/left
	 * (its start above/left of the RenderInfo) with a negative origin. Cast
	 * through WORD so it sign-extends to int (e.g. -8 stays -8, not 65528),
	 * keeping line_k/line_m and err_seed correct for such clipped lines. X, Y,
	 * dX and dY are already WORD, so they sign-extend on their own. */
	int line_dmaj = line_major ? ((int)l->X - (int)(WORD)l->Xorigin)
				   : ((int)l->Y - (int)(WORD)l->Yorigin);
	int line_dmin = line_major ? ((int)l->Y - (int)(WORD)l->Yorigin)
				   : ((int)l->X - (int)(WORD)l->Xorigin);
	int line_k = line_dmaj < 0 ? -line_dmaj : line_dmaj;
	int line_m = line_dmin < 0 ? -line_dmin : line_dmin;
	UWORD err_seed = (UWORD)(line_s * line_k - line_l * line_m + line_l / 2);

	/* P96's PatternShift is an LSB-based bit index: DrawLineDefault draws the
	 * segment's first pixel with pattern bit (0x0001 << PatternShift). The
	 * firmware uses an MSB-based index, (0x8000 >> offset), so translate here.
	 * 15 - PatternShift makes (0x8000 >> (15 - shift)) == (0x0001 << shift),
	 * matching DrawLineDefault pixel-for-pixel. (HW-confirmed via the PatDiff
	 * readback: the card pattern was 1px off because P96 sends PatternShift=15
	 * for a fresh line and the firmware mirrored it to bit 0. The wiki's "shifts
	 * the pattern PatternShift bits to the left" is misleading, like its line
	 * rounding text.) */
	UWORD line_pat_off = (UWORD)((15 - (l->PatternShift & 15)) & 15);

	if (b->CardFlags & CARDFLAG_ZORRO_3) {
		dmy_cache
		gfxdata->offset[GFXDATA_DST] = (uint32_t)r->Memory - (uint32_t)b->MemoryBase;
		gfxdata->pitch[GFXDATA_DST] = (r->BytesPerRow >> 2);

		gfxdata->u8_user[GFXDATA_U8_COLORMODE] = (uint8_t)colormode;
		gfxdata->u8_user[GFXDATA_U8_DRAWMODE] = l->DrawMode;
		gfxdata->u8_user[GFXDATA_U8_LINE_PATTERN_OFFSET] = line_pat_off;
		gfxdata->u8_user[GFXDATA_U8_LINE_PADDING] = l->pad;

		gfxdata->rgb[0] = l->FgPen;
		gfxdata->rgb[1] = l->BgPen;

		gfxdata->x[0] = l->X;
		gfxdata->x[1] = l->dX;
		gfxdata->y[0] = l->Y;
		gfxdata->y[1] = l->dY;

		gfxdata->user[0] = l->Length;
		gfxdata->user[1] = l->LinePtrn;
		gfxdata->user[2] = ((line_pat_off << 8) | l->pad);
		gfxdata->user[3] = err_seed;

		gfxdata->mask = mask;

		zzwrite16(&registers->blitter_dma_op, OP_DRAWLINE);
	} else {
		uint32_t offset = ((uint32_t)r->Memory - (uint32_t)b->MemoryBase);
		/* Solid lines (LinePtrn 0xFFFF, full mask, JAM1) dispatch to the
		 * firmware's draw_line_solid, which ignores BgPen and the pattern
		 * shift. Skip those two slow Z2 register writes for solid lines.
		 * X3 (LinePtrn) is still written: the firmware selects the solid
		 * path on rect_x3 == 0xFFFF, and the cache collapses the repeats. */
		int solid = (l->DrawMode == 0 && l->LinePtrn == 0xFFFF && mask == 0xFF);

		writeBlitterDstOffset(registers, offset);
		writeBlitterDstPitch(registers, r->BytesPerRow >> 2);

		writeBlitterColorMode(registers, colormode | (l->DrawMode << 8));

		writeBlitterRGB(registers, l->FgPen);

		if (!solid)
			writeBlitterRGB2(registers, l->BgPen);

		writeBlitterX1(registers, l->X);
		writeBlitterY1(registers, l->Y);
		writeBlitterX2(registers, l->dX);
		writeBlitterY2(registers, l->dY);
		writeBlitterUser1(registers, l->Length);
		writeBlitterUser3(registers, err_seed);
		writeBlitterX3(registers, l->LinePtrn);
		if (!solid)
			writeBlitterY3(registers, line_pat_off | (l->pad << 8));

		zzwrite16(&registers->blitter_op_draw_line, mask);
	}
}

/* Off-screen bitmaps we place in card VRAM. `bm` must stay the first
 * member so the struct BitMap* P96 hands back is also a ZZBitMap*.
 * All live wrappers sit on zz_bitmap_list; the pointer-identity walk
 * decides ours/foreign, with magic + the Planes[0] range check kept as
 * consistency guards. */
struct ZZBitMap {
	struct BitMap bm;
	struct ZZBitMap *next;
	ULONG magic;
	ULONG card_offset;
	ULONG rgbformat;
	UWORD width, height;
};

static struct ZZBitMap *zz_bitmap_list = NULL;

static struct ZZBitMap *zz_bitmap_ours(struct BoardInfo *b, struct BitMap *bm) {
	struct ZZBitMap *zbm;

	if (!bm)
		return NULL;

	/* membership first: never read wrapper fields (they sit past the
	 * end of a plain struct BitMap) before proving bm is ours */
	for (zbm = zz_bitmap_list; zbm; zbm = zbm->next)
		if (&zbm->bm == bm)
			break;
	if (!zbm)
		return NULL;

	if (!zz_offscreen_is_ours(zbm->magic, (uint32_t)bm->Planes[0],
			(uint32_t)b->MemoryBase,
			zz_card_window_size ? (uint32_t)zz_card_window_size :
			(uint32_t)b->MemorySize))
		return NULL;

	return zbm;
}

struct BitMap * ZZ_AllocBitMap(__REGA0(struct BoardInfo *b), __REGD0(ULONG width), __REGD1(ULONG height), __REGA1(struct TagItem *tags)) {
	if (!(b->CardFlags & CARDFLAG_ZORRO_3))
		return NULL;

	MNTZZ9KRegs* registers = (MNTZZ9KRegs*)b->RegisterBase;
	ULONG rgbformat = RGBFB_CLUT;
	ULONG mode_width = 0;
	ULONG alignment = 0;
	BOOL constant_pitch = FALSE;

	struct TagItem *tag = tags;
	while (tag && tag->ti_Tag != TAG_DONE) {
		switch (tag->ti_Tag) {
			case TAG_IGNORE: break;
			case TAG_SKIP: tag += tag->ti_Data; break;
			case TAG_MORE: tag = (struct TagItem *)tag->ti_Data; continue;
			case ABMA_RGBFormat: rgbformat = tag->ti_Data; break;
			case ABMA_NoMemory: return NULL;
			/* a flag, not a pitch: the stride must not depend on the
			 * bitmap width (ABMA_ModeWidth carries the mode's width) */
			case ABMA_ConstantBytesPerRow: constant_pitch = tag->ti_Data != 0; break;
			case ABMA_ModeWidth: mode_width = tag->ti_Data; break;
			case ABMA_Alignment: alignment = tag->ti_Data; break;
			/* requests we cannot honor in card VRAM: CPU-owned
			 * fast-mem bitmaps, explicit system-memory bitmaps,
			 * caller-supplied memory, byte-swapped views -> NULL so
			 * P96 uses system RAM */
			case ABMA_UserPrivate:
			case ABMA_System:
			case ABMA_Memory:
			case ABMA_ConstantByteSwapping:
				if (tag->ti_Data) return NULL;
				break;
			/* ABMA_Clear needs no handling: the firmware zero-fills
			 * every surface it allocates */
			default: break;
		}
		tag++;
	}

	if (!zz_rgbformat_bytes_per_pixel(rgbformat))
		return NULL;

	/* the firmware allocator starts surfaces 256-byte aligned; refuse
	 * alignment requests we cannot promise */
	if (alignment && !zz_offscreen_align_valid(alignment))
		return NULL;
	uint32_t pitch_align = (alignment > ZZ_OFFSCREEN_PITCH_ALIGN) ?
		alignment : ZZ_OFFSCREEN_PITCH_ALIGN;

	/* constant pitch: derive the stride from the display mode's width
	 * so every bitmap of that mode/format shares it; the padding is
	 * deterministic, which keeps it constant */
	ULONG pitch_width = width;
	if (constant_pitch && mode_width > pitch_width)
		pitch_width = mode_width;

	/* pitch computed locally rather than via CalculateBytesPerRow: the
	 * P96-facing callback sizes display modes and stays YUV-blind,
	 * while this path also allocates YUV overlay source bitmaps */
	uint16_t bytesperrow = zz_offscreen_pad_pitch_to(
		zz_offscreen_bytes_per_row(rgbformat, pitch_width),
		pitch_align);
	uint32_t size = (uint32_t)bytesperrow * height;
	if (!size) return NULL;

	dmy_cache
	gfxdata->u8_user[1] = 1;
	gfxdata->offset[1] = size;
	zzwrite16(&registers->blitter_acc_op, ACC_OP_ALLOC_SURFACE);

	uint32_t card_offset = gfxdata->offset[0];
	if (!card_offset) return NULL;

	struct ZZBitMap *zbm = (struct ZZBitMap *)AllocMem(sizeof(struct ZZBitMap), MEMF_PUBLIC | MEMF_CLEAR);
	if (!zbm) {
		gfxdata->offset[0] = card_offset;
		gfxdata->u8_user[0] = 0;
		zzwrite16(&registers->blitter_acc_op, ACC_OP_FREE_SURFACE);
		return NULL;
	}

	zbm->bm.BytesPerRow = bytesperrow;
	zbm->bm.Rows = height;
	zbm->bm.Depth = (UBYTE)zz_rgbformat_depth(rgbformat);
	zbm->bm.Planes[0] = (PLANEPTR)((uint32_t)b->MemoryBase + card_offset);
	zbm->magic = ZZ_OFFSCREEN_MAGIC;
	zbm->card_offset = card_offset;
	zbm->rgbformat = rgbformat;
	zbm->width = width;
	zbm->height = height;
	zbm->next = zz_bitmap_list;
	zz_bitmap_list = zbm;

	return &zbm->bm;
}

BOOL ZZ_FreeBitMap(__REGA0(struct BoardInfo *b), __REGA1(struct BitMap *bm), __REGA2(struct TagItem *tags)) {
	struct ZZBitMap *zbm = zz_bitmap_ours(b, bm);
	if (!zbm) return FALSE;

	struct ZZBitMap **pp = &zz_bitmap_list;
	while (*pp && *pp != zbm)
		pp = &(*pp)->next;
	if (*pp)
		*pp = zbm->next;

	if (b->CardFlags & CARDFLAG_ZORRO_3) {
		MNTZZ9KRegs* registers = (MNTZZ9KRegs*)b->RegisterBase;
		dmy_cache
		gfxdata->offset[0] = zbm->card_offset;
		gfxdata->u8_user[0] = 0;
		zzwrite16(&registers->blitter_acc_op, ACC_OP_FREE_SURFACE);
	}

	FreeMem(zbm, sizeof(struct ZZBitMap));
	return TRUE;
}

ULONG ZZ_GetBitMapAttr(__REGA0(struct BoardInfo *b), __REGA1(struct BitMap *bm), __REGD0(ULONG attr)) {
	struct ZZBitMapAttrs attrs;
	struct ZZBitMap *zbm;

	if (!bm) return 0;

	if ((zbm = zz_bitmap_ours(b, bm))) {
		attrs.memory = (uint32_t)bm->Planes[0];
		attrs.basememory = (uint32_t)b->MemoryBase;
		attrs.bytesperrow = bm->BytesPerRow;
		attrs.bytesperpixel = zz_rgbformat_bytes_per_pixel(zbm->rgbformat);
		attrs.bitsperpixel = zz_rgbformat_storage_bits(zbm->rgbformat);
		attrs.rgbformat = zbm->rgbformat;
		attrs.width = zbm->width;
		attrs.height = zbm->height;
		/* ModeInfo convention: color depth, 15 for RGB555 */
		attrs.depth = zz_rgbformat_depth(zbm->rgbformat);
	} else {
		/* foreign bitmap (system RAM, planar): answer from the plain
		 * struct BitMap fields */
		attrs.memory = (uint32_t)bm->Planes[0];
		attrs.basememory = (uint32_t)b->MemoryBase;
		attrs.bytesperrow = bm->BytesPerRow;
		attrs.bytesperpixel = 1;
		attrs.bitsperpixel = bm->Depth;
		attrs.rgbformat = RGBFB_PLANAR;
		attrs.width = (uint32_t)bm->BytesPerRow * 8;
		attrs.height = bm->Rows;
		attrs.depth = bm->Depth;
	}

	return zz_offscreen_attr(&attrs, attr);
}

/* WriteYUVRect contract probe (Stage A). P96 calls this hook from its
 * software picture-in-picture path (p96PIP with a YUV source), but the
 * TagItem contract - how the source stride and format arrive - is
 * undocumented and P96's source is closed. This probe logs every
 * argument, the full tag list and the head of the source data, then
 * delegates to P96's own converter, so a Sashimi capture from a real
 * PIP session defines the contract the accelerated implementation will
 * code against. Build with -DDEBUG (and -ldebug) to enable the
 * logging; a release build is a pure pass-through. */
int ZZ_WriteYUVRect(__REGA0(struct BoardInfo *b), __REGA1(APTR src), __REGD0(SHORT srcx), __REGD1(SHORT srcy), __REGA2(struct RenderInfo *r), __REGD2(SHORT dstx), __REGD3(SHORT dsty), __REGD4(SHORT w), __REGD5(SHORT h), __REGA3(struct TagItem *tags)) {
	int result;
#ifdef DEBUG
	static ULONG yuv_calls = 0;
	ULONG n = yuv_calls++;
	int do_log = (n < 8) || ((n & 0xFF) == 0);

	if (do_log) {
		KPrintF("ZZ9000: WriteYUVRect #%ld src=%lx sx=%ld sy=%ld dst=%lx bpr=%ld fmt=%ld dx=%ld dy=%ld w=%ld h=%ld tags=%lx\n",
			n, (ULONG)src, (LONG)srcx, (LONG)srcy,
			(ULONG)(r ? (ULONG)r->Memory : 0),
			(LONG)(r ? r->BytesPerRow : 0),
			(LONG)(r ? (LONG)r->RGBFormat : -1),
			(LONG)dstx, (LONG)dsty, (LONG)w, (LONG)h, (ULONG)tags);

		struct TagItem *tag = tags;
		int guard = 32;
		while (tag && tag->ti_Tag != TAG_DONE && guard--) {
			switch (tag->ti_Tag) {
				case TAG_IGNORE: break;
				case TAG_SKIP: tag += tag->ti_Data; break;
				case TAG_MORE: tag = (struct TagItem *)tag->ti_Data; continue;
				default:
					KPrintF("ZZ9000:   tag=%08lx data=%08lx\n",
						tag->ti_Tag, tag->ti_Data);
					break;
			}
			tag++;
		}

		if (src) {
			const UBYTE *p = (const UBYTE *)src;
			KPrintF("ZZ9000:   src[0..7]=%02lx %02lx %02lx %02lx %02lx %02lx %02lx %02lx\n",
				(ULONG)p[0], (ULONG)p[1], (ULONG)p[2], (ULONG)p[3],
				(ULONG)p[4], (ULONG)p[5], (ULONG)p[6], (ULONG)p[7]);
			KPrintF("ZZ9000:   src[8..15]=%02lx %02lx %02lx %02lx %02lx %02lx %02lx %02lx\n",
				(ULONG)p[8], (ULONG)p[9], (ULONG)p[10], (ULONG)p[11],
				(ULONG)p[12], (ULONG)p[13], (ULONG)p[14], (ULONG)p[15]);
		}
	}
#endif

	if (!b->WriteYUVRectDefault)
		return 0;
	result = b->WriteYUVRectDefault(b, src, srcx, srcy, r, dstx, dsty, w, h, tags);

#ifdef DEBUG
	if (do_log)
		KPrintF("ZZ9000:   WriteYUVRectDefault returned %ld\n", (LONG)result);
#endif

	return result;
}


/* --- P96 video window (PIP) Features API -----------------------------
 * Contract modeled on WinUAE's uaegfx overlay (the only public
 * reference): P96 creates the PIP as an SFT_MEMORYWINDOW feature, the
 * driver allocates the YUV source bitmap through its own AllocBitMap
 * and the firmware composites it into the display (OP_VIDEO_OVERLAY +
 * shadow scanout). One overlay at a time. */

static struct ZZOverlayState zz_overlay;
static struct BitMap *zz_overlay_bitmap = NULL;
/* which allocator made it, so the right one frees it */
static BOOL zz_overlay_bitmap_p96 = FALSE;
/* RastPort handed to the app for P96PIP_SourceRPort: wraps the source
 * bitmap; equivalent of InitRastPort defaults, built by hand so the
 * driver needs no graphics.library base */
static struct RastPort zz_overlay_rport;

static void zz_overlay_free_source(struct BoardInfo *b) {
	if (!zz_overlay_bitmap)
		return;
	if (zz_overlay_bitmap_p96 && zz_p96_free_bitmap)
		zz_p96_free_bitmap(b, zz_overlay_bitmap, NULL);
	else
		ZZ_FreeBitMap(b, zz_overlay_bitmap, NULL);
	zz_overlay_bitmap = NULL;
	zz_overlay_bitmap_p96 = FALSE;
}

/* Push the full overlay state (or OFF) to the firmware; returns the
 * firmware status word (0 = OK). */
static ULONG zz_overlay_push(struct BoardInfo *b, int off) {
	MNTZZ9KRegs* registers = (MNTZZ9KRegs*)b->RegisterBase;

	dmy_cache
	if (off) {
		gfxdata->u8_user[GFXDATA_U8_OVERLAY_SUBCMD] = OVERLAY_SUBCMD_OFF;
	} else {
		gfxdata->u8_user[GFXDATA_U8_OVERLAY_SUBCMD] = OVERLAY_SUBCMD_SET;
		gfxdata->u8_user[GFXDATA_U8_YUV_VARIANT] =
			(uint8_t)zz_overlay_variant(zz_overlay.rgbformat);
		gfxdata->offset[1] = (uint32_t)zz_overlay_bitmap->Planes[0] -
			(uint32_t)b->MemoryBase;
		gfxdata->pitch[1] = zz_overlay_bitmap->BytesPerRow;
		gfxdata->x[0] = (UWORD)zz_overlay.dst_x;
		gfxdata->y[0] = (UWORD)zz_overlay.dst_y;
		gfxdata->x[1] = (UWORD)zz_overlay.dst_w;
		gfxdata->y[1] = (UWORD)zz_overlay.dst_h;
		gfxdata->x[2] = zz_overlay.src_w;
		gfxdata->y[2] = zz_overlay.src_h;
		gfxdata->user[0] = (UWORD)((zz_overlay.occlusion ? 1 : 0) |
			(zz_overlay.active ? 2 : 0));
		/* pen convention: written as the raw 68k ULONG, the firmware
		 * reads it unswapped (see overlay_handle_op) */
		gfxdata->u32_user[1] = zz_overlay.color_key;
	}
	zzwrite16(&registers->blitter_dma_op, OP_VIDEO_OVERLAY);

	return gfxdata->u32_user[0];
}

APTR ZZ_CreateFeature(__REGA0(struct BoardInfo *b), __REGD0(ULONG type), __REGA1(struct TagItem *tags)) {
	if (!b || type != SFT_MEMORYWINDOW)
		return NULL;
	if (zz_overlay_bitmap)
		return NULL; /* one overlay at a time (WinUAE parity) */
	if (!(b->CardFlags & CARDFLAG_ZORRO_3) || !b->AllocBitMap)
		return NULL;

	memset(&zz_overlay, 0, sizeof(zz_overlay));

	struct TagItem *tag = tags;
	int guard = 64;
	while (tag && tag->ti_Tag != TAG_DONE && guard--) {
		switch (tag->ti_Tag) {
			case TAG_IGNORE: break;
			case TAG_SKIP: tag += tag->ti_Data; break;
			case TAG_MORE: tag = (struct TagItem *)tag->ti_Data; continue;
			default:
#ifdef DEBUG
				KPrintF("ZZ9000: CreateFeature tag=%08lx data=%08lx\n",
					tag->ti_Tag, tag->ti_Data);
#endif
				zz_overlay_apply_tag(&zz_overlay, tag->ti_Tag, tag->ti_Data);
				break;
		}
		tag++;
	}

	if (!zz_overlay_source_valid(zz_overlay.src_w, zz_overlay.src_h,
			zz_overlay.rgbformat)) {
		KPrintF("ZZ9000: CreateFeature REJECT src %ldx%ld fmt %ld\n",
			(LONG)zz_overlay.src_w, (LONG)zz_overlay.src_h,
			(LONG)zz_overlay.rgbformat);
		return NULL;
	}

	/* Allocate the YUV source with the PicassoIV tag set
	 * (ABMA_Displayable/Visible/Clear - the tags WinUAE's uaegfx
	 * CreateFeature copied from the PicassoIV driver, which calls
	 * bi->AllocBitMap exactly like this). PREFER the saved rtg.library
	 * constructor: it returns a bitmap P96 fully manages, inside the
	 * board window its bookkeeping can attribute to this board. A
	 * bench with the source in our surface heap (outside every board
	 * window) froze the machine at the activating SetFeatureAttrs -
	 * the moment RiVA's first LockVLayer runs over P96's bitmap
	 * bookkeeping. Fall back to our own hook (board-pinned surface
	 * heap) when the library default was not captured. */
	{
		struct TagItem alloc_tags[5];
		alloc_tags[0].ti_Tag = ABMA_RGBFormat;
		alloc_tags[0].ti_Data = zz_overlay.rgbformat;
		alloc_tags[1].ti_Tag = ABMA_Clear;
		alloc_tags[1].ti_Data = 1;
		alloc_tags[2].ti_Tag = ABMA_Displayable;
		alloc_tags[2].ti_Data = 1;
		alloc_tags[3].ti_Tag = ABMA_Visible;
		alloc_tags[3].ti_Data = 1;
		alloc_tags[4].ti_Tag = TAG_DONE;
		alloc_tags[4].ti_Data = 0;

		zz_overlay_bitmap_p96 = FALSE;
		if (zz_p96_alloc_bitmap) {
			zz_overlay_bitmap = zz_p96_alloc_bitmap(b,
				zz_overlay.src_w, zz_overlay.src_h, alloc_tags);
			if (zz_overlay_bitmap) {
				zz_overlay_bitmap_p96 = TRUE;
			} else {
				KPrintF("ZZ9000: CreateFeature P96 AllocBitMap "
					"declined, using ZZ hook\n");
			}
		}
		if (!zz_overlay_bitmap)
			zz_overlay_bitmap = ZZ_AllocBitMap(b, zz_overlay.src_w,
				zz_overlay.src_h, alloc_tags);
	}
	if (!zz_overlay_bitmap) {
		KPrintF("ZZ9000: CreateFeature AllocBitMap FAILED\n");
		return NULL;
	}

	/* the compositor reads the source from card VRAM; ZZ_AllocBitMap
	 * guarantees that, so this is a belt-and-braces diagnostic. The
	 * bound is the autoconfig window, NOT b->MemorySize: the firmware
	 * surface heap sits above the P96 window (see zz_card_window_size),
	 * which a previous MemorySize-based check here misread as a
	 * system-RAM placement. */
	if ((uint32_t)zz_overlay_bitmap->Planes[0] < (uint32_t)b->MemoryBase ||
	    (uint32_t)zz_overlay_bitmap->Planes[0] - (uint32_t)b->MemoryBase >=
	    (uint32_t)zz_card_window_size) {
		KPrintF("ZZ9000: CreateFeature source NOT on board (%08lx)\n",
			(ULONG)zz_overlay_bitmap->Planes[0]);
		zz_overlay_free_source(b);
		return NULL;
	}

	/* enable the firmware master gate (the vblank ISR checks it) */
	{
		MNTZZ9KRegs* registers = (MNTZZ9KRegs*)b->RegisterBase;
		zzwrite16(&registers->blitter_user1, CARD_FEATURE_VIDEO_OVERLAY);
		zzwrite16(&registers->set_feature_status, 1);
	}

	memset(&zz_overlay_rport, 0, sizeof(zz_overlay_rport));
	zz_overlay_rport.BitMap = zz_overlay_bitmap;
	zz_overlay_rport.Mask = 0xFF;
	zz_overlay_rport.FgPen = -1;
	zz_overlay_rport.AOlPen = -1;
	zz_overlay_rport.LinePtrn = 0xFFFF;
	zz_overlay_rport.DrawMode = 1; /* JAM2 */

	zz_overlay.magic = ZZ_OVERLAY_MAGIC;

	{
		ULONG fw_status = zz_overlay_push(b, 0);
		if (fw_status != 0) {
			KPrintF("ZZ9000: CreateFeature fw SET status %ld\n",
				(LONG)fw_status);
			zz_overlay.magic = 0;
			zz_overlay_free_source(b);
			return NULL;
		}
	}

	KPrintF("ZZ9000: CreateFeature OK (%ldx%ld fmt %ld src %08lx)\n",
		(LONG)zz_overlay.src_w, (LONG)zz_overlay.src_h,
		(LONG)zz_overlay.rgbformat, (ULONG)zz_overlay_bitmap->Planes[0]);
	return (APTR)&zz_overlay;
}

static int zz_overlay_cookie_ok(APTR fd) {
	return fd == (APTR)&zz_overlay && zz_overlay.magic == ZZ_OVERLAY_MAGIC &&
		zz_overlay_bitmap != NULL;
}

ULONG ZZ_SetFeatureAttrs(__REGA0(struct BoardInfo *b), __REGA1(APTR fd), __REGD0(ULONG type), __REGA2(struct TagItem *tags)) {
	ULONG count = 0;
	int dirty = 0;

	if (!b || !zz_overlay_cookie_ok(fd))
		return 0;

	struct TagItem *tag = tags;
	int guard = 64;
	while (tag && tag->ti_Tag != TAG_DONE && guard--) {
		switch (tag->ti_Tag) {
			case TAG_IGNORE: break;
			case TAG_SKIP: tag += tag->ti_Data; break;
			case TAG_MORE: tag = (struct TagItem *)tag->ti_Data; continue;
			default:
#ifdef DEBUG
				KPrintF("ZZ9000: SetFeatureAttrs tag=%08lx data=%08lx\n",
					tag->ti_Tag, tag->ti_Data);
#endif
				/* source geometry/format is fixed at CreateFeature:
				 * the backing bitmap cannot grow under a live
				 * feature */
				if (!zz_overlay_tag_create_only(tag->ti_Tag))
					dirty |= zz_overlay_apply_tag(&zz_overlay,
						tag->ti_Tag, tag->ti_Data);
				count++;
				break;
		}
		tag++;
	}

	if (dirty)
		zz_overlay_push(b, 0);

	return count;
}

ULONG ZZ_GetFeatureAttrs(__REGA0(struct BoardInfo *b), __REGA1(APTR fd), __REGD0(ULONG type), __REGA2(struct TagItem *tags)) {
	ULONG count = 0;
	int have = zz_overlay_cookie_ok(fd);

	if (!b)
		return 0;

	struct TagItem *tag = tags;
	int guard = 64;
	while (tag && tag->ti_Tag != TAG_DONE && guard--) {
		switch (tag->ti_Tag) {
			case TAG_IGNORE: break;
			case TAG_SKIP: tag += tag->ti_Data; break;
			case TAG_MORE: tag = (struct TagItem *)tag->ti_Data; continue;
			default: {
				uint32_t out;
				/* the app's p96PIP_GetTags queries arrive here:
				 * unanswered source-object queries crash the app
				 * on an undefined pointer */
				if (have && tag->ti_Tag == ZZ_P96PIP_SOURCEBITMAP) {
					if (tag->ti_Data)
						*(ULONG *)tag->ti_Data = (ULONG)zz_overlay_bitmap;
					count++;
#ifdef DEBUG
					KPrintF("ZZ9000: GetFeatureAttrs PIP bitmap -> %08lx\n",
						(ULONG)zz_overlay_bitmap);
#endif
					break;
				}
				if (have && tag->ti_Tag == ZZ_P96PIP_SOURCERPORT) {
					if (tag->ti_Data)
						*(ULONG *)tag->ti_Data = (ULONG)&zz_overlay_rport;
					count++;
#ifdef DEBUG
					KPrintF("ZZ9000: GetFeatureAttrs PIP rport -> %08lx\n",
						(ULONG)&zz_overlay_rport);
#endif
					break;
				}
				if (zz_overlay_query_tag(&zz_overlay, have,
						(uint32_t)zz_overlay_bitmap,
						tag->ti_Tag, &out)) {
					/* ti_Data is a pointer to the result slot
					 * (WinUAE settag semantics) */
					if (tag->ti_Data)
						*(ULONG *)tag->ti_Data = out;
					count++;
				}
#ifdef DEBUG
				KPrintF("ZZ9000: GetFeatureAttrs tag=%08lx data=%08lx\n",
					tag->ti_Tag, tag->ti_Data);
#endif
				break;
			}
		}
		tag++;
	}

	return count;
}

BOOL ZZ_DeleteFeature(__REGA0(struct BoardInfo *b), __REGA1(APTR fd), __REGD0(ULONG type)) {
	if (!b || type != SFT_MEMORYWINDOW || !zz_overlay_cookie_ok(fd))
		return FALSE;

	zz_overlay_push(b, 1);
	zz_overlay.magic = 0;
	zz_overlay_free_source(b);

	return TRUE;
}

// This function shall blit a planar bitmap anywhere in the 68K address space into a chunky bitmap in video RAM.
// The source bitmap that contains the data to be blitted is in the bm argument.
// If one of its plane pointers is 0x0, the source data of that bitplane shall be considered to consist of all-zeros.
// If one of its plane pointers is 0xffffffff, the data in this bitplane shall be considered to be all ones.
void BlitPlanar2Chunky(__REGA0(struct BoardInfo *b), __REGA1(struct BitMap *bm), __REGA2(struct RenderInfo *r), __REGD0(SHORT x), __REGD1(SHORT y), __REGD2(SHORT dx), __REGD3(SHORT dy), __REGD4(SHORT w), __REGD5(SHORT h), __REGD6(UBYTE minterm), __REGD7(UBYTE mask)) {
	if (!b || !r || !bm)
		return;
	if (w<1 || h<1)
		return;

	// b->BlitPlanar2ChunkyDefault(b, bm, r, x, y, dx, dy, w, h, minterm, mask);
	// return;

	MNTZZ9KRegs* registers = (MNTZZ9KRegs*)b->RegisterBase;
	uint32_t offset = ((uint32_t)r->Memory - (uint32_t)b->MemoryBase);
	uint32_t zz_template_addr = Z3_TEMPLATE_ADDR;
	uint16_t zz_mask = mask;
	uint8_t cur_plane = 0x01;

	uint16_t line_size = planar_line_bytes_padded(w);
	uint32_t output_plane_size = line_size * h;

	if (output_plane_size * bm->Depth > 0xFFFF && (!(b->CardFlags & CARDFLAG_ZORRO_3))) {
		b->BlitPlanar2ChunkyDefault(b, bm, r, x, y, dx, dy, w, h, minterm, mask);
		return;
	}

	if (!(b->CardFlags & CARDFLAG_ZORRO_3)) {
		zz_template_addr = b->MemorySize;
	}

	if (b->CardFlags & CARDFLAG_ZORRO_3) {
		gfxdata->offset[GFXDATA_DST] = (uint32_t)r->Memory - (uint32_t)b->MemoryBase;
		gfxdata->offset[GFXDATA_SRC] = zz_template_addr;
		gfxdata->pitch[GFXDATA_DST] = (r->BytesPerRow >> 2);
		gfxdata->pitch[GFXDATA_SRC] = line_size;

		gfxdata->mask = mask;
		gfxdata->minterm = minterm;
	} else {
		writeBlitterDstOffset(registers, offset);
		writeBlitterDstPitch(registers, r->BytesPerRow >> 2);
		writeBlitterColorMode(registers, mnt_colormode(r->RGBFormat) | (minterm << 8));
		writeBlitterSrcOffset(registers, zz_template_addr);
		writeBlitterSrcPitch(registers, line_size);
	}

	for (int16_t i = 0; i < bm->Depth; i++) {
		uint16_t x_offset = (x >> 3);
		if ((uint32_t)bm->Planes[i] == 0xFFFFFFFF) {
			memset((uint8_t*)(((uint32_t)b->MemoryBase)+zz_template_addr), 0xFF, output_plane_size);
		}
		else if (bm->Planes[i] != NULL) {
			uint8_t* bmp_mem = (uint8_t*)bm->Planes[i] + (y * bm->BytesPerRow) + x_offset;
			uint8_t* zz_dest = (uint8_t*)(((uint32_t)b->MemoryBase)+zz_template_addr);
			for (int16_t y_line = 0; y_line < h; y_line++) {
				memcpy(zz_dest, bmp_mem, line_size);
				zz_dest += line_size;
				bmp_mem += bm->BytesPerRow;
			}
		}
		else {
			zz_mask &= (cur_plane ^ 0xFF);
		}
		cur_plane <<= 1;
		zz_template_addr += output_plane_size;
	}

	if (b->CardFlags & CARDFLAG_ZORRO_3) {
		gfxdata->x[0] = (x & 0x07);
		gfxdata->x[1] = dx;
		gfxdata->x[2] = w;
		gfxdata->y[1] = dy;
		gfxdata->y[2] = h;

		gfxdata->user[0] = zz_mask;
		gfxdata->user[1] = bm->Depth;

		zzwrite16(&registers->blitter_dma_op, OP_P2C);
	} else {
		writeBlitterX1(registers, x & 0x07);
		writeBlitterX2(registers, dx);
		writeBlitterY2(registers, dy);
		writeBlitterX3(registers, w);
		writeBlitterY3(registers, h);

		writeBlitterUser2(registers, zz_mask);

		zzwrite16(&registers->blitter_op_p2c, mask | bm->Depth << 8);
	}
}

void BlitPlanar2Direct(__REGA0(struct BoardInfo *b), __REGA1(struct BitMap *bm), __REGA2(struct RenderInfo *r), __REGA3(struct ColorIndexMapping *clut), __REGD0(SHORT x), __REGD1(SHORT y), __REGD2(SHORT dx), __REGD3(SHORT dy), __REGD4(SHORT w), __REGD5(SHORT h), __REGD6(UBYTE minterm), __REGD7(UBYTE mask)) {
	if (!b || !r || !bm || !clut)
		return;
	if (w<1 || h<1)
		return;

	// b->BlitPlanar2DirectDefault(b, bm, r, clut, x, y, dx, dy, w, h, minterm, mask);
	// return;

	MNTZZ9KRegs* registers = (MNTZZ9KRegs*)b->RegisterBase;
	uint32_t offset = ((uint32_t)r->Memory - (uint32_t)b->MemoryBase);
	uint32_t zz_template_addr = Z3_TEMPLATE_ADDR;
	uint16_t zz_mask = mask;
	uint8_t cur_plane = 0x01;

	uint16_t line_size = planar_line_bytes(x, w);
	uint32_t output_plane_size = line_size * h;
	uint32_t staged_size = (256 << 2) + (output_plane_size * bm->Depth);

	if (staged_size > 0xFFFF && (!(b->CardFlags & CARDFLAG_ZORRO_3))) {
		b->BlitPlanar2DirectDefault(b, bm, r, clut, x, y, dx, dy, w, h, minterm, mask);
		return;
	}

	if (!(b->CardFlags & CARDFLAG_ZORRO_3)) {
		zz_template_addr = b->MemorySize;
	}

	if (b->CardFlags & CARDFLAG_ZORRO_3) {
		gfxdata->offset[GFXDATA_DST] = (uint32_t)r->Memory - (uint32_t)b->MemoryBase;
		gfxdata->offset[GFXDATA_SRC] = zz_template_addr;
		gfxdata->pitch[GFXDATA_DST] = (r->BytesPerRow >> 2);
		gfxdata->pitch[GFXDATA_SRC] = line_size;
		gfxdata->rgb[0] = clut->ColorMask;

		gfxdata->u8_user[GFXDATA_U8_COLORMODE] = (uint8_t)mnt_colormode(r->RGBFormat);
		gfxdata->mask = mask;
		gfxdata->minterm = minterm;
	} else {
		writeBlitterDstOffset(registers, offset);
		writeBlitterDstPitch(registers, r->BytesPerRow >> 2);
		writeBlitterColorMode(registers, mnt_colormode(r->RGBFormat) | (minterm << 8));
		writeBlitterSrcOffset(registers, zz_template_addr);
		writeBlitterSrcPitch(registers, line_size);
		writeBlitterRGB(registers, clut->ColorMask);
	}

	memcpy((uint8_t*)(((uint32_t)b->MemoryBase)+zz_template_addr), clut->Colors, (256 << 2));
	zz_template_addr += (256 << 2);

	for (int16_t i = 0; i < bm->Depth; i++) {
		uint16_t x_offset = (x >> 3);
		if ((uint32_t)bm->Planes[i] == 0xFFFFFFFF) {
			memset((uint8_t*)(((uint32_t)b->MemoryBase)+zz_template_addr), 0xFF, output_plane_size);
		}
		else if (bm->Planes[i] != NULL) {
			uint8_t* bmp_mem = (uint8_t*)bm->Planes[i] + (y * bm->BytesPerRow) + x_offset;
			uint8_t* zz_dest = (uint8_t*)(((uint32_t)b->MemoryBase)+zz_template_addr);
			for (int16_t y_line = 0; y_line < h; y_line++) {
				memcpy(zz_dest, bmp_mem, line_size);
				zz_dest += line_size;
				bmp_mem += bm->BytesPerRow;
			}
		}
		else {
			zz_mask &= (cur_plane ^ 0xFF);
		}
		cur_plane <<= 1;
		zz_template_addr += output_plane_size;
	}

	if(b->CardFlags & CARDFLAG_ZORRO_3) {
		gfxdata->x[0] = (x & 0x07);
		gfxdata->x[1] = dx;
		gfxdata->x[2] = w;
		gfxdata->y[1] = dy;
		gfxdata->y[2] = h;

		gfxdata->user[0] = zz_mask;
		gfxdata->user[1] = bm->Depth;

//		unsigned long mi=minterm;
//		unsigned long ma=mask;
//		unsigned long zm=zz_mask;
//		unsigned long cm=clut->ColorMask;
//		unsigned long cf=r->RGBFormat;
//		KPrintF("minterm = %ld, mask=%ld, zzmask=0x%08lX, colormask=0x%08lX, colorformat=%ld\n", mi, ma, zm, cm, cf);

		zzwrite16(&registers->blitter_dma_op, OP_P2D);
	} else {
		writeBlitterX1(registers, x & 0x07);
		writeBlitterX2(registers, dx);
		writeBlitterY2(registers, dy);
		writeBlitterX3(registers, w);
		writeBlitterY3(registers, h);

		writeBlitterUser2(registers, zz_mask);

		zzwrite16(&registers->blitter_op_p2d, mask | bm->Depth << 8);
	}
}

BOOL SetSprite(__REGA0(struct BoardInfo *b), __REGD0(BOOL what), __REGD7(RGBFTYPE format)) {
	if (!b || !b->RegisterBase)
		return FALSE;

	MNTZZ9KRegs* registers = (MNTZZ9KRegs*)b->RegisterBase;

	/*
	 * Keep the legacy ZZ9000 behavior here: the existing firmware/driver
	 * path expects SetSprite() to assert hardware-sprite visibility even
	 * when P96 passes FALSE during setup or screen transitions.
	 */
	zzwrite16(&registers->sprite_bitmap, 1);
	return TRUE;
}

void SetSpritePosition(__REGA0(struct BoardInfo *b), __REGD0(WORD x), __REGD1(WORD y), __REGD7(RGBFTYPE format)) {
	if (!b || !b->RegisterBase)
		return;

	MNTZZ9KRegs* registers = (MNTZZ9KRegs*)b->RegisterBase;
	// The firmware stores b->XOffset/YOffset through SetPanning/SetSpriteImage
	// and applies the P96 sprite offset adjustment when positioning the sprite.
	b->MouseX = x;
	b->MouseY = y;

	if(b->CardFlags & CARDFLAG_ZORRO_3) {
		dmy_cache
		gfxdata->x[0] = x;
		gfxdata->y[0] = y + b->YSplit;

		zzwrite16(&registers->blitter_dma_op, OP_SPRITE_XY);
	} else {
		writeBlitterX1(registers, x);
		writeBlitterY1(registers, y + b->YSplit);
		zzwrite16(&registers->sprite_y, 1);
	}
}

void SetSpriteImage(__REGA0(struct BoardInfo *b), __REGD7(RGBFTYPE format)) {
	if (!b) return;
	MNTZZ9KRegs* registers = (MNTZZ9KRegs*)b->RegisterBase;
	uint32_t zz_template_addr = Z3_TEMPLATE_ADDR;
	if (!(b->CardFlags & CARDFLAG_ZORRO_3)) {
		zz_template_addr = b->MemorySize;
	}

	uint32_t flags = b->Flags;
	int hiressprite = 1;
	int doubledsprite = 0;
	if (flags & BIF_HIRESSPRITE)
		hiressprite = 2;
	if (flags & BIF_BIGSPRITE)
		doubledsprite = 1;

	uint16_t data_size = ((b->MouseWidth >> 3) * 2 * hiressprite) * (b->MouseHeight);
	memcpy((uint8_t*)(((uint32_t)b->MemoryBase)+zz_template_addr), b->MouseImage + 2 * hiressprite, data_size);

	if(b->CardFlags & CARDFLAG_ZORRO_3) {
		dmy_cache
		gfxdata->offset[1] = zz_template_addr;
		gfxdata->x[0] = b->XOffset;
		gfxdata->x[1] = b->MouseWidth | (doubledsprite << 8);
		gfxdata->y[0] = b->YOffset;
		gfxdata->y[1] = b->MouseHeight | ((hiressprite - 1) << 8);

		zzwrite16(&registers->blitter_dma_op, OP_SPRITE_BITMAP);
	} else {
		writeBlitterSrcOffset(registers, zz_template_addr);

		writeBlitterX1(registers, b->XOffset);
		writeBlitterX2(registers, b->MouseWidth | (doubledsprite << 8));
		writeBlitterY1(registers, b->YOffset);
		writeBlitterY2(registers, b->MouseHeight | ((hiressprite - 1) << 8));

		zzwrite16(&registers->sprite_bitmap, 0);
	}
}

void SetSpriteColor(__REGA0(struct BoardInfo *b), __REGD0(UBYTE idx), __REGD1(UBYTE R), __REGD2(UBYTE G), __REGD3(UBYTE B), __REGD7(RGBFTYPE format)) {
	if (!b) return;
	MNTZZ9KRegs* registers = (MNTZZ9KRegs*)b->RegisterBase;

	if (b->CardFlags & CARDFLAG_ZORRO_3) {
		dmy_cache
		((char *)&gfxdata->rgb[0])[0] = B;
		((char *)&gfxdata->rgb[0])[1] = G;
		((char *)&gfxdata->rgb[0])[2] = R;
		((char *)&gfxdata->rgb[0])[3] = 0x00;
		gfxdata->u8offset = idx + 1;

		zzwrite16(&registers->blitter_dma_op, OP_SPRITE_COLOR);
	} else {
		writeBlitterUser1(registers, R);
		writeBlitterUser2(registers, B | (G << 8));
		zzwrite16(&registers->sprite_colors, idx + 1);
	}
}

void SetSplitPosition(__REGA0(struct BoardInfo *b),__REGD0(SHORT pos)) {
	if (!b)
		return;

	b->YSplit = pos;
	if (!b->RegisterBase)
		return;

	MNTZZ9KRegs* registers = (MNTZZ9KRegs*)b->RegisterBase;
	uint32_t offset = 0;

	if (pos != 0) {
		if (!b->VisibleBitMap || !b->VisibleBitMap->Planes[0]) {
			pos = 0;
			b->YSplit = 0;
		} else {
			offset = ((uint32_t)b->VisibleBitMap->Planes[0]) - ((uint32_t)b->MemoryBase);
		}
	}

	if (b->CardFlags & CARDFLAG_ZORRO_3) {
		dmy_cache
		gfxdata->offset[0] = offset;
		gfxdata->y[0] = pos;
		zzwrite16(&registers->blitter_dma_op, OP_SET_SPLIT_POS);
	} else {
		writeBlitterSrcOffset(registers, offset);
		zzwrite16(&registers->blitter_set_split_pos, pos);
	}
}

static uint32_t device_vectors[] = {
	(uint32_t)OpenLib,
	(uint32_t)CloseLib,
	(uint32_t)ExpungeLib,
	0,
	(uint32_t)FindCard,
	(uint32_t)InitCard,
	-1
};

struct InitTable
{
	ULONG LibBaseSize;
	APTR FunctionTable;
	APTR DataTable;
	APTR InitLibTable;
};

const uint32_t auto_init_tables[4] = {
	sizeof(struct GFXBase),
	(uint32_t)device_vectors,
	0,
	(uint32_t)InitLib,
};
