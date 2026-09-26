/*
 * Small helpers for ZZ9000 blitter register and descriptor writes.
 *
 * The firmware keeps the latest register values, so a repeated write of the
 * same value to the same board can be skipped on slow Zorro II setup paths.
 */

#ifndef ZZ9000_BLITTER_CACHE_H
#define ZZ9000_BLITTER_CACHE_H

#include <stdint.h>

/* Two adjacent, four-byte-aligned descriptor halves can share one Zorro III
 * 32-bit transaction. Their byte layout must match two individual 16-bit
 * stores: the ARM firmware swaps each half in place after consuming a command.
 * may_alias permits writing the existing uint16_t fields through a word. */
typedef uint32_t __attribute__((__may_alias__)) BlitterDescriptorWord;

static inline void blitter_write_descriptor_pair(volatile uint16_t *fields,
	uint16_t first, uint16_t second)
{
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
	*(volatile BlitterDescriptorWord *)fields =
		((uint32_t)first << 16) | second;
#elif __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	*(volatile BlitterDescriptorWord *)fields =
		((uint32_t)second << 16) | first;
#else
#error Unknown byte order for descriptor writes
#endif
}

struct BlitterRegisterCache {
	const volatile void *registers;
	uint32_t valid;
	uint16_t src_pitch;
	uint16_t user1;
	uint16_t user2;
	uint32_t rgb2;
};

enum {
	BLITTER_CACHE_SRC_PITCH = 1u << 0,
	BLITTER_CACHE_USER1     = 1u << 1,
	BLITTER_CACHE_USER2     = 1u << 2,
	BLITTER_CACHE_RGB2      = 1u << 3,
};

static inline void blitter_cache_reset(struct BlitterRegisterCache *cache)
{
	cache->registers = 0;
	cache->valid = 0;
	cache->src_pitch = 0;
	cache->user1 = 0;
	cache->user2 = 0;
	cache->rgb2 = 0;
}

static inline void blitter_cache_select(struct BlitterRegisterCache *cache,
	const volatile void *registers)
{
	if (cache->registers != registers) {
		cache->registers = registers;
		cache->valid = 0;
	}
}

static inline void blitter_cache_invalidate(struct BlitterRegisterCache *cache,
	const volatile void *registers, uint32_t bits)
{
	blitter_cache_select(cache, registers);
	cache->valid &= ~bits;
}

static inline int blitter_cache_write16_needed(struct BlitterRegisterCache *cache,
	const volatile void *registers, uint32_t bit, uint16_t *slot, uint16_t value)
{
	blitter_cache_select(cache, registers);
	if (!(cache->valid & bit) || *slot != value) {
		*slot = value;
		cache->valid |= bit;
		return 1;
	}

	return 0;
}

static inline int blitter_cache_src_pitch_write_needed(struct BlitterRegisterCache *cache,
	const volatile void *registers, uint16_t value)
{
	blitter_cache_select(cache, registers);
	cache->src_pitch = value;
	cache->valid &= ~BLITTER_CACHE_SRC_PITCH;
	return 1;
}

static inline int blitter_cache_write32_needed(struct BlitterRegisterCache *cache,
	const volatile void *registers, uint32_t bit, uint32_t *slot, uint32_t value)
{
	blitter_cache_select(cache, registers);
	if (!(cache->valid & bit) || *slot != value) {
		*slot = value;
		cache->valid |= bit;
		return 1;
	}

	return 0;
}

#endif
