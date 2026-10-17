/* ZZ9000 RTG/SDK memory ownership boundaries. */

#ifndef ZZ9000_MEMORY_LAYOUT_H
#define ZZ9000_MEMORY_LAYOUT_H

#include <stdint.h>
#include "../include/zz9000_aperture.h"

/*
 * P96 and SDK host-visible allocations share the firmware's interval
 * allocator. The RTG driver sees the whole Z3 aperture below the firmware
 * image; individual off-screen surfaces are reserved through ACC_OP rather
 * than by a second static split.
 */
#define ZZ_Z3_P96_MEMORY_ARM_START 0x00200000UL
#define ZZ_Z3_P96_MEMORY_ARM_END   0x08200000UL
#define ZZ_Z3_P96_MEMORY_SIZE \
	(ZZ_Z3_P96_MEMORY_ARM_END - ZZ_Z3_P96_MEMORY_ARM_START)

static inline int zz_z2_staging_fits(uint32_t prefix, uint32_t unit_size,
	uint32_t count)
{
	return prefix <= ZZ_Z2_TEMPLATE_SIZE &&
		(count == 0 || unit_size <=
			(ZZ_Z2_TEMPLATE_SIZE - prefix) / count);
}

#endif /* ZZ9000_MEMORY_LAYOUT_H */
