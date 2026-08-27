/*
 * Host-visible Zorro II aperture descriptor ABI.
 *
 * This header deliberately has no AmigaOS dependencies so the layout
 * negotiation can be exercised by the native RTG unit tests.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ZZ9000_APERTURE_H
#define ZZ9000_APERTURE_H

#include <stdint.h>

/* The FPGA exposes the descriptor as two ordered 16-bit words.  Offsets are
 * relative to the board's AutoConfig base (the direct-register bank starts at
 * 0x1000).  The low word is also the write-only host acknowledgement. */
#define ZZ_REG_Z2_APERTURE_INFO_HI 0x111cUL
#define ZZ_REG_Z2_APERTURE_INFO_LO 0x111eUL
#define ZZ_Z2_APERTURE_ACK_TOKEN   0xa502U

#define ZZ_FW_CAP_Z2_APERTURE_LAYOUT (1U << 2)

#define ZZ_Z2_APERTURE_INFO_MAGIC_MASK      0xff000000UL
#define ZZ_Z2_APERTURE_INFO_MAGIC           0x5a000000UL
#define ZZ_Z2_APERTURE_INFO_GENERATION_MASK 0x00ff0000UL
#define ZZ_Z2_APERTURE_INFO_GENERATION_2    0x00020000UL

#define ZZ_Z2_APERTURE_INFO_2M 0x5a020502UL
#define ZZ_Z2_APERTURE_INFO_4M 0x5a020704UL
#define ZZ_Z2_APERTURE_INFO_8M 0x5a020708UL

#define ZZ_Z2_REGISTER_SPACE_SIZE 0x00010000UL
#define ZZ_Z2_TEMPLATE_SIZE       0x00010000UL
#define ZZ_Z2_AUDIO_SIZE          0x00010000UL
/* Generation 2 carves a fixed 48 KiB direct-ring reservation out of the
 * top of the old host-window region: it sits between the smaller host heap
 * and the audio scratch, holds the single Z2 audio direct-ring grant, and
 * is not a payload region -- clients learn its geometry only from the SDK
 * acquire op. */
#define ZZ_Z2_DIRECT_RING_RESERVE_SIZE 0x0000C000UL

struct ZZApertureRegion {
	uint32_t base; /* board-relative */
	uint32_t size;
};

struct ZZApertureLayout {
	uint32_t descriptor;
	uint32_t aperture_size;
	struct ZZApertureRegion framebuffer;
	struct ZZApertureRegion pip;
	struct ZZApertureRegion template_scratch;
	struct ZZApertureRegion host_window;
	struct ZZApertureRegion audio;
};

enum ZZApertureNegotiation {
	ZZ_APERTURE_INVALID = -1,
	ZZ_APERTURE_LEGACY = 0,
	ZZ_APERTURE_VALID = 1
};

static inline int zz_region_end(uint32_t base, uint32_t size,
	uint32_t *end)
{
	if (size > UINT32_MAX - base)
		return 0;
	*end = base + size;
	return 1;
}

static inline int zz_aperture_region_valid(
	const struct ZZApertureRegion *region, uint32_t aperture_size,
	int allow_empty)
{
	uint32_t end;

	if (!region || (!allow_empty && region->size == 0))
		return 0;
	if (region->size == 0)
		return region->base == 0;
	return zz_region_end(region->base, region->size, &end) &&
		end <= aperture_size;
}

static inline int zz_aperture_regions_overlap(
	const struct ZZApertureRegion *a, const struct ZZApertureRegion *b)
{
	uint32_t a_end, b_end;

	if (!a->size || !b->size)
		return 0;
	if (!zz_region_end(a->base, a->size, &a_end) ||
	    !zz_region_end(b->base, b->size, &b_end))
		return 1;
	return a->base < b_end && b->base < a_end;
}

static inline int zz_aperture_layout_valid(
	const struct ZZApertureLayout *layout)
{
	const struct ZZApertureRegion *regions[5];
	unsigned i, j;

	if (!layout || layout->aperture_size < ZZ_Z2_REGISTER_SPACE_SIZE ||
	    layout->framebuffer.base != ZZ_Z2_REGISTER_SPACE_SIZE)
		return 0;
	if (!zz_aperture_region_valid(&layout->framebuffer,
			layout->aperture_size, 0) ||
	    !zz_aperture_region_valid(&layout->pip,
			layout->aperture_size, 1) ||
	    !zz_aperture_region_valid(&layout->template_scratch,
			layout->aperture_size, 0) ||
	    !zz_aperture_region_valid(&layout->host_window,
			layout->aperture_size, 0) ||
	    !zz_aperture_region_valid(&layout->audio,
			layout->aperture_size, 0))
		return 0;

	regions[0] = &layout->framebuffer;
	regions[1] = &layout->pip;
	regions[2] = &layout->template_scratch;
	regions[3] = &layout->host_window;
	regions[4] = &layout->audio;
	for (i = 0; i < 5; i++)
		for (j = i + 1; j < 5; j++)
			if (zz_aperture_regions_overlap(regions[i], regions[j]))
				return 0;

	return layout->template_scratch.size == ZZ_Z2_TEMPLATE_SIZE &&
		layout->audio.size == ZZ_Z2_AUDIO_SIZE &&
		layout->audio.base + layout->audio.size ==
		layout->aperture_size;
}

static inline int zz_z2_aperture_profile(uint32_t descriptor,
	struct ZZApertureLayout *layout)
{
	struct ZZApertureLayout value;

	value.descriptor = descriptor;
	value.pip.base = 0;
	value.pip.size = 0;
	switch (descriptor) {
	case ZZ_Z2_APERTURE_INFO_2M:
		value.aperture_size = 0x00200000UL;
		value.framebuffer = (struct ZZApertureRegion){0x00010000UL, 0x001c0000UL};
		value.template_scratch = (struct ZZApertureRegion){0x001d0000UL, 0x00010000UL};
		value.host_window = (struct ZZApertureRegion){0x001e0000UL, 0x00004000UL};
		value.audio = (struct ZZApertureRegion){0x001f0000UL, 0x00010000UL};
		break;
	case ZZ_Z2_APERTURE_INFO_4M:
		value.aperture_size = 0x00400000UL;
		value.framebuffer = (struct ZZApertureRegion){0x00010000UL, 0x00388000UL};
		value.pip = (struct ZZApertureRegion){0x00398000UL, 0x00038000UL};
		value.template_scratch = (struct ZZApertureRegion){0x003d0000UL, 0x00010000UL};
		value.host_window = (struct ZZApertureRegion){0x003e0000UL, 0x00004000UL};
		value.audio = (struct ZZApertureRegion){0x003f0000UL, 0x00010000UL};
		break;
	case ZZ_Z2_APERTURE_INFO_8M:
		value.aperture_size = 0x00800000UL;
		value.framebuffer = (struct ZZApertureRegion){0x00010000UL, 0x00770000UL};
		value.pip = (struct ZZApertureRegion){0x00780000UL, 0x00040000UL};
		value.template_scratch = (struct ZZApertureRegion){0x007c0000UL, 0x00010000UL};
		value.host_window = (struct ZZApertureRegion){0x007d0000UL, 0x00014000UL};
		value.audio = (struct ZZApertureRegion){0x007f0000UL, 0x00010000UL};
		break;
	default:
		return 0;
	}

	if (!zz_aperture_layout_valid(&value))
		return 0;
	if (layout)
		*layout = value;
	return 1;
}

/* Compatibility handshake. A capability/descriptor half-match is never
 * enough to change ownership: no firmware capability, or a non-magic FPGA
 * value, preserves the legacy layout and never acknowledges it. */
static inline enum ZZApertureNegotiation zz_z2_aperture_negotiate(
	uint32_t descriptor, uint32_t board_size, uint16_t firmware_caps,
	struct ZZApertureLayout *layout)
{
	struct ZZApertureLayout value;

	if (!(firmware_caps & ZZ_FW_CAP_Z2_APERTURE_LAYOUT))
		return ZZ_APERTURE_LEGACY;
	if ((descriptor & ZZ_Z2_APERTURE_INFO_MAGIC_MASK) !=
			ZZ_Z2_APERTURE_INFO_MAGIC)
		return ZZ_APERTURE_LEGACY;
	if (!zz_z2_aperture_profile(descriptor, &value) ||
	    value.aperture_size != board_size)
		return ZZ_APERTURE_INVALID;
	if (layout)
		*layout = value;
	return ZZ_APERTURE_VALID;
}

static inline uint32_t zz_aperture_memory_offset(uint32_t board_offset)
{
	return board_offset - ZZ_Z2_REGISTER_SPACE_SIZE;
}

#endif /* ZZ9000_APERTURE_H */
