/*
 * Host tests for the P96 video window (PIP) feature helpers
 * (overlay_feature.h).
 *
 * Build/run: make -C rtg/tests test (or `make rtg-tests` from the root).
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "overlay_feature.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond) do { \
	checks++; \
	if (!(cond)) { \
		failures++; \
		printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
	} \
} while (0)

static void test_variant_map(void)
{
	CHECK(zz_overlay_variant(14) == 0); /* CGX  -> YUV422_VARIANT_CGX */
	CHECK(zz_overlay_variant(17) == 1); /* 422  -> YUV422_VARIANT_STD */
	CHECK(zz_overlay_variant(18) == 2); /* PC   -> YUV422_VARIANT_PC */

	/* not accepted: RGB/CLUT, ACCUPAK, PA/PAPC (unadvertised), junk */
	CHECK(zz_overlay_variant(0) < 0);
	CHECK(zz_overlay_variant(1) < 0);
	CHECK(zz_overlay_variant(9) < 0);
	CHECK(zz_overlay_variant(15) < 0);
	CHECK(zz_overlay_variant(16) < 0);
	CHECK(zz_overlay_variant(19) < 0);
	CHECK(zz_overlay_variant(20) < 0);
	CHECK(zz_overlay_variant(0xffffffffu) < 0);
}

static void test_format_mask(void)
{
	CHECK(ZZ_OVERLAY_SOURCE_FORMATS & (1UL << 14));
	CHECK(ZZ_OVERLAY_SOURCE_FORMATS & (1UL << 17));
	CHECK(ZZ_OVERLAY_SOURCE_FORMATS & (1UL << 18));
	CHECK(!(ZZ_OVERLAY_SOURCE_FORMATS & (1UL << 19)));
	CHECK(!(ZZ_OVERLAY_SOURCE_FORMATS & (1UL << 15)));
	CHECK(!(ZZ_OVERLAY_SOURCE_FORMATS & (1UL << 9)));
}

static void test_source_validation(void)
{
	CHECK(zz_overlay_source_valid(16, 16, 14));
	CHECK(zz_overlay_source_valid(320, 240, 17));
	CHECK(zz_overlay_source_valid(4096, 4096, 18));

	CHECK(!zz_overlay_source_valid(15, 240, 14));   /* too narrow */
	CHECK(!zz_overlay_source_valid(320, 15, 14));   /* too short */
	CHECK(!zz_overlay_source_valid(4097, 240, 14)); /* too wide */
	CHECK(!zz_overlay_source_valid(320, 4097, 14)); /* too tall */
	CHECK(!zz_overlay_source_valid(320, 240, 9));   /* RGB source */
	CHECK(!zz_overlay_source_valid(0, 0, 14));
}

static void test_apply_tags(void)
{
	struct ZZOverlayState st;
	memset(&st, 0, sizeof(st));

	/* geometry/key/active tags are "dirty" (firmware must see them) */
	CHECK(zz_overlay_apply_tag(&st, ZZ_FA_ACTIVE, 1) == 1);
	CHECK(st.active == 1);
	CHECK(zz_overlay_apply_tag(&st, ZZ_FA_LEFT, (uint32_t)-12) == 1);
	CHECK(st.dst_x == -12);
	CHECK(zz_overlay_apply_tag(&st, ZZ_FA_TOP, 34) == 1);
	CHECK(st.dst_y == 34);
	CHECK(zz_overlay_apply_tag(&st, ZZ_FA_WIDTH, 320) == 1);
	CHECK(st.dst_w == 320);
	CHECK(zz_overlay_apply_tag(&st, ZZ_FA_HEIGHT, 240) == 1);
	CHECK(st.dst_h == 240);
	CHECK(zz_overlay_apply_tag(&st, ZZ_FA_OCCLUSION, 5) == 1);
	CHECK(st.occlusion == 1);
	CHECK(zz_overlay_apply_tag(&st, ZZ_FA_COLOR, 0xF81F) == 1);
	CHECK(st.color_key == 0xF81F);
	CHECK(zz_overlay_apply_tag(&st, ZZ_FA_FORMAT, 14) == 1);
	CHECK(st.rgbformat == 14);
	CHECK(zz_overlay_apply_tag(&st, ZZ_FA_SOURCEWIDTH, 640) == 1);
	CHECK(st.src_w == 640);
	CHECK(zz_overlay_apply_tag(&st, ZZ_FA_SOURCEHEIGHT, 360) == 1);
	CHECK(st.src_h == 360);

	/* clip + brightness are stored but NOT dirty (WinUAE parity:
	 * accepted and echoed, never applied) */
	CHECK(zz_overlay_apply_tag(&st, ZZ_FA_CLIPLEFT, 3) == 0);
	CHECK(st.clip_l == 3);
	CHECK(zz_overlay_apply_tag(&st, ZZ_FA_CLIPTOP, 4) == 0);
	CHECK(zz_overlay_apply_tag(&st, ZZ_FA_CLIPWIDTH, 5) == 0);
	CHECK(zz_overlay_apply_tag(&st, ZZ_FA_CLIPHEIGHT, 6) == 0);
	CHECK(zz_overlay_apply_tag(&st, ZZ_FA_BRIGHTNESS, 0x1234) == 0);
	CHECK(st.brightness == 0x1234);

	/* unknown tags: ignored, not dirty */
	CHECK(zz_overlay_apply_tag(&st, 0x80000030u, 7) == 0);
	CHECK(zz_overlay_apply_tag(&st, 0, 7) == 0);
}

static void test_query_tags(void)
{
	struct ZZOverlayState st;
	uint32_t out = 0xDEADBEEF;
	memset(&st, 0, sizeof(st));
	st.active = 1;
	st.dst_x = -5;
	st.dst_y = 10;
	st.dst_w = 300;
	st.dst_h = 200;
	st.src_w = 640;
	st.src_h = 360;
	st.rgbformat = 14;
	st.color_key = 0xAA55AA55;
	st.occlusion = 1;
	st.brightness = 9;
	st.clip_l = 1;

	/* limits answered even without a live feature */
	CHECK(zz_overlay_query_tag(&st, 0, 0, ZZ_FA_MINWIDTH, &out) && out == 16);
	CHECK(zz_overlay_query_tag(&st, 0, 0, ZZ_FA_MAXHEIGHT, &out) && out == 4096);
	/* state queries need a live feature */
	CHECK(!zz_overlay_query_tag(&st, 0, 0, ZZ_FA_ACTIVE, &out));

	CHECK(zz_overlay_query_tag(&st, 1, 0x40001234, ZZ_FA_ACTIVE, &out) && out == 1);
	CHECK(zz_overlay_query_tag(&st, 1, 0x40001234, ZZ_FA_LEFT, &out) &&
		(int32_t)out == -5);
	CHECK(zz_overlay_query_tag(&st, 1, 0x40001234, ZZ_FA_WIDTH, &out) && out == 300);
	CHECK(zz_overlay_query_tag(&st, 1, 0x40001234, ZZ_FA_SOURCEWIDTH, &out) && out == 640);
	CHECK(zz_overlay_query_tag(&st, 1, 0x40001234, ZZ_FA_FORMAT, &out) && out == 14);
	CHECK(zz_overlay_query_tag(&st, 1, 0x40001234, ZZ_FA_COLOR, &out) && out == 0xAA55AA55);
	CHECK(zz_overlay_query_tag(&st, 1, 0x40001234, ZZ_FA_BITMAP, &out) && out == 0x40001234);
	CHECK(zz_overlay_query_tag(&st, 1, 0x40001234, ZZ_FA_BRIGHTNESS, &out) && out == 9);
	CHECK(zz_overlay_query_tag(&st, 1, 0x40001234, ZZ_FA_CLIPLEFT, &out) && out == 1);

	/* unknown queries unanswered */
	CHECK(!zz_overlay_query_tag(&st, 1, 0, 0x80000030u, &out));
	CHECK(!zz_overlay_query_tag(&st, 1, 0, 0, &out));
}

int main(void)
{
	test_variant_map();
	test_format_mask();
	test_source_validation();
	test_apply_tags();
	test_query_tags();

	if (failures) {
		printf("%d of %d checks failed\n", failures, checks);
		return 1;
	}

	printf("all %d overlay feature checks passed\n", checks);
	return 0;
}
