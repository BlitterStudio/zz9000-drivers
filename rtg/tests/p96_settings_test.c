#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mode_timing.h"

#define SETTINGS_PATH "../../installer/ZZ9000Installer/Devs/Picasso96Settings"
#define SETTINGS_Z3_PATH "../../installer/ZZ9000Installer/Devs/Picasso96Settings-Z3"

enum {
	DEPTH_8 = 1 << 0,
	DEPTH_16 = 1 << 1,
	DEPTH_32 = 1 << 2,
};

enum {
	MNTR_FLAGS = 44,
	MNTR_MIN_SIZE = 48,
	MNTR_DPMS_STANDBY = 1 << 0,
	MNTR_DPMS_SUSPEND = 1 << 1,
	MNTR_DPMS_ACTIVE_OFF = 1 << 2,
	MNTR_DPMS_ALL = MNTR_DPMS_STANDBY |
		MNTR_DPMS_SUSPEND |
		MNTR_DPMS_ACTIVE_OFF,
};

enum {
	MIHD_ACTIVE = 2,
	MIHD_WIDTH = 4,
	MIHD_HEIGHT = 6,
	MIHD_DEPTH = 8,
	MIHD_HOR_TOTAL = 10,
	MIHD_HOR_BLANK_SIZE = 12,
	MIHD_HOR_SYNC_START = 14,
	MIHD_HOR_SYNC_SIZE = 16,
	MIHD_VER_TOTAL = 20,
	MIHD_VER_BLANK_SIZE = 22,
	MIHD_VER_SYNC_START = 24,
	MIHD_VER_SYNC_SIZE = 26,
	MIHD_PIXEL_CLOCK = 30,
	MIHD_MIN_SIZE = 34,
};

struct ModeDepths {
	int saw_resolution;
	unsigned active_mask;
	unsigned timing_mask;
};

struct MonitorCaps {
	unsigned chunks;
	uint32_t flags;
};

struct TimingCaps {
	unsigned records;
};

static uint16_t be16(const uint8_t *p)
{
	return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

static uint32_t be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) |
		((uint32_t)p[1] << 16) |
		((uint32_t)p[2] << 8) |
		p[3];
}

static int read_exact(FILE *fp, void *buf, size_t len)
{
	return fread(buf, 1, len, fp) == len;
}

static unsigned depth_bit(uint8_t depth)
{
	switch (depth) {
	case 8:
		return DEPTH_8;
	case 16:
		return DEPTH_16;
	case 32:
		return DEPTH_32;
	default:
		return 0;
	}
}

static int skip_pad_byte(FILE *fp, uint32_t size)
{
	if ((size & 1U) == 0)
		return 1;

	return fgetc(fp) != EOF;
}

static int mode_timings_populated(const uint8_t *data)
{
	uint16_t width = be16(data + MIHD_WIDTH);
	uint16_t height = be16(data + MIHD_HEIGHT);
	uint16_t hor_total = be16(data + MIHD_HOR_TOTAL);
	uint16_t hor_sync_start = be16(data + MIHD_HOR_SYNC_START);
	uint16_t hor_sync_size = be16(data + MIHD_HOR_SYNC_SIZE);
	uint16_t ver_total = be16(data + MIHD_VER_TOTAL);
	uint16_t ver_sync_start = be16(data + MIHD_VER_SYNC_START);
	uint16_t ver_sync_size = be16(data + MIHD_VER_SYNC_SIZE);

	return hor_total != width &&
		hor_sync_start != 0 &&
		hor_sync_size != 0 &&
		ver_total != height &&
		ver_sync_start != 0 &&
		ver_sync_size != 0;
}

static int mode_timing_matches_hardware(const char *settings_path,
	const uint8_t *data)
{
	const struct zz_rtg_mode_timing *timing;
	uint16_t width = be16(data + MIHD_WIDTH);
	uint16_t height = be16(data + MIHD_HEIGHT);
	uint16_t expected_hblank;
	uint16_t expected_vblank;
	uint16_t expected_hsync_start;
	uint16_t expected_vsync_start;

	timing = zz_rtg_mode_timing_for_logical_size(width, height);
	if (!timing) {
		fprintf(stderr, "FAIL %s unsupported settings mode %ux%u\n",
			settings_path, width, height);
		return 0;
	}

	expected_hblank = (uint16_t)(timing->htotal - width);
	expected_vblank = (uint16_t)(timing->vtotal - height);
	expected_hsync_start = (uint16_t)(timing->hsync_start - width);
	expected_vsync_start = (uint16_t)(timing->vsync_start - height);

	if (be16(data + MIHD_HOR_TOTAL) != timing->htotal ||
	    be16(data + MIHD_HOR_BLANK_SIZE) != expected_hblank ||
	    be16(data + MIHD_HOR_SYNC_START) != expected_hsync_start ||
	    be16(data + MIHD_HOR_SYNC_SIZE) !=
			(uint16_t)(timing->hsync_end - timing->hsync_start) ||
	    be16(data + MIHD_VER_TOTAL) != timing->vtotal ||
	    be16(data + MIHD_VER_BLANK_SIZE) != expected_vblank ||
	    be16(data + MIHD_VER_SYNC_START) != expected_vsync_start ||
	    be16(data + MIHD_VER_SYNC_SIZE) !=
			(uint16_t)(timing->vsync_end - timing->vsync_start) ||
	    be32(data + MIHD_PIXEL_CLOCK) != timing->pixel_clock_hz) {
		fprintf(stderr,
			"FAIL %s %ux%ux%u timing is not the hardware modeline\n"
			"     got HT=%u HB=%u HS=%u/%u VT=%u VB=%u VS=%u/%u clock=%lu\n"
			"expected HT=%u HB=%u HS=%u/%u VT=%u VB=%u VS=%u/%u clock=%lu\n",
			settings_path, width, height, data[MIHD_DEPTH],
			be16(data + MIHD_HOR_TOTAL),
			be16(data + MIHD_HOR_BLANK_SIZE),
			be16(data + MIHD_HOR_SYNC_START),
			be16(data + MIHD_HOR_SYNC_SIZE),
			be16(data + MIHD_VER_TOTAL),
			be16(data + MIHD_VER_BLANK_SIZE),
			be16(data + MIHD_VER_SYNC_START),
			be16(data + MIHD_VER_SYNC_SIZE),
			(unsigned long)be32(data + MIHD_PIXEL_CLOCK),
			timing->htotal, expected_hblank, expected_hsync_start,
			(uint16_t)(timing->hsync_end - timing->hsync_start),
			timing->vtotal, expected_vblank, expected_vsync_start,
			(uint16_t)(timing->vsync_end - timing->vsync_start),
			(unsigned long)timing->pixel_clock_hz);
		return 0;
	}

	return 1;
}

static int parse_p96_settings(const char *settings_path,
	struct ModeDepths *full_hd, struct ModeDepths *wide_1440,
	struct MonitorCaps *monitor, struct TimingCaps *timings)
{
	FILE *fp;
	uint8_t hdr[12];
	uint32_t form_size;
	uint16_t current_w = 0;
	uint16_t current_h = 0;

	fp = fopen(settings_path, "rb");
	if (!fp) {
		perror(settings_path);
		return 0;
	}

	if (!read_exact(fp, hdr, sizeof(hdr)) ||
	    memcmp(hdr, "FORM", 4) != 0 ||
	    memcmp(hdr + 8, "P96S", 4) != 0) {
		fprintf(stderr, "FAIL Picasso96Settings is not a P96S FORM file\n");
		fclose(fp);
		return 0;
	}
	form_size = be32(hdr + 4);

	for (;;) {
		uint8_t chunk_hdr[8];
		uint8_t *data;
		uint32_t size;

		if (!read_exact(fp, chunk_hdr, sizeof(chunk_hdr))) {
			if (feof(fp))
				break;
			fprintf(stderr, "FAIL short chunk header\n");
			fclose(fp);
			return 0;
		}

		size = be32(chunk_hdr + 4);
		data = malloc(size ? size : 1);
		if (!data) {
			fprintf(stderr, "FAIL out of memory reading chunk\n");
			fclose(fp);
			return 0;
		}
		if (!read_exact(fp, data, size)) {
			fprintf(stderr, "FAIL short chunk payload\n");
			free(data);
			fclose(fp);
			return 0;
		}

		if (memcmp(chunk_hdr, "MNTR", 4) == 0) {
			if (size < MNTR_MIN_SIZE) {
				fprintf(stderr, "FAIL short Picasso96 monitor chunk\n");
				free(data);
				fclose(fp);
				return 0;
			}
			monitor->chunks++;
			monitor->flags = be32(data + MNTR_FLAGS);
		} else if (memcmp(chunk_hdr, "RSHD", 4) == 0 && size >= 8) {
			current_w = be16(data + 4);
			current_h = be16(data + 6);

			if (current_w == 1920 && current_h == 1080)
				full_hd->saw_resolution = 1;
			if (current_w == 2560 && current_h == 1440)
				wide_1440->saw_resolution = 1;
		} else if (memcmp(chunk_hdr, "MIHD", 4) == 0 && size >= MIHD_MIN_SIZE) {
			int active = be16(data + MIHD_ACTIVE) != 0;
			uint16_t mode_w = be16(data + MIHD_WIDTH);
			uint16_t mode_h = be16(data + MIHD_HEIGHT);
			unsigned bit = depth_bit(data[MIHD_DEPTH]);
			int timings_populated = mode_timings_populated(data);

			if (!mode_timing_matches_hardware(settings_path, data)) {
				free(data);
				fclose(fp);
				return 0;
			}
			timings->records++;

			if (mode_w != current_w || mode_h != current_h) {
				fprintf(stderr, "FAIL mode chunk does not match current resolution\n");
				free(data);
				fclose(fp);
				return 0;
			}

			if (mode_w == 1920 && mode_h == 1080) {
				if (active)
					full_hd->active_mask |= bit;
				if (timings_populated)
					full_hd->timing_mask |= bit;
			}
			if (mode_w == 2560 && mode_h == 1440) {
				if (active)
					wide_1440->active_mask |= bit;
				if (timings_populated)
					wide_1440->timing_mask |= bit;
			}
		}

		free(data);
		if (!skip_pad_byte(fp, size)) {
			fprintf(stderr, "FAIL short chunk padding\n");
			fclose(fp);
			return 0;
		}
	}

	if (ftell(fp) != (long)form_size + 8) {
		fprintf(stderr, "FAIL FORM size does not match Picasso96 settings file\n");
		fclose(fp);
		return 0;
	}

	fclose(fp);
	return 1;
}

static int expect_timing_records(const char *name,
	const struct TimingCaps *timings)
{
	if (timings->records != 40) {
		printf("FAIL %-32s records=%u required=40\n",
			name, timings->records);
		return 0;
	}

	printf("ok   %s records=%u\n", name, timings->records);
	return 1;
}

static int expect_depths(const char *name, const struct ModeDepths *actual,
	unsigned required)
{
	if (!actual->saw_resolution) {
		printf("FAIL %-32s resolution missing\n", name);
		return 0;
	}

	if (actual->active_mask != required) {
		printf("FAIL %-32s active=0x%x required=0x%x\n",
			name, actual->active_mask, required);
		return 0;
	}

	printf("ok   %s active=0x%x\n", name, actual->active_mask);
	return 1;
}

static int expect_timed_depths(const char *name, const struct ModeDepths *actual,
	unsigned required)
{
	if ((actual->timing_mask & required) != required) {
		printf("FAIL %-32s timings=0x%x required=0x%x\n",
			name, actual->timing_mask, required);
		return 0;
	}

	printf("ok   %s timings=0x%x\n", name, actual->timing_mask);
	return 1;
}

static int expect_dpms(const char *name, const struct MonitorCaps *monitor)
{
	if (monitor->chunks != 1) {
		printf("FAIL %-32s monitor chunks=%u required=1\n",
			name, monitor->chunks);
		return 0;
	}

	if ((monitor->flags & MNTR_DPMS_ALL) != MNTR_DPMS_ALL) {
		printf("FAIL %-32s DPMS flags=0x%x required=0x%x\n",
			name, monitor->flags, MNTR_DPMS_ALL);
		return 0;
	}

	printf("ok   %s DPMS flags=0x%x\n", name, monitor->flags);
	return 1;
}

int main(void)
{
	struct ModeDepths full_hd = {0, 0, 0};
	struct ModeDepths wide_1440 = {0, 0, 0};
	struct ModeDepths z3_full_hd = {0, 0, 0};
	struct ModeDepths z3_wide_1440 = {0, 0, 0};
	struct MonitorCaps monitor = {0, 0};
	struct MonitorCaps z3_monitor = {0, 0};
	struct TimingCaps timings = {0};
	struct TimingCaps z3_timings = {0};
	int ok = 1;

	if (!parse_p96_settings(SETTINGS_PATH, &full_hd, &wide_1440, &monitor,
		&timings))
		return 1;
	if (!parse_p96_settings(SETTINGS_Z3_PATH, &z3_full_hd, &z3_wide_1440,
		&z3_monitor, &z3_timings))
		return 1;

	ok &= expect_timing_records("Picasso96 shared fixed timings", &timings);
	ok &= expect_timing_records("Picasso96 Z3 fixed timings", &z3_timings);

	ok &= expect_dpms("Picasso96 shared monitor", &monitor);
	ok &= expect_dpms("Picasso96 Z3 monitor", &z3_monitor);

	ok &= expect_depths("Picasso96 shared 1920x1080", &full_hd,
		DEPTH_8 | DEPTH_16);
	ok &= expect_timed_depths("Picasso96 shared 1920x1080 timings", &full_hd,
		DEPTH_8 | DEPTH_16 | DEPTH_32);
	ok &= expect_depths("Picasso96 Z3 1920x1080", &z3_full_hd,
		DEPTH_8 | DEPTH_16 | DEPTH_32);
	ok &= expect_timed_depths("Picasso96 Z3 1920x1080 timings", &z3_full_hd,
		DEPTH_8 | DEPTH_16 | DEPTH_32);
	ok &= expect_depths("Picasso96 2560x1440 baseline", &wide_1440,
		DEPTH_8 | DEPTH_16);
	ok &= expect_timed_depths("Picasso96 2560x1440 timings", &wide_1440,
		DEPTH_8 | DEPTH_16);
	ok &= expect_depths("Picasso96 Z3 2560x1440 baseline", &z3_wide_1440,
		DEPTH_8 | DEPTH_16);
	ok &= expect_timed_depths("Picasso96 Z3 2560x1440 timings", &z3_wide_1440,
		DEPTH_8 | DEPTH_16);

	if (!ok)
		return 1;

	printf("all Picasso96 settings tests passed\n");
	return 0;
}
