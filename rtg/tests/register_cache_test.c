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

static void expect_descriptor_pair(const char *name, uint16_t first, uint16_t second)
{
	struct {
		uint32_t before;
		volatile uint16_t fields[2];
		uint32_t after;
	} descriptor = { 0x12345678, { 0, 0 }, 0x87654321 };

	blitter_write_descriptor_pair(descriptor.fields, first, second);
	if (descriptor.fields[0] != first || descriptor.fields[1] != second ||
		descriptor.before != 0x12345678 || descriptor.after != 0x87654321) {
		printf("FAIL %s: descriptor pair changed fields or neighbours\n", name);
		failures++;
		return;
	}
	printf("ok   %s\n", name);
}

int main(void)
{
	struct BlitterRegisterCache cache;
	uint16_t user1 = 0;
	uint16_t user2 = 0;
	uint32_t rgb2 = 0;
	int regs_a = 0;
	int regs_b = 0;

	blitter_cache_reset(&cache);

	expect_write("first src pitch writes",
		blitter_cache_src_pitch_write_needed(&cache, &regs_a, 256), 1);
	expect_write("same src pitch still writes",
		blitter_cache_src_pitch_write_needed(&cache, &regs_a, 256), 1);
	expect_write("changed src pitch writes",
		blitter_cache_src_pitch_write_needed(&cache, &regs_a, 320), 1);
	expect_write("different board writes",
		blitter_cache_src_pitch_write_needed(&cache, &regs_b, 320), 1);
	expect_write("same value after board switch writes",
		blitter_cache_src_pitch_write_needed(&cache, &regs_b, 320), 1);
	if (cache.src_pitch != 320) {
		printf("FAIL src pitch cache slot actual=%u expected=320\n",
			(unsigned)cache.src_pitch);
		failures++;
	}

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

	blitter_cache_invalidate(&cache, &regs_a, BLITTER_CACHE_USER1);
	expect_write("invalidated user1 writes",
		blitter_cache_write16_needed(&cache, &regs_a, BLITTER_CACHE_USER1,
			&user1, 12), 1);
	expect_write("user2 survives user1 invalidate",
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

	expect_descriptor_pair("clipped negative X/dX",
		(uint16_t)(int16_t)-8, (uint16_t)(int16_t)-31);
	expect_descriptor_pair("length and line pattern", 0xffff, 0x8001);
	/* A negative signed pad sign-extends in the existing user[2] encoding. */
	expect_descriptor_pair("signed pattern padding",
		(uint16_t)((15 << 8) | -1), 0x1234);

	if (failures) {
		printf("%d register cache test(s) failed\n", failures);
		return 1;
	}

	printf("all register cache tests passed\n");
	return 0;
}
