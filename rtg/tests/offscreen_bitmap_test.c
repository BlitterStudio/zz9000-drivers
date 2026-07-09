/*
 * Host tests for the off-screen bitmap helpers (offscreen_bitmap.h).
 *
 * Build/run: make -C rtg/tests test (or `make rtg-tests` from the root).
 */

#include <stdio.h>
#include <stdint.h>
#include "offscreen_bitmap.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond) do { \
	checks++; \
	if (!(cond)) { \
		failures++; \
		printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
	} \
} while (0)

static void test_format_tables(void)
{
	/* the four chunky formats the card advertises */
	CHECK(zz_rgbformat_bytes_per_pixel(1) == 1);   /* CLUT */
	CHECK(zz_rgbformat_bytes_per_pixel(9) == 4);   /* B8G8R8A8 */
	CHECK(zz_rgbformat_bytes_per_pixel(10) == 2);  /* R5G6B5 */
	CHECK(zz_rgbformat_bytes_per_pixel(11) == 2);  /* R5G5B5 */
	CHECK(zz_rgbformat_depth(1) == 8);
	CHECK(zz_rgbformat_depth(9) == 32);
	CHECK(zz_rgbformat_depth(10) == 16);
	CHECK(zz_rgbformat_depth(11) == 15);   /* color depth: "15-bit" mode */
	CHECK(zz_rgbformat_storage_bits(1) == 8);
	CHECK(zz_rgbformat_storage_bits(9) == 32);
	CHECK(zz_rgbformat_storage_bits(10) == 16);
	CHECK(zz_rgbformat_storage_bits(11) == 16); /* but 16 bits of storage */

	/* planar, unsupported orders, YUV, out of range: all refused */
	CHECK(zz_rgbformat_bytes_per_pixel(0) == 0);   /* RGBFB_NONE/planar */
	CHECK(zz_rgbformat_bytes_per_pixel(2) == 0);   /* 24BPP RGB */
	CHECK(zz_rgbformat_bytes_per_pixel(6) == 0);   /* 32BPP ARGB */
	CHECK(zz_rgbformat_bytes_per_pixel(14) == 0);  /* YUV 4:2:2 */
	CHECK(zz_rgbformat_bytes_per_pixel(21) == 0);
	CHECK(zz_rgbformat_bytes_per_pixel(0xffffffffu) == 0);
	CHECK(zz_rgbformat_depth(0) == 0);
	CHECK(zz_rgbformat_depth(6) == 0);
	CHECK(zz_rgbformat_storage_bits(0) == 0);
}

static void test_pad_pitch(void)
{
	/* the blitter steps pitches in longwords: strides pad up to 4 */
	CHECK(zz_offscreen_pad_pitch(0) == 0);
	CHECK(zz_offscreen_pad_pitch(1) == 4);
	CHECK(zz_offscreen_pad_pitch(3) == 4);
	CHECK(zz_offscreen_pad_pitch(4) == 4);
	CHECK(zz_offscreen_pad_pitch(17) == 20);   /* 17px CLUT bitmap */
	CHECK(zz_offscreen_pad_pitch(34) == 36);   /* 17px hi/truecolor */
	CHECK(zz_offscreen_pad_pitch(5120) == 5120);
}

static void test_alignment(void)
{
	/* honorable ABMA_Alignment values: powers of two up to the
	 * allocator's 256-byte start-address guarantee */
	CHECK(zz_offscreen_align_valid(1));
	CHECK(zz_offscreen_align_valid(4));
	CHECK(zz_offscreen_align_valid(16));
	CHECK(zz_offscreen_align_valid(64));
	CHECK(zz_offscreen_align_valid(256));
	CHECK(!zz_offscreen_align_valid(0));
	CHECK(!zz_offscreen_align_valid(3));
	CHECK(!zz_offscreen_align_valid(24));
	CHECK(!zz_offscreen_align_valid(512));
	CHECK(!zz_offscreen_align_valid(0x80000000u));

	/* pitch padding to a requested row alignment; blitter minimum of
	 * 4 always applies */
	CHECK(zz_offscreen_pad_pitch_to(17, 0) == 20);
	CHECK(zz_offscreen_pad_pitch_to(17, 16) == 32);
	CHECK(zz_offscreen_pad_pitch_to(100, 64) == 128);
	CHECK(zz_offscreen_pad_pitch_to(256, 256) == 256);
	CHECK(zz_offscreen_pad_pitch_to(257, 256) == 512);
}

static void test_is_ours(void)
{
	const uint32_t base = 0x40000000u, size = 0x04000000u; /* 64 MB Z3 */

	CHECK(zz_offscreen_is_ours(ZZ_OFFSCREEN_MAGIC, base, base, size));
	CHECK(zz_offscreen_is_ours(ZZ_OFFSCREEN_MAGIC, base + size - 4, base, size));

	/* wrong magic */
	CHECK(!zz_offscreen_is_ours(0, base, base, size));
	CHECK(!zz_offscreen_is_ours(0xDEADBEEFu, base, base, size));
	/* pixels outside the board window (system-RAM bitmap) */
	CHECK(!zz_offscreen_is_ours(ZZ_OFFSCREEN_MAGIC, base - 4, base, size));
	CHECK(!zz_offscreen_is_ours(ZZ_OFFSCREEN_MAGIC, base + size, base, size));
	CHECK(!zz_offscreen_is_ours(ZZ_OFFSCREEN_MAGIC, 0, base, size));
	/* wrap-around does not sneak in */
	CHECK(!zz_offscreen_is_ours(ZZ_OFFSCREEN_MAGIC, base - 1, base, 0xffffffffu - base + 1));
}

static void test_attr_dispatch(void)
{
	struct ZZBitMapAttrs a = {
		.memory = 0x40100000u,
		.basememory = 0x40000000u,
		.bytesperrow = 1280 * 4,
		.bytesperpixel = 4,
		.bitsperpixel = 32,
		.rgbformat = 9,
		.width = 1280,
		.height = 720,
		.depth = 32,
	};

	CHECK(zz_offscreen_attr(&a, ZZ_GBMA_MEMORY) == 0x40100000u);
	CHECK(zz_offscreen_attr(&a, ZZ_GBMA_BASEMEMORY) == 0x40000000u);
	CHECK(zz_offscreen_attr(&a, ZZ_GBMA_BYTESPERROW) == 5120);
	CHECK(zz_offscreen_attr(&a, ZZ_GBMA_BYTESPERPIXEL) == 4);
	CHECK(zz_offscreen_attr(&a, ZZ_GBMA_BITSPERPIXEL) == 32);
	CHECK(zz_offscreen_attr(&a, ZZ_GBMA_RGBFORMAT) == 9);
	CHECK(zz_offscreen_attr(&a, ZZ_GBMA_WIDTH) == 1280);
	CHECK(zz_offscreen_attr(&a, ZZ_GBMA_HEIGHT) == 720);
	CHECK(zz_offscreen_attr(&a, ZZ_GBMA_DEPTH) == 32);

	/* unknown attrs read 0 (including the gap at TAG_USER+5) */
	CHECK(zz_offscreen_attr(&a, 0x80000005u) == 0);
	CHECK(zz_offscreen_attr(&a, 0x8000000Au) == 0);
	CHECK(zz_offscreen_attr(&a, 0) == 0);
	CHECK(zz_offscreen_attr(&a, 1) == 0);

	/* RGB555: 15-bit color depth in 16 bits of storage */
	struct ZZBitMapAttrs a15 = {
		.memory = 0x40200000u,
		.basememory = 0x40000000u,
		.bytesperrow = 640 * 2,
		.bytesperpixel = 2,
		.bitsperpixel = zz_rgbformat_storage_bits(11),
		.rgbformat = 11,
		.width = 640,
		.height = 480,
		.depth = zz_rgbformat_depth(11),
	};
	CHECK(zz_offscreen_attr(&a15, ZZ_GBMA_DEPTH) == 15);
	CHECK(zz_offscreen_attr(&a15, ZZ_GBMA_BITSPERPIXEL) == 16);
	CHECK(zz_offscreen_attr(&a15, ZZ_GBMA_BYTESPERPIXEL) == 2);
}

static void test_planar_fallback_shape(void)
{
	/* a foreign planar bitmap resolved by the driver's fallback path:
	 * BytesPerRow=80 (640px), Rows=256, Depth=3 */
	struct ZZBitMapAttrs a = {
		.memory = 0x00200000u,   /* chip RAM plane */
		.basememory = 0x40000000u,
		.bytesperrow = 80,
		.bytesperpixel = 1,
		.bitsperpixel = 3,
		.rgbformat = 0,          /* RGBFB_PLANAR */
		.width = 80 * 8,
		.height = 256,
		.depth = 3,
	};

	CHECK(zz_offscreen_attr(&a, ZZ_GBMA_WIDTH) == 640);
	CHECK(zz_offscreen_attr(&a, ZZ_GBMA_DEPTH) == 3);
	CHECK(zz_offscreen_attr(&a, ZZ_GBMA_RGBFORMAT) == 0);
	CHECK(zz_offscreen_attr(&a, ZZ_GBMA_MEMORY) == 0x00200000u);
}

int main(void)
{
	test_format_tables();
	test_pad_pitch();
	test_alignment();
	test_is_ours();
	test_attr_dispatch();
	test_planar_fallback_shape();

	if (failures) {
		printf("%d of %d checks failed\n", failures, checks);
		return 1;
	}

	printf("all %d offscreen bitmap checks passed\n", checks);
	return 0;
}
