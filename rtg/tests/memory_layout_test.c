#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../memory_layout.h"

static int failures;
static int checks;

#define CHECK(cond) do { \
	checks++; \
	if (!(cond)) { \
		failures++; \
		printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
	} \
} while (0)

static void check_profile(uint32_t info, uint32_t size,
	uint32_t fb_size, uint32_t pip_base, uint32_t pip_size,
	uint32_t template_base, uint32_t host_base, uint32_t host_size)
{
	struct ZZApertureLayout layout;

	CHECK(zz_z2_aperture_negotiate(info, size,
		ZZ_FW_CAP_Z2_APERTURE_LAYOUT, &layout) == ZZ_APERTURE_VALID);
	CHECK(layout.descriptor == info);
	CHECK(layout.aperture_size == size);
	CHECK(layout.framebuffer.base == 0x10000);
	CHECK(layout.framebuffer.size == fb_size);
	CHECK(layout.pip.base == pip_base);
	CHECK(layout.pip.size == pip_size);
	CHECK(layout.template_scratch.base == template_base);
	CHECK(layout.template_scratch.size == 0x10000);
	CHECK(layout.host_window.base == host_base);
	CHECK(layout.host_window.size == host_size);
	CHECK(layout.audio.base == size - 0x10000);
	CHECK(layout.audio.size == 0x10000);
	CHECK(zz_aperture_layout_valid(&layout));
	CHECK(zz_aperture_memory_offset(layout.template_scratch.base) ==
		template_base - 0x10000);
}

static void test_profiles(void)
{
	check_profile(ZZ_Z2_APERTURE_INFO_2M, 0x200000, 0x1c0000,
		0, 0, 0x1d0000, 0x1e0000, 0x4000);
	check_profile(ZZ_Z2_APERTURE_INFO_4M, 0x400000, 0x388000,
		0x398000, 0x38000, 0x3d0000, 0x3e0000, 0x4000);
	check_profile(ZZ_Z2_APERTURE_INFO_8M, 0x800000, 0x770000,
		0x780000, 0x40000, 0x7c0000, 0x7d0000, 0x14000);
}

static void test_compatibility_matrix(void)
{
	struct ZZApertureLayout layout;

	memset(&layout, 0xa5, sizeof(layout));
	CHECK(zz_z2_aperture_negotiate(ZZ_Z2_APERTURE_INFO_4M, 0x400000,
		0, &layout) == ZZ_APERTURE_LEGACY); /* new FPGA, old firmware */
	CHECK(zz_z2_aperture_negotiate(0, 0x400000,
		ZZ_FW_CAP_Z2_APERTURE_LAYOUT, &layout) == ZZ_APERTURE_LEGACY);
	CHECK(zz_z2_aperture_negotiate(0x12345678, 0x400000,
		ZZ_FW_CAP_Z2_APERTURE_LAYOUT, &layout) == ZZ_APERTURE_LEGACY);
	CHECK(zz_z2_aperture_negotiate(ZZ_Z2_APERTURE_INFO_4M, 0x200000,
		ZZ_FW_CAP_Z2_APERTURE_LAYOUT, &layout) == ZZ_APERTURE_INVALID);
	CHECK(zz_z2_aperture_negotiate(0x5a030704, 0x400000,
		ZZ_FW_CAP_Z2_APERTURE_LAYOUT, &layout) == ZZ_APERTURE_INVALID);
	CHECK(zz_z2_aperture_negotiate(0x5a010604, 0x400000,
		ZZ_FW_CAP_Z2_APERTURE_LAYOUT, &layout) == ZZ_APERTURE_INVALID);
}

/* PR #74 review: generation-1 descriptors must keep negotiating VALID
 * (with their old larger host window) so a new driver cannot disable RTG
 * on an old FPGA/firmware pair, and the acknowledgement must echo the
 * descriptor's own generation. */
static void test_generation1_compatibility(void)
{
	struct ZZApertureLayout gen1;
	struct ZZApertureLayout gen2;

	CHECK(zz_z2_aperture_negotiate(ZZ_Z2_APERTURE_INFO_4M_GEN1, 0x400000,
		ZZ_FW_CAP_Z2_APERTURE_LAYOUT, &gen1) == ZZ_APERTURE_VALID);
	CHECK(zz_z2_aperture_negotiate(ZZ_Z2_APERTURE_INFO_2M_GEN1, 0x200000,
		ZZ_FW_CAP_Z2_APERTURE_LAYOUT, &gen1) == ZZ_APERTURE_VALID);
	CHECK(zz_z2_aperture_negotiate(ZZ_Z2_APERTURE_INFO_8M_GEN1, 0x800000,
		ZZ_FW_CAP_Z2_APERTURE_LAYOUT, &gen1) == ZZ_APERTURE_VALID);
	CHECK(zz_z2_aperture_profile(ZZ_Z2_APERTURE_INFO_4M_GEN1, &gen1));
	CHECK(zz_z2_aperture_profile(ZZ_Z2_APERTURE_INFO_4M, &gen2));
	CHECK(gen1.host_window.size == 0x00010000UL);
	CHECK(gen2.host_window.size == 0x00004000UL);
	CHECK(zz_z2_aperture_ack_token(ZZ_Z2_APERTURE_INFO_4M_GEN1) ==
		ZZ_Z2_APERTURE_ACK_TOKEN_GEN1);
	CHECK(zz_z2_aperture_ack_token(ZZ_Z2_APERTURE_INFO_4M) ==
		ZZ_Z2_APERTURE_ACK_TOKEN_GEN2);
	/* A gen-1 descriptor against the wrong board size still fails. */
	CHECK(zz_z2_aperture_negotiate(ZZ_Z2_APERTURE_INFO_4M_GEN1, 0x200000,
		ZZ_FW_CAP_Z2_APERTURE_LAYOUT, &gen1) == ZZ_APERTURE_INVALID);
}

static void test_malformed_layouts(void)
{
	struct ZZApertureLayout layout;

	CHECK(zz_z2_aperture_profile(ZZ_Z2_APERTURE_INFO_4M, &layout));
	layout.pip.base = layout.framebuffer.base;
	CHECK(!zz_aperture_layout_valid(&layout)); /* overlap */

	CHECK(zz_z2_aperture_profile(ZZ_Z2_APERTURE_INFO_4M, &layout));
	layout.host_window.base = 0xfffffff0U;
	layout.host_window.size = 0x100U;
	CHECK(!zz_aperture_layout_valid(&layout)); /* overflow */

	CHECK(zz_z2_aperture_profile(ZZ_Z2_APERTURE_INFO_2M, &layout));
	layout.audio.base--;
	CHECK(!zz_aperture_layout_valid(&layout)); /* top reservation mismatch */
}

static void test_z3_boundary(void)
{
	CHECK(ZZ_Z3_P96_MEMORY_SIZE == 0x02e00000UL);
	CHECK(ZZ_Z3_P96_MEMORY_ARM_START + ZZ_Z3_P96_MEMORY_SIZE ==
		ZZ_SDK_SHARED_HEAP_ARM_START);
}

static void test_z2_staging_bounds(void)
{
	CHECK(zz_z2_staging_fits(0, 0x2000, 8));
	CHECK(!zz_z2_staging_fits(0, 0x2001, 8));
	CHECK(zz_z2_staging_fits(0x400, 0x1f80, 8));
	CHECK(!zz_z2_staging_fits(0x400, 0x1f81, 8));
	CHECK(!zz_z2_staging_fits(0x10001, 0, 0));
	CHECK(!zz_z2_staging_fits(0, UINT32_MAX, UINT32_MAX));
}

int main(void)
{
	test_profiles();
	test_compatibility_matrix();
	test_generation1_compatibility();
	test_malformed_layouts();
	test_z3_boundary();
	test_z2_staging_bounds();

	if (failures) {
		printf("%d of %d memory layout checks failed\n", failures, checks);
		return 1;
	}
	printf("all %d memory layout checks passed\n", checks);
	return 0;
}
