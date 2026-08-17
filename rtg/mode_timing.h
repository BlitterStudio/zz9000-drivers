#ifndef ZZ9000_MODE_TIMING_H
#define ZZ9000_MODE_TIMING_H

#include <stddef.h>
#include <stdint.h>

/* Fixed timing actually programmed by firmware for each RTG mode register.
 * The clock values are the rounded result of the 100 MHz input PLL tuple,
 * not the old 100 MHz placeholder reported to Picasso96 for every mode. */
struct zz_rtg_mode_timing {
	uint16_t width;
	uint16_t height;
	uint16_t mode_id;
	uint16_t hsync_start;
	uint16_t hsync_end;
	uint16_t htotal;
	uint16_t vsync_start;
	uint16_t vsync_end;
	uint16_t vtotal;
	uint32_t pixel_clock_hz;
};

static const struct zz_rtg_mode_timing zz_rtg_mode_timings[] = {
	{ 1280,  720,  0, 1390, 1430, 1650,  725,  730,  750,  75000000U },
	{  800,  600,  1,  840,  968, 1056,  601,  605,  628,  40000000U },
	{  640,  480,  2,  656,  752,  800,  490,  492,  525,  25000000U },
	{  640,  400, 16,  656,  752,  800,  490,  492,  525,  25000000U },
	{ 1024,  768,  3, 1048, 1184, 1344,  771,  777,  806,  65000000U },
	{ 1280, 1024,  4, 1328, 1440, 1688, 1025, 1028, 1066, 108000000U },
	{ 1920, 1080,  5, 2008, 2052, 2200, 1084, 1089, 1125, 150000000U },
	{  720,  576,  6,  732,  796,  864,  581,  586,  625,  27108434U },
	{  720,  480,  8,  736,  768,  800,  490,  492,  525,  25333333U },
	{  640,  512,  9,  840,  968, 1056,  601,  605,  628,  40000000U },
	{ 1600, 1200, 10, 1704, 1880, 2160, 1201, 1204, 1242, 161538462U },
	{ 2560, 1440, 11, 2680, 2944, 3328, 1441, 1444, 1465, 146428571U },
	{ 1920,  800, 17, 2024, 2224, 2528,  801,  804,  828, 125000000U },
};

static inline const struct zz_rtg_mode_timing *
zz_rtg_mode_timing_for_output_size(uint16_t width, uint16_t height)
{
	unsigned i;

	for (i = 0; i < sizeof(zz_rtg_mode_timings) /
				 sizeof(zz_rtg_mode_timings[0]); i++) {
		if (zz_rtg_mode_timings[i].width == width &&
		    zz_rtg_mode_timings[i].height == height)
			return &zz_rtg_mode_timings[i];
	}

	return NULL;
}

static inline const struct zz_rtg_mode_timing *
zz_rtg_mode_timing_for_logical_size(uint16_t width, uint16_t height)
{
	if (width < 640 && height < 480) {
		width = (uint16_t)(width * 2U);
		height = (uint16_t)(height * 2U);
	}

	return zz_rtg_mode_timing_for_output_size(width, height);
}

#endif
