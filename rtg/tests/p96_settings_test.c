#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SETTINGS_PATH "../../installer/ZZ9000Installer/Devs/Picasso96Settings"

enum {
	DEPTH_8 = 1 << 0,
	DEPTH_16 = 1 << 1,
	DEPTH_32 = 1 << 2,
};

enum {
	MIHD_ACTIVE = 2,
	MIHD_WIDTH = 4,
	MIHD_HEIGHT = 6,
	MIHD_DEPTH = 8,
	MIHD_HOR_TOTAL = 10,
	MIHD_HOR_SYNC_START = 14,
	MIHD_HOR_SYNC_SIZE = 16,
	MIHD_VER_TOTAL = 20,
	MIHD_VER_SYNC_START = 24,
	MIHD_VER_SYNC_SIZE = 26,
	MIHD_MIN_SIZE = 34,
};

struct ModeDepths {
	int saw_resolution;
	unsigned active_mask;
	unsigned timing_mask;
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

static int parse_p96_settings(struct ModeDepths *full_hd, struct ModeDepths *wide_1440)
{
	FILE *fp;
	uint8_t hdr[12];
	uint16_t current_w = 0;
	uint16_t current_h = 0;

	fp = fopen(SETTINGS_PATH, "rb");
	if (!fp) {
		perror(SETTINGS_PATH);
		return 0;
	}

	if (!read_exact(fp, hdr, sizeof(hdr)) ||
	    memcmp(hdr, "FORM", 4) != 0 ||
	    memcmp(hdr + 8, "P96S", 4) != 0) {
		fprintf(stderr, "FAIL Picasso96Settings is not a P96S FORM file\n");
		fclose(fp);
		return 0;
	}

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

		if (memcmp(chunk_hdr, "RSHD", 4) == 0 && size >= 8) {
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

	fclose(fp);
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

int main(void)
{
	struct ModeDepths full_hd = {0, 0, 0};
	struct ModeDepths wide_1440 = {0, 0, 0};
	int ok = 1;

	if (!parse_p96_settings(&full_hd, &wide_1440))
		return 1;

	ok &= expect_depths("Picasso96 1920x1080 test mode", &full_hd,
		DEPTH_8 | DEPTH_16);
	ok &= expect_timed_depths("Picasso96 1920x1080 timings", &full_hd,
		DEPTH_8 | DEPTH_16 | DEPTH_32);
	ok &= expect_depths("Picasso96 2560x1440 baseline", &wide_1440,
		DEPTH_8 | DEPTH_16);
	ok &= expect_timed_depths("Picasso96 2560x1440 timings", &wide_1440,
		DEPTH_8 | DEPTH_16);

	if (!ok)
		return 1;

	printf("all Picasso96 settings tests passed\n");
	return 0;
}
