/* ZZ9000 RTG/SDK memory ownership boundaries. */

#ifndef ZZ9000_MEMORY_LAYOUT_H
#define ZZ9000_MEMORY_LAYOUT_H

#include <stdint.h>
#include "../include/zz9000_aperture.h"

/*
 * Z3 legacy P96 region, as offsets from P96's MemoryBase (ARM 0x00200000).
 * Firmware without ZZ_FW_CAP_P96_CARDMEM must keep P96 below the GFXData
 * scratch. Capable firmware arbitrates the full board window itself; its
 * fixed GFXData/template/mailbox reservations remain unavailable to P96.
 */
#define Z3_GFXDATA_ADDR  0x031F0000UL
#define Z3_TEMPLATE_ADDR 0x03200000UL
#define ZZ_Z3_P96_MEMORY_ARM_START 0x00200000UL
#define ZZ_Z3_P96_MEMORY_SIZE      Z3_GFXDATA_ADDR
#define ZZ_Z3_P96_MEMORY_ARM_END \
	(ZZ_Z3_P96_MEMORY_ARM_START + ZZ_Z3_P96_MEMORY_SIZE)

static inline int zz_cardmem_enabled(int z3, uint16_t firmware_capabilities)
{
	return z3 && (firmware_capabilities & ZZ_FW_CAP_P96_CARDMEM) != 0;
}

static inline uint32_t zz_cardmem_memory_size(int z3,
	uint16_t firmware_capabilities, uint32_t card_window_size,
	uint32_t legacy_size)
{
	return zz_cardmem_enabled(z3, firmware_capabilities) ?
		card_window_size : legacy_size;
}

static inline int zz_z2_staging_fits(uint32_t prefix, uint32_t unit_size,
	uint32_t count)
{
	return prefix <= ZZ_Z2_TEMPLATE_SIZE &&
		(count == 0 || unit_size <=
			(ZZ_Z2_TEMPLATE_SIZE - prefix) / count);
}

#endif /* ZZ9000_MEMORY_LAYOUT_H */
