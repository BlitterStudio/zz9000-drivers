/* P96 card-memory command marshalling helpers. */
#ifndef ZZ9000_CARDMEM_PROTOCOL_H
#define ZZ9000_CARDMEM_PROTOCOL_H

#include <stdint.h>

#include "memory_layout.h"

#define ZZ_CARDMEM_GRANULARITY 256UL

static inline uint32_t zz_cardmem_byte_swap32(uint32_t value)
{
	return ((value & 0x000000ffUL) << 24) |
		((value & 0x0000ff00UL) << 8) |
		((value & 0x00ff0000UL) >> 8) |
		((value & 0xff000000UL) >> 24);
}

static inline uint32_t zz_cardmem_to_be32(uint32_t value)
{
#if (defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && \
	__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__) || defined(__mc68000__)
	return value;
#else
	return zz_cardmem_byte_swap32(value);
#endif
}

static inline uint32_t zz_cardmem_from_be32(uint32_t value)
{
	return zz_cardmem_to_be32(value);
}

static inline void zz_cardmem_store_be32(volatile uint32_t *destination,
	uint32_t value)
{
	*destination = zz_cardmem_to_be32(value);
}

static inline uint32_t zz_cardmem_load_be32(const volatile uint32_t *source)
{
	return zz_cardmem_from_be32(*source);
}

static inline uint32_t zz_cardmem_round_size(uint32_t size)
{
	if (size > UINT32_MAX - (ZZ_CARDMEM_GRANULARITY - 1))
		return 0;
	return (size + ZZ_CARDMEM_GRANULARITY - 1) &
		~(ZZ_CARDMEM_GRANULARITY - 1);
}

/* P96 supplies bytesperrow and RGBFormat for displayable bitmaps. The
 * firmware's 256-byte start granularity exceeds P96's normal alignment for
 * every supported pixel format, so their address alignment needs no further
 * restriction. */
static inline uint32_t zz_cardmem_alignment(uint32_t bytesperrow,
	uint32_t rgbformat)
{
	(void)bytesperrow;
	(void)rgbformat;
	return ZZ_CARDMEM_GRANULARITY;
}

static inline int zz_cardmem_status_accepted(uint8_t status)
{
	return status != 0;
}

static inline void zz_cardmem_marshal_alloc(volatile uint32_t offset[2],
	uint32_t size, uint32_t alignment)
{
	zz_cardmem_store_be32(&offset[0], alignment);
	zz_cardmem_store_be32(&offset[1], size);
}

static inline void zz_cardmem_marshal_abs(volatile uint32_t offset[2],
	uint32_t target, uint32_t size)
{
	zz_cardmem_store_be32(&offset[0], target);
	zz_cardmem_store_be32(&offset[1], size);
}

static inline uint32_t zz_cardmem_unmarshal_reply(
	const volatile uint32_t offset[2])
{
	return zz_cardmem_load_be32(&offset[0]);
}

#endif /* ZZ9000_CARDMEM_PROTOCOL_H */
