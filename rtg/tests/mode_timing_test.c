#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include "mode_timing.h"

struct expected_mode {
	uint16_t logical_width;
	uint16_t logical_height;
	uint16_t output_width;
	uint16_t output_height;
	uint16_t mode_id;
	uint16_t htotal;
	uint16_t vtotal;
	uint32_t pixel_clock_hz;
	double refresh_hz;
};

static const struct expected_mode expected_modes[] = {
	{ 1280,  720, 1280,  720,  0, 1650,  750,  75000000U, 60.60606 },
	{  800,  600,  800,  600,  1, 1056,  628,  40000000U, 60.31654 },
	{  640,  480,  640,  480,  2,  800,  525,  25000000U, 59.52381 },
	{  320,  200,  640,  400, 16,  800,  525,  25000000U, 59.52381 },
	{  320,  240,  640,  480,  2,  800,  525,  25000000U, 59.52381 },
	{ 1024,  768, 1024,  768,  3, 1344,  806,  65000000U, 60.00384 },
	{ 1280, 1024, 1280, 1024,  4, 1688, 1066, 108000000U, 60.01974 },
	{ 1920, 1080, 1920, 1080,  5, 2200, 1125, 150000000U, 60.60606 },
	{  720,  576,  720,  576,  6,  864,  625,  27108434U, 50.20080 },
	{  320,  256,  640,  512,  9, 1056,  628,  40000000U, 60.31654 },
	{  640,  512,  640,  512,  9, 1056,  628,  40000000U, 60.31654 },
	{  720,  480,  720,  480,  8,  800,  525,  25333333U, 60.31746 },
	{ 1600, 1200, 1600, 1200, 10, 2160, 1242, 161538462U, 60.21443 },
	{ 2560, 1440, 2560, 1440, 11, 3328, 1465, 146428571U, 30.03343 },
	{ 1920,  800, 1920,  800, 17, 2528,  828, 125000000U, 59.71764 },
};

static int check_mode(const struct expected_mode *expected)
{
	const struct zz_rtg_mode_timing *mode;
	double refresh;

	mode = zz_rtg_mode_timing_for_logical_size(expected->logical_width,
		expected->logical_height);
	if (!mode) {
		fprintf(stderr, "FAIL no timing for %ux%u\n",
			expected->logical_width, expected->logical_height);
		return 0;
	}

	if (mode->width != expected->output_width ||
	    mode->height != expected->output_height ||
	    mode->mode_id != expected->mode_id ||
	    mode->htotal != expected->htotal ||
	    mode->vtotal != expected->vtotal ||
	    mode->pixel_clock_hz != expected->pixel_clock_hz) {
		fprintf(stderr,
			"FAIL %ux%u got output=%ux%u mode=%u totals=%ux%u clock=%lu\n",
			expected->logical_width, expected->logical_height,
			mode->width, mode->height, mode->mode_id,
			mode->htotal, mode->vtotal,
			(unsigned long)mode->pixel_clock_hz);
		return 0;
	}

	refresh = (double)mode->pixel_clock_hz /
		((double)mode->htotal * mode->vtotal);
	if (fabs(refresh - expected->refresh_hz) > 0.001) {
		fprintf(stderr, "FAIL %ux%u refresh %.5f expected %.5f\n",
			expected->logical_width, expected->logical_height,
			refresh, expected->refresh_hz);
		return 0;
	}

	printf("ok   %ux%u -> %ux%u %.3f kHz %.3f Hz\n",
		expected->logical_width, expected->logical_height,
		mode->width, mode->height,
		(double)mode->pixel_clock_hz / mode->htotal / 1000.0,
		refresh);
	return 1;
}

int main(void)
{
	unsigned i;
	int ok = 1;

	for (i = 0; i < sizeof(expected_modes) / sizeof(expected_modes[0]); i++)
		ok &= check_mode(&expected_modes[i]);

	if (zz_rtg_mode_timing_for_logical_size(1366, 768) != NULL) {
		fprintf(stderr, "FAIL unsupported 1366x768 returned a timing\n");
		ok = 0;
	}

	if (!ok)
		return 1;

	puts("all fixed modeline timing checks passed");
	return 0;
}
