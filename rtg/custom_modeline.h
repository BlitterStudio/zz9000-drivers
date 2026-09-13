/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef ZZ9000_RTG_CUSTOM_MODELINE_H
#define ZZ9000_RTG_CUSTOM_MODELINE_H

#include <stdint.h>

#include "mode_timing.h"
#include "zz_custom_mode.h"

/* P96 mode flags duplicated from settings.h (GMB_*): the Picasso96
 * headers are only available to the Amiga build, while this helper is
 * also compiled by the host tests. Bit positions are fixed by the P96
 * settings file format; the reference Cirrus driver maps a SET polarity
 * bit to a negative sync (CirrusGD5434.chip.asm "polarity"). */
#define ZZ_P96_GMF_DOUBLECLOCK    (1u << 0)
#define ZZ_P96_GMF_INTERLACE      (1u << 1)
#define ZZ_P96_GMF_DOUBLESCAN     (1u << 2)
#define ZZ_P96_GMF_HPOLARITY      (1u << 3)
#define ZZ_P96_GMF_VPOLARITY      (1u << 4)
#define ZZ_P96_GMF_DOUBLEVERTICAL (1u << 6)

/* Scan behaviour the ZZ9000 scanout cannot reproduce on a custom
 * modeline. The fixed preset path keeps its historical flag strip; the
 * custom path refuses these instead of silently changing the timing. */
#define ZZ_P96_UNSUPPORTED_SCAN_FLAGS \
	(ZZ_P96_GMF_DOUBLECLOCK | ZZ_P96_GMF_INTERLACE | \
	 ZZ_P96_GMF_DOUBLESCAN | ZZ_P96_GMF_DOUBLEVERTICAL)

/* Plain P96 ModeInfo timing view. Sync starts are offsets from the
 * active width/height, totals are absolute (the same convention the
 * packaged Picasso96Settings carry; see rtg/tests/p96_settings_test.c).
 * Keeping raw values instead of struct ModeInfo makes the conversion
 * testable on the host. */
struct zz_p96_mode {
	uint16_t width;
	uint16_t height;
	uint16_t hor_total;
	uint16_t hor_sync_start;
	uint16_t hor_sync_size;
	uint16_t ver_total;
	uint16_t ver_sync_start;
	uint16_t ver_sync_size;
	uint16_t flags;
	uint32_t pixel_clock;
};

enum zz_modeline_action {
	ZZ_MODELINE_REJECT = 0,
	ZZ_MODELINE_PRESET,
	ZZ_MODELINE_CUSTOM,
};

struct zz_modeline_plan {
	uint8_t action;
	uint8_t scale;
	/* ZZ_MODELINE_PRESET: the fixed table row to program. */
	const struct zz_rtg_mode_timing *preset;
	/* ZZ_MODELINE_CUSTOM: validated modeline with a resolved PLL tuple
	 * and the achieved clock P96 must be told about. */
	struct zz_custom_mode custom;
	uint32_t achieved_clock;
};

/* Fixed-preset selection by output dimensions only, exactly like the
 * pre-custom-modeline driver: modes below 640x480 logical size are
 * doubled and scaled 2x inside a larger preset sync frame. Unknown
 * dimensions yield NULL (the caller refuses the mode). */
static inline const struct zz_rtg_mode_timing *zz_p96_preset_for_dimensions(
	const struct zz_p96_mode *p96, uint16_t *scale)
{
	if (p96->width < ZZ_CUSTOM_MIN_WIDTH ||
	    p96->height < ZZ_CUSTOM_MIN_HEIGHT)
		return NULL;

	*scale = p96->width < 640 && p96->height < 480 ? 3 : 0;
	return zz_rtg_mode_timing_for_logical_size(p96->width, p96->height);
}

/* Exact-match variant for firmware with the custom-mode capability:
 * a mode keeps the fixed preset only when its packaged timing (totals,
 * sync offsets and sizes relative to the logical active area, and the
 * pixel clock) is exactly the preset's. Anything else is a genuinely
 * custom modeline and must not be discarded in favour of the preset. */
/* Polarity flags intentionally do not participate: packaged P96 modes
 * historically disagree with the firmware presets' common polarity. */
static inline const struct zz_rtg_mode_timing *zz_p96_matching_preset(
	const struct zz_p96_mode *p96, uint16_t *scale)
{
	const struct zz_rtg_mode_timing *timing =
		zz_p96_preset_for_dimensions(p96, scale);

	if (!timing)
		return NULL;

	if (p96->hor_total != timing->htotal ||
	    p96->hor_sync_start !=
		(uint16_t)(timing->hsync_start - p96->width) ||
	    p96->hor_sync_size !=
		(uint16_t)(timing->hsync_end - timing->hsync_start) ||
	    p96->ver_total != timing->vtotal ||
	    p96->ver_sync_start !=
		(uint16_t)(timing->vsync_start - p96->height) ||
	    p96->ver_sync_size !=
		(uint16_t)(timing->vsync_end - timing->vsync_start) ||
	    p96->pixel_clock != timing->pixel_clock_hz)
		return NULL;

	return timing;
}

/* Convert P96 timing to the firmware custom modeline. Fails (without
 * touching *mode beyond partial staging) on unsupported scan flags,
 * mixed H/V sync polarity (the formatter has a single polarity bit),
 * or geometrically invalid timing. The PLL tuple is left unresolved. */
static inline int zz_p96_to_custom_mode(const struct zz_p96_mode *p96,
	struct zz_custom_mode *mode)
{
	uint32_t hsync_start;
	uint32_t hsync_end;
	uint32_t vsync_start;
	uint32_t vsync_end;
	uint16_t h_negative;
	uint16_t v_negative;

	if (p96->flags & ZZ_P96_UNSUPPORTED_SCAN_FLAGS)
		return 0;

	h_negative = (p96->flags & ZZ_P96_GMF_HPOLARITY) ? 1u : 0u;
	v_negative = (p96->flags & ZZ_P96_GMF_VPOLARITY) ? 1u : 0u;
	if (h_negative != v_negative)
		return 0; /* mixed polarity: one formatter bit serves both */

	hsync_start = (uint32_t)p96->width + p96->hor_sync_start;
	hsync_end = hsync_start + p96->hor_sync_size;
	vsync_start = (uint32_t)p96->height + p96->ver_sync_start;
	vsync_end = vsync_start + p96->ver_sync_size;
	if (hsync_end > 0xffffU || vsync_end > 0xffffU)
		return 0;

	mode->width = p96->width;
	mode->height = p96->height;
	mode->hsync_start = (uint16_t)hsync_start;
	mode->hsync_end = (uint16_t)hsync_end;
	mode->htotal = p96->hor_total;
	mode->vsync_start = (uint16_t)vsync_start;
	mode->vsync_end = (uint16_t)vsync_end;
	mode->vtotal = p96->ver_total;
	mode->polarity = h_negative;
	mode->mul = 0;
	mode->div = 0;
	mode->div2 = 0;

	return zz_custom_geometry_valid(mode);
}

/* Shared decision core. require_complete_timing selects between the
 * SetGC contract (a fully validated modeline) and the clock
 * negotiation contract of ResolvePixelClock/GetPixelClock, which must
 * keep answering while P96 tooling still edits totals and porches. */
static inline int zz_p96_plan_core(const struct zz_p96_mode *p96,
	uint16_t firmware_capabilities, int require_complete_timing,
	struct zz_modeline_plan *plan)
{
	uint16_t scale = 0;

	plan->action = ZZ_MODELINE_REJECT;
	plan->scale = 0;
	plan->preset = NULL;
	plan->achieved_clock = 0;

	if (firmware_capabilities & ZZ_FW_CAP_CUSTOM_MODE) {
		plan->preset = zz_p96_matching_preset(p96, &scale);
		if (plan->preset) {
			plan->scale = (uint8_t)scale;
			plan->action = ZZ_MODELINE_PRESET;
			return 1;
		}
		if (require_complete_timing &&
		    !zz_p96_to_custom_mode(p96, &plan->custom))
			return 0;
		plan->achieved_clock =
			zz_custom_resolve_clock(p96->pixel_clock, &plan->custom);
		if (!plan->achieved_clock)
			return 0;
		plan->action = ZZ_MODELINE_CUSTOM;
		return 1;
	}

	plan->preset = zz_p96_preset_for_dimensions(p96, &scale);
	if (!plan->preset)
		return 0;
	plan->scale = (uint8_t)scale;
	plan->action = ZZ_MODELINE_PRESET;
	return 1;
}

/* The SetGC decision: a complete, validated modeline (custom) or a
 * fixed preset row. Firmware without ZZ_FW_CAP_CUSTOM_MODE keeps the
 * legacy fixed table: selection is by dimensions only and a non-preset
 * mode is refused rather than displayed with a wrong modeline. */
static inline int zz_p96_plan_modeline(const struct zz_p96_mode *p96,
	uint16_t firmware_capabilities, struct zz_modeline_plan *plan)
{
	return zz_p96_plan_core(p96, firmware_capabilities, 1, plan);
}

/* Clock negotiation for ResolvePixelClock and GetPixelClock: reports
 * the single clock the card would program, WITHOUT demanding a
 * complete timing. Only the PLL bounds decide the custom answer;
 * flags and full geometry stay validated at SetGC time. */
static inline int zz_p96_negotiate_pixel_clock(const struct zz_p96_mode *p96,
	uint16_t firmware_capabilities, struct zz_modeline_plan *plan)
{
	return zz_p96_plan_core(p96, firmware_capabilities, 0, plan);
}

/* Bus-agnostic staging of one custom modeline transaction. The write
 * callback receives raw word offsets (the ZZ_CUSTOM_REG_* constants,
 * valid on both Zorro buses); the status callback returns the already
 * extracted commit status word. */
typedef void (*zz_custom_write_fn)(void *context, uint16_t offset,
	uint16_t value);
typedef uint16_t (*zz_custom_status_fn)(void *context);

static inline int zz_custom_stage_and_commit(const struct zz_custom_mode *mode,
	uint8_t colormode, zz_custom_write_fn write, void *context,
	zz_custom_status_fn read_status)
{
	/* SELECT starts a fresh staged transaction and clears any earlier
	 * poisoned one; PARAM/VALUE pairs then carry every required field
	 * before the packed COMMIT (slot | color << 8, no scaling). */
	write(context, ZZ_CUSTOM_REG_SELECT, ZZ_CUSTOM_MODE_SLOT);

	write(context, ZZ_CUSTOM_REG_PARAM, ZZ_CUSTOM_HRES);
	write(context, ZZ_CUSTOM_REG_VALUE, mode->width);
	write(context, ZZ_CUSTOM_REG_PARAM, ZZ_CUSTOM_VRES);
	write(context, ZZ_CUSTOM_REG_VALUE, mode->height);
	write(context, ZZ_CUSTOM_REG_PARAM, ZZ_CUSTOM_HSTART);
	write(context, ZZ_CUSTOM_REG_VALUE, mode->hsync_start);
	write(context, ZZ_CUSTOM_REG_PARAM, ZZ_CUSTOM_HEND);
	write(context, ZZ_CUSTOM_REG_VALUE, mode->hsync_end);
	write(context, ZZ_CUSTOM_REG_PARAM, ZZ_CUSTOM_HTOTAL);
	write(context, ZZ_CUSTOM_REG_VALUE, mode->htotal);
	write(context, ZZ_CUSTOM_REG_PARAM, ZZ_CUSTOM_VSTART);
	write(context, ZZ_CUSTOM_REG_VALUE, mode->vsync_start);
	write(context, ZZ_CUSTOM_REG_PARAM, ZZ_CUSTOM_VEND);
	write(context, ZZ_CUSTOM_REG_VALUE, mode->vsync_end);
	write(context, ZZ_CUSTOM_REG_PARAM, ZZ_CUSTOM_VTOTAL);
	write(context, ZZ_CUSTOM_REG_VALUE, mode->vtotal);
	write(context, ZZ_CUSTOM_REG_PARAM, ZZ_CUSTOM_POLARITY);
	write(context, ZZ_CUSTOM_REG_VALUE, mode->polarity);
	write(context, ZZ_CUSTOM_REG_PARAM, ZZ_CUSTOM_MUL);
	write(context, ZZ_CUSTOM_REG_VALUE, mode->mul);
	write(context, ZZ_CUSTOM_REG_PARAM, ZZ_CUSTOM_DIV);
	write(context, ZZ_CUSTOM_REG_VALUE, mode->div);
	write(context, ZZ_CUSTOM_REG_PARAM, ZZ_CUSTOM_DIV2);
	write(context, ZZ_CUSTOM_REG_VALUE, mode->div2);

	write(context, ZZ_CUSTOM_REG_COMMIT,
		(uint16_t)(ZZ_CUSTOM_MODE_SLOT | ((uint16_t)colormode << 8)));

	return read_status(context) == ZZ_CUSTOM_STATUS_OK;
}

#endif
