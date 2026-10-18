/* ZZ9000 RTG/SDK memory ownership boundaries. */

#ifndef ZZ9000_MEMORY_LAYOUT_H
#define ZZ9000_MEMORY_LAYOUT_H

#include <stdint.h>
#include "../include/zz9000_aperture.h"

/*
 * Z3 main-board window, as offsets from P96's MemoryBase (ARM 0x00200000).
 * P96 runs its own allocator over [MemoryBase, MemoryBase + MemorySize) for
 * screens and every bitmap ZZ_AllocBitMap declines; the firmware cannot see
 * those allocations. MemorySize therefore stops exactly at the GFXData
 * scratch, where the firmware's host-visible heap begins (template scratch,
 * capture backing, off-screen surfaces from ACC_OP_ALLOC_SURFACE, SDK
 * allocations, mailbox and audio rings).
 */
#define Z3_GFXDATA_ADDR  0x031F0000UL
#define Z3_TEMPLATE_ADDR 0x03200000UL
#define ZZ_Z3_P96_MEMORY_ARM_START 0x00200000UL
#define ZZ_Z3_P96_MEMORY_SIZE      Z3_GFXDATA_ADDR
#define ZZ_Z3_P96_MEMORY_ARM_END \
	(ZZ_Z3_P96_MEMORY_ARM_START + ZZ_Z3_P96_MEMORY_SIZE)

static inline int zz_z2_staging_fits(uint32_t prefix, uint32_t unit_size,
	uint32_t count)
{
	return prefix <= ZZ_Z2_TEMPLATE_SIZE &&
		(count == 0 || unit_size <=
			(ZZ_Z2_TEMPLATE_SIZE - prefix) / count);
}

#endif /* ZZ9000_MEMORY_LAYOUT_H */
