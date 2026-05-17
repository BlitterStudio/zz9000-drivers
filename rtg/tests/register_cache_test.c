#include <stdint.h>
#include <stdio.h>

#include "blitter_cache.h"

static int failures;

static void expect_write(const char *name, int actual, int expected)
{
	if (actual != expected) {
		printf("FAIL %-32s actual=%d expected=%d\n", name, actual, expected);
		failures++;
		return;
	}

	printf("ok   %s\n", name);
}

int main(void)
{
	struct BlitterRegisterCache cache;
	uint16_t src_pitch_a = 0;
	uint16_t src_pitch_b = 0;
	uint16_t user1 = 0;
	uint16_t user2 = 0;
	uint32_t rgb2 = 0;
	int regs_a;
	int regs_b;

	blitter_cache_reset(&cache);

	expect_write("first src pitch writes",
		blitter_cache_write16_needed(&cache, &regs_a, BLITTER_CACHE_SRC_PITCH,
			&src_pitch_a, 256), 1);
	expect_write("same src pitch skips",
		blitter_cache_write16_needed(&cache, &regs_a, BLITTER_CACHE_SRC_PITCH,
			&src_pitch_a, 256), 0);
	expect_write("changed src pitch writes",
		blitter_cache_write16_needed(&cache, &regs_a, BLITTER_CACHE_SRC_PITCH,
			&src_pitch_a, 320), 1);
	expect_write("different board writes",
		blitter_cache_write16_needed(&cache, &regs_b, BLITTER_CACHE_SRC_PITCH,
			&src_pitch_b, 320), 1);
	expect_write("same value after board switch skips",
		blitter_cache_write16_needed(&cache, &regs_b, BLITTER_CACHE_SRC_PITCH,
			&src_pitch_b, 320), 0);

	blitter_cache_reset(&cache);
	expect_write("first user1 writes",
		blitter_cache_write16_needed(&cache, &regs_a, BLITTER_CACHE_USER1,
			&user1, 12), 1);
	expect_write("same user1 skips",
		blitter_cache_write16_needed(&cache, &regs_a, BLITTER_CACHE_USER1,
			&user1, 12), 0);
	expect_write("first user2 writes",
		blitter_cache_write16_needed(&cache, &regs_a, BLITTER_CACHE_USER2,
			&user2, 0x5a), 1);
	expect_write("same user2 skips",
		blitter_cache_write16_needed(&cache, &regs_a, BLITTER_CACHE_USER2,
			&user2, 0x5a), 0);

	expect_write("first rgb2 writes",
		blitter_cache_write32_needed(&cache, &regs_a, BLITTER_CACHE_RGB2,
			&rgb2, 0x11223344), 1);
	expect_write("same rgb2 skips",
		blitter_cache_write32_needed(&cache, &regs_a, BLITTER_CACHE_RGB2,
			&rgb2, 0x11223344), 0);
	expect_write("changed rgb2 writes",
		blitter_cache_write32_needed(&cache, &regs_a, BLITTER_CACHE_RGB2,
			&rgb2, 0x55667788), 1);

	if (failures) {
		printf("%d register cache test(s) failed\n", failures);
		return 1;
	}

	printf("all register cache tests passed\n");
	return 0;
}
