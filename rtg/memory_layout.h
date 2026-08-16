/* ZZ9000 RTG/SDK memory ownership boundaries. */

#ifndef ZZ9000_MEMORY_LAYOUT_H
#define ZZ9000_MEMORY_LAYOUT_H

#include <stdint.h>
#include "../include/zz9000_aperture.h"

/*
 * P96's MemoryBase maps to firmware FRAMEBUFFER_ADDRESS. The SDK shared heap
 * begins at 0x03000000 in the firmware's ARM address space, so P96 must stop
 * allocating VRAM before that address. Keeping this relationship expressed as
 * a subtraction makes the cross-component boundary explicit.
 */
#define ZZ_Z3_P96_MEMORY_ARM_START 0x00200000UL
#define ZZ_SDK_SHARED_HEAP_ARM_START 0x03000000UL
#define ZZ_Z3_P96_MEMORY_SIZE \
	(ZZ_SDK_SHARED_HEAP_ARM_START - ZZ_Z3_P96_MEMORY_ARM_START)

static inline int zz_z2_staging_fits(uint32_t prefix, uint32_t unit_size,
	uint32_t count)
{
	return prefix <= ZZ_Z2_TEMPLATE_SIZE &&
		(count == 0 || unit_size <=
			(ZZ_Z2_TEMPLATE_SIZE - prefix) / count);
}

#endif /* ZZ9000_MEMORY_LAYOUT_H */
