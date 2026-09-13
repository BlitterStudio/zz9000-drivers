/* Host tests for the P96 custom modeline path: conversion from P96
 * timing (sync starts as offsets from the active area), preset
 * matching, capability-gated planning, and clock resolution agreement.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "custom_modeline.h"

static int failures;
#define CHECK(expr) do { if (!(expr)) { \
	printf("FAIL:%d: %s\n", __LINE__, #expr); failures++; \
} } while (0)

#define CUSTOM_CAPS ZZ_FW_CAP_CUSTOM_MODE
#define LEGACY_CAPS 0U

/* Settings-file convention used throughout: presets described with
 * sync starts relative to the logical active area. */
static struct zz_p96_mode p96_from_preset(
	const struct zz_rtg_mode_timing *timing, uint16_t logical_w,
	uint16_t logical_h, uint16_t flags)
{
	struct zz_p96_mode p96;

	memset(&p96, 0, sizeof(p96));
	p96.width = logical_w;
	p96.height = logical_h;
	p96.hor_total = timing->htotal;
	p96.hor_sync_start =
		(uint16_t)(timing->hsync_start - logical_w);
	p96.hor_sync_size =
		(uint16_t)(timing->hsync_end - timing->hsync_start);
	p96.ver_total = timing->vtotal;
	p96.ver_sync_start =
		(uint16_t)(timing->vsync_start - logical_h);
	p96.ver_sync_size =
		(uint16_t)(timing->vsync_end - timing->vsync_start);
	p96.pixel_clock = timing->pixel_clock_hz;
	p96.flags = flags;
	return p96;
}

static struct zz_p96_mode mode_960x720(void)
{
	struct zz_p96_mode p96;

	memset(&p96, 0, sizeof(p96));
	p96.width = 960;
	p96.height = 720;
	p96.hor_total = 1180;
	p96.hor_sync_start = 80; /* offsets from the active width */
	p96.hor_sync_size = 40;
	p96.ver_total = 750;
	p96.ver_sync_start = 5;
	p96.ver_sync_size = 5;
	p96.flags = ZZ_P96_GMF_HPOLARITY | ZZ_P96_GMF_VPOLARITY;
	p96.pixel_clock = 53100000U; /* 1180*750*60 */
	return p96;
}

static void test_960x720_plan(void)
{
	struct zz_p96_mode p96 = mode_960x720();
	struct zz_modeline_plan plan;
	struct zz_p96_mode untouched = p96;

	CHECK(zz_p96_plan_modeline(&p96, CUSTOM_CAPS, &plan) == 1);
	CHECK(plan.action == ZZ_MODELINE_CUSTOM);
	CHECK(plan.preset == NULL);
	/* offsets became absolute frame positions */
	CHECK(plan.custom.width == 960);
	CHECK(plan.custom.height == 720);
	CHECK(plan.custom.hsync_start == 1040);
	CHECK(plan.custom.hsync_end == 1080);
	CHECK(plan.custom.htotal == 1180);
	CHECK(plan.custom.vsync_start == 725);
	CHECK(plan.custom.vsync_end == 730);
	CHECK(plan.custom.vtotal == 750);
	CHECK(plan.custom.polarity == 1);
	/* nearest clock within 0.5%, actually produced by the tuple */
	CHECK(plan.achieved_clock != 0);
	CHECK(plan.achieved_clock >= 53100000U - 53100000U / 200U);
	CHECK(plan.achieved_clock <= 53100000U + 53100000U / 200U);
	CHECK(zz_custom_clock_hz(plan.custom.mul, plan.custom.div,
		plan.custom.div2) == plan.achieved_clock);
	CHECK(plan.custom.mul >= 2 && plan.custom.mul <= 64);
	CHECK(plan.custom.div >= 1 && plan.custom.div <= 5);
	CHECK(plan.custom.div2 >= 1 && plan.custom.div2 <= 128);
	CHECK(zz_custom_mode_valid(&plan.custom));
	/* planning never mutates the P96 description */
	CHECK(memcmp(&p96, &untouched, sizeof(p96)) == 0);

	/* GetPixelClock/Resolve/SetGC agreement: the achieved clock is a
	 * fixed point of both the planner and the clock negotiator, so
	 * all three report the same rate. */
	p96.pixel_clock = plan.achieved_clock;
	CHECK(zz_p96_plan_modeline(&p96, CUSTOM_CAPS, &plan) == 1);
	CHECK(plan.action == ZZ_MODELINE_CUSTOM);
	CHECK(plan.achieved_clock == p96.pixel_clock);
	CHECK(zz_p96_negotiate_pixel_clock(&p96, CUSTOM_CAPS, &plan) == 1);
	CHECK(plan.action == ZZ_MODELINE_CUSTOM);
	CHECK(plan.achieved_clock == p96.pixel_clock);

	/* legacy firmware refuses the unknown dimensions outright */
	CHECK(zz_p96_plan_modeline(&p96, LEGACY_CAPS, &plan) == 0);
	CHECK(plan.action == ZZ_MODELINE_REJECT);
	CHECK(zz_p96_negotiate_pixel_clock(&p96, LEGACY_CAPS, &plan) == 0);
}

static void test_negotiate_incomplete_timing(void)
{
	struct zz_p96_mode p96 = mode_960x720();
	struct zz_modeline_plan plan;

	/* mid-edit timing: porches and sync sizes not filled in yet.
	 * The clock callbacks must keep answering so P96 settings
	 * tooling can negotiate a pixel clock for the mode. */
	p96.hor_sync_start = 0;
	p96.hor_sync_size = 0;
	p96.ver_sync_start = 0;
	p96.ver_sync_size = 0;
	p96.pixel_clock = 75000000U;
	CHECK(zz_p96_negotiate_pixel_clock(&p96, CUSTOM_CAPS, &plan) == 1);
	CHECK(plan.action == ZZ_MODELINE_CUSTOM);
	CHECK(plan.achieved_clock == 75000000U);

	/* SetGC still refuses the incomplete modeline */
	CHECK(zz_p96_plan_modeline(&p96, CUSTOM_CAPS, &plan) == 0);

	/* scan flags do not block negotiation (validated at SetGC), but
	 * a clock outside the PLL range does */
	p96 = mode_960x720();
	p96.flags |= ZZ_P96_GMF_DOUBLESCAN;
	CHECK(zz_p96_negotiate_pixel_clock(&p96, CUSTOM_CAPS, &plan) == 1);
	CHECK(plan.achieved_clock != 0);
	p96.pixel_clock = 240000000U;
	CHECK(zz_p96_negotiate_pixel_clock(&p96, CUSTOM_CAPS, &plan) == 0);
}

/* --- preset retention ---------------------------------------------- */

static void test_every_preset_stays_preset(void)
{
	unsigned i;
	unsigned count = sizeof(zz_rtg_mode_timings) /
		sizeof(zz_rtg_mode_timings[0]);

	for (i = 0; i < count; i++) {
		const struct zz_rtg_mode_timing *timing =
			&zz_rtg_mode_timings[i];
		/* packaged settings carry negative sync flags on every
		 * mode; polarity is not part of the numeric match */
		struct zz_p96_mode p96 = p96_from_preset(timing,
			timing->width, timing->height,
			ZZ_P96_GMF_HPOLARITY | ZZ_P96_GMF_VPOLARITY);
		struct zz_modeline_plan plan;

		CHECK(zz_p96_plan_modeline(&p96, CUSTOM_CAPS, &plan) == 1);
		CHECK(plan.action == ZZ_MODELINE_PRESET);
		CHECK(plan.preset == timing);
		CHECK(plan.scale == 0);

		/* dimension-only legacy path returns the same row even
		 * with scrambled timing numbers */
		p96.hor_total = (uint16_t)(timing->htotal + 7);
		p96.hor_sync_start = 1;
		p96.pixel_clock = 123456789U;
		CHECK(zz_p96_plan_modeline(&p96, LEGACY_CAPS, &plan) == 1);
		CHECK(plan.action == ZZ_MODELINE_PRESET);
		CHECK(plan.preset == timing);
	}
}

static void test_lowres_doubling_stays_preset(void)
{
	/* 320x200/320x240/320x256 are doubled into larger presets with
	 * the sync offsets kept relative to the logical active area */
	static const uint16_t widths[] = { 320, 320, 320 };
	static const uint16_t heights[] = { 200, 240, 256 };
	unsigned i;

	for (i = 0; i < 3; i++) {
		const struct zz_rtg_mode_timing *timing =
			zz_rtg_mode_timing_for_output_size(
				(uint16_t)(widths[i] * 2U),
				(uint16_t)(heights[i] * 2U));
		struct zz_p96_mode p96;
		struct zz_modeline_plan plan;

		CHECK(timing != NULL);
		p96 = p96_from_preset(timing, widths[i], heights[i],
			ZZ_P96_GMF_HPOLARITY | ZZ_P96_GMF_VPOLARITY);
		/* 320x256 carries doublescan in the packaged settings */
		if (i == 2)
			p96.flags |= ZZ_P96_GMF_DOUBLESCAN;

		CHECK(zz_p96_plan_modeline(&p96, CUSTOM_CAPS, &plan) == 1);
		CHECK(plan.action == ZZ_MODELINE_PRESET);
		CHECK(plan.preset == timing);
		CHECK(plan.scale == 3);
		CHECK(zz_p96_plan_modeline(&p96, LEGACY_CAPS, &plan) == 1);
		CHECK(plan.preset == timing);
		CHECK(plan.scale == 3);
	}
}

static void test_modified_timing_same_dimensions(void)
{
	const struct zz_rtg_mode_timing *timing =
		zz_rtg_mode_timing_for_output_size(1280, 720);
	struct zz_p96_mode p96 = p96_from_preset(timing, 1280, 720,
		ZZ_P96_GMF_HPOLARITY | ZZ_P96_GMF_VPOLARITY);
	struct zz_modeline_plan plan;
	uint16_t scale = 0;

	/* changed horizontal total: the preset no longer applies and the
	 * request must not be discarded in its favour */
	p96.hor_total = (uint16_t)(timing->htotal + 10);
	p96.pixel_clock = 74700000U;
	CHECK(zz_p96_matching_preset(&p96, &scale) == NULL);
	CHECK(zz_p96_plan_modeline(&p96, CUSTOM_CAPS, &plan) == 1);
	CHECK(plan.action == ZZ_MODELINE_CUSTOM);
	CHECK(plan.custom.htotal == 1660);
	CHECK(plan.achieved_clock >= 74700000U - 74700000U / 200U);
	CHECK(plan.achieved_clock <= 74700000U + 74700000U / 200U);

	/* changed clock only, preset totals: still a custom clock */
	p96 = p96_from_preset(timing, 1280, 720,
		ZZ_P96_GMF_HPOLARITY | ZZ_P96_GMF_VPOLARITY);
	p96.pixel_clock = 74000000U;
	CHECK(zz_p96_plan_modeline(&p96, CUSTOM_CAPS, &plan) == 1);
	CHECK(plan.action == ZZ_MODELINE_CUSTOM);
	CHECK(plan.achieved_clock >= 74000000U - 74000000U / 200U);
	CHECK(plan.achieved_clock <= 74000000U + 74000000U / 200U);

	/* changed polarity only keeps the preset: flags were never part
	 * of the packaged preset identity (they were historically
	 * inaccurate), so existing installs must not flip to custom */
	p96 = p96_from_preset(timing, 1280, 720, 0);
	CHECK(zz_p96_matching_preset(&p96, &scale) == timing);

	/* legacy firmware: a modified timing at preset dimensions keeps
	 * running the fixed preset (historical behavior) */
	p96.hor_total = (uint16_t)(timing->htotal + 10);
	p96.pixel_clock = 74700000U;
	CHECK(zz_p96_plan_modeline(&p96, LEGACY_CAPS, &plan) == 1);
	CHECK(plan.action == ZZ_MODELINE_PRESET);
	CHECK(plan.preset == timing);
}

/* --- invalid timing and flags -------------------------------------- */

static void test_invalid_custom_modes(void)
{
	struct zz_p96_mode base = mode_960x720();
	struct zz_p96_mode p96;
	struct zz_modeline_plan plan;
	struct zz_custom_mode custom;

	/* width must be 8-aligned (64-bit DMA rows) */
	p96 = base; p96.width = 962; p96.hor_sync_start = 78;
	CHECK(zz_p96_plan_modeline(&p96, CUSTOM_CAPS, &plan) == 0);
	CHECK(zz_p96_to_custom_mode(&p96, &custom) == 0);

	/* envelope */
	p96 = base; p96.width = 312; p96.hor_sync_start = 24;
	CHECK(zz_p96_to_custom_mode(&p96, &custom) == 0);
	p96 = base; p96.height = 199; p96.ver_sync_start = 4;
	CHECK(zz_p96_to_custom_mode(&p96, &custom) == 0);

	/* strict nonzero porches and sync widths */
	p96 = base; p96.hor_sync_start = 0;
	CHECK(zz_p96_to_custom_mode(&p96, &custom) == 0);
	p96 = base; p96.hor_sync_size = 0;
	CHECK(zz_p96_to_custom_mode(&p96, &custom) == 0);
	p96 = base; p96.hor_sync_start = 220; p96.hor_sync_size = 40;
	/* 960+220+40 == htotal: zero back porch */
	CHECK(zz_p96_to_custom_mode(&p96, &custom) == 0);
	p96 = base; p96.ver_sync_size = 0;
	CHECK(zz_p96_to_custom_mode(&p96, &custom) == 0);

	/* totals beyond the 12-bit frame counters */
	p96 = base; p96.hor_total = 4096; p96.hor_sync_start = 60;
	CHECK(zz_p96_to_custom_mode(&p96, &custom) == 0);
	p96 = base; p96.ver_total = 5000; p96.ver_sync_start = 60;
	CHECK(zz_p96_to_custom_mode(&p96, &custom) == 0);

	/* scan flags are refused, not silently stripped */
	p96 = base; p96.flags = ZZ_P96_GMF_INTERLACE;
	CHECK(zz_p96_to_custom_mode(&p96, &custom) == 0);
	p96 = base; p96.flags = ZZ_P96_GMF_DOUBLESCAN;
	CHECK(zz_p96_to_custom_mode(&p96, &custom) == 0);
	p96 = base; p96.flags = ZZ_P96_GMF_DOUBLECLOCK;
	CHECK(zz_p96_to_custom_mode(&p96, &custom) == 0);
	p96 = base; p96.flags = ZZ_P96_GMF_DOUBLEVERTICAL;
	CHECK(zz_p96_to_custom_mode(&p96, &custom) == 0);

	/* mixed H/V polarity cannot be honored by one formatter bit */
	p96 = base; p96.flags = ZZ_P96_GMF_HPOLARITY;
	CHECK(zz_p96_to_custom_mode(&p96, &custom) == 0);
	p96 = base; p96.flags = ZZ_P96_GMF_VPOLARITY;
	CHECK(zz_p96_to_custom_mode(&p96, &custom) == 0);

	/* agreed polarity maps onto the single firmware bit */
	p96 = base; p96.flags = 0;
	CHECK(zz_p96_to_custom_mode(&p96, &custom) == 1);
	CHECK(custom.polarity == 0);
	p96.flags = ZZ_P96_GMF_HPOLARITY | ZZ_P96_GMF_VPOLARITY;
	CHECK(zz_p96_to_custom_mode(&p96, &custom) == 1);
	CHECK(custom.polarity == 1);

	/* offset wrap-around must not truncate into a valid frame */
	p96 = base; p96.hor_sync_start = 65535; p96.hor_sync_size = 40;
	CHECK(zz_p96_to_custom_mode(&p96, &custom) == 0);
}

static void test_clock_limits(void)
{
	struct zz_p96_mode p96 = mode_960x720();
	struct zz_modeline_plan plan;

	p96.pixel_clock = 20000000U; /* below the PLL output floor */
	CHECK(zz_p96_plan_modeline(&p96, CUSTOM_CAPS, &plan) == 0);
	p96.pixel_clock = 170000000U; /* above the ceiling */
	CHECK(zz_p96_plan_modeline(&p96, CUSTOM_CAPS, &plan) == 0);

	/* exact tuple: 75 MHz must come back exact, never approximate */
	p96.pixel_clock = 75000000U;
	CHECK(zz_p96_plan_modeline(&p96, CUSTOM_CAPS, &plan) == 1);
	CHECK(plan.achieved_clock == 75000000U);

	/* 1080p's rounded preset clock resolves exactly for custom too */
	p96.pixel_clock = 148571429U;
	CHECK(zz_p96_plan_modeline(&p96, CUSTOM_CAPS, &plan) == 1);
	CHECK(plan.achieved_clock == 148571429U);
}

/* --- legacy rejection keeps a usable plan afterwards ---------------- */

static void test_legacy_reject_then_valid(void)
{
	struct zz_p96_mode bad = mode_960x720();
	struct zz_modeline_plan plan;
	const struct zz_rtg_mode_timing *timing;
	struct zz_p96_mode good;

	/* old firmware, unknown dimensions: refused... */
	CHECK(zz_p96_plan_modeline(&bad, LEGACY_CAPS, &plan) == 0);
	CHECK(plan.action == ZZ_MODELINE_REJECT);
	CHECK(plan.preset == NULL);

	/* ...and the previously accepted preset still plans cleanly,
	 * which is what SetGC/SetPanning restore relies on */
	timing = zz_rtg_mode_timing_for_output_size(1280, 720);
	good = p96_from_preset(timing, 1280, 720,
		ZZ_P96_GMF_HPOLARITY | ZZ_P96_GMF_VPOLARITY);
	CHECK(zz_p96_plan_modeline(&good, LEGACY_CAPS, &plan) == 1);
	CHECK(plan.action == ZZ_MODELINE_PRESET);
	CHECK(plan.preset == timing);
}

int main(void)
{
	test_960x720_plan();
	test_negotiate_incomplete_timing();
	test_every_preset_stays_preset();
	test_lowres_doubling_stays_preset();
	test_modified_timing_same_dimensions();
	test_invalid_custom_modes();
	test_clock_limits();
	test_legacy_reject_then_valid();

	if (failures) {
		printf("custom_modeline_test: %d failure(s)\n", failures);
		return 1;
	}
	printf("custom_modeline_test: all checks passed\n");
	return 0;
}
