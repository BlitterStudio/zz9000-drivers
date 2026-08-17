/* Rewrite the timing fields in the committed Picasso96Settings profiles from
 * the fixed modelines ZZ9000.card actually selects. Build and run from the
 * repository root, for example:
 *
 *   cc -O2 -Wall -Wextra -Irtg -o /tmp/update-p96-settings \
 *      tools/update-p96-settings.c
 *   /tmp/update-p96-settings \
 *      installer/ZZ9000Installer/Devs/Picasso96Settings \
 *      installer/ZZ9000Installer/Devs/Picasso96Settings-Z3
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mode_timing.h"

enum {
	MIHD_WIDTH = 4,
	MIHD_HEIGHT = 6,
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
	EXPECTED_MODE_RECORDS = 40,
};

static uint16_t get_be16(const uint8_t *p)
{
	return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

static uint32_t get_be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) |
		((uint32_t)p[1] << 16) |
		((uint32_t)p[2] << 8) |
		p[3];
}

static void put_be16(uint8_t *p, uint16_t value)
{
	p[0] = (uint8_t)(value >> 8);
	p[1] = (uint8_t)value;
}

static void put_be32(uint8_t *p, uint32_t value)
{
	p[0] = (uint8_t)(value >> 24);
	p[1] = (uint8_t)(value >> 16);
	p[2] = (uint8_t)(value >> 8);
	p[3] = (uint8_t)value;
}

static int rewrite_mode(const char *path, uint8_t *data)
{
	const struct zz_rtg_mode_timing *timing;
	uint16_t width = get_be16(data + MIHD_WIDTH);
	uint16_t height = get_be16(data + MIHD_HEIGHT);

	timing = zz_rtg_mode_timing_for_logical_size(width, height);
	if (!timing) {
		fprintf(stderr, "%s: unsupported mode %ux%u\n", path, width, height);
		return 0;
	}
	if (timing->hsync_start < width || timing->vsync_start < height) {
		fprintf(stderr, "%s: invalid physical sync origin for %ux%u\n",
			path, width, height);
		return 0;
	}

	put_be16(data + MIHD_HOR_TOTAL, timing->htotal);
	put_be16(data + MIHD_HOR_BLANK_SIZE,
		(uint16_t)(timing->htotal - width));
	put_be16(data + MIHD_HOR_SYNC_START,
		(uint16_t)(timing->hsync_start - width));
	put_be16(data + MIHD_HOR_SYNC_SIZE,
		(uint16_t)(timing->hsync_end - timing->hsync_start));
	put_be16(data + MIHD_VER_TOTAL, timing->vtotal);
	put_be16(data + MIHD_VER_BLANK_SIZE,
		(uint16_t)(timing->vtotal - height));
	put_be16(data + MIHD_VER_SYNC_START,
		(uint16_t)(timing->vsync_start - height));
	put_be16(data + MIHD_VER_SYNC_SIZE,
		(uint16_t)(timing->vsync_end - timing->vsync_start));
	put_be32(data + MIHD_PIXEL_CLOCK, timing->pixel_clock_hz);
	return 1;
}

static int update_file(const char *path)
{
	FILE *fp;
	uint8_t *data;
	char *tmp_path;
	long length;
	size_t pos;
	unsigned records = 0;
	int ok = 0;

	fp = fopen(path, "rb");
	if (!fp) {
		perror(path);
		return 0;
	}
	if (fseek(fp, 0, SEEK_END) != 0 || (length = ftell(fp)) < 12 ||
	    fseek(fp, 0, SEEK_SET) != 0) {
		fprintf(stderr, "%s: cannot determine file size\n", path);
		fclose(fp);
		return 0;
	}
	data = malloc((size_t)length);
	if (!data) {
		fprintf(stderr, "%s: out of memory\n", path);
		fclose(fp);
		return 0;
	}
	if (fread(data, 1, (size_t)length, fp) != (size_t)length) {
		fprintf(stderr, "%s: short read\n", path);
		fclose(fp);
		free(data);
		return 0;
	}
	fclose(fp);

	if (memcmp(data, "FORM", 4) != 0 || memcmp(data + 8, "P96S", 4) != 0 ||
	    get_be32(data + 4) + 8U != (uint32_t)length) {
		fprintf(stderr, "%s: invalid P96S FORM\n", path);
		goto done;
	}

	for (pos = 12; pos + 8 <= (size_t)length;) {
		uint32_t chunk_size = get_be32(data + pos + 4);
		size_t payload = pos + 8;
		size_t next = payload + chunk_size + (chunk_size & 1U);

		if (next > (size_t)length) {
			fprintf(stderr, "%s: truncated chunk\n", path);
			goto done;
		}
		if (memcmp(data + pos, "MIHD", 4) == 0) {
			if (chunk_size < MIHD_MIN_SIZE ||
			    !rewrite_mode(path, data + payload))
				goto done;
			records++;
		}
		pos = next;
	}

	if (pos != (size_t)length || records != EXPECTED_MODE_RECORDS) {
		fprintf(stderr, "%s: found %u mode records, expected %u\n",
			path, records, EXPECTED_MODE_RECORDS);
		goto done;
	}

	tmp_path = malloc(strlen(path) + 5);
	if (!tmp_path) {
		fprintf(stderr, "%s: out of memory for temporary path\n", path);
		goto done;
	}
	sprintf(tmp_path, "%s.tmp", path);
	fp = fopen(tmp_path, "wb");
	if (!fp) {
		fprintf(stderr, "%s: cannot write replacement\n", tmp_path);
		remove(tmp_path);
		free(tmp_path);
		goto done;
	}
	if (fwrite(data, 1, (size_t)length, fp) != (size_t)length) {
		fprintf(stderr, "%s: cannot write replacement\n", tmp_path);
		fclose(fp);
		remove(tmp_path);
		free(tmp_path);
		goto done;
	}
	if (fclose(fp) != 0) {
		fprintf(stderr, "%s: cannot close replacement\n", tmp_path);
		remove(tmp_path);
		free(tmp_path);
		goto done;
	}
	if (rename(tmp_path, path) != 0) {
		perror("rename");
		remove(tmp_path);
		free(tmp_path);
		goto done;
	}
	free(tmp_path);
	printf("updated %s (%u mode records)\n", path, records);
	ok = 1;

done:
	free(data);
	return ok;
}

int main(int argc, char **argv)
{
	int i;
	int ok = 1;

	if (argc < 2) {
		fprintf(stderr, "usage: %s Picasso96Settings [...]\n", argv[0]);
		return 2;
	}
	for (i = 1; i < argc; i++)
		ok &= update_file(argv[i]);

	return ok ? 0 : 1;
}
