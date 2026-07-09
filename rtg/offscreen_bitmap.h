/*
 * Pure helpers for P96 off-screen bitmaps placed in ZZ9000 card VRAM
 * (the AllocBitMap/FreeBitMap/GetBitMapAttr hooks).
 *
 * No Amiga headers so the logic is host-testable; the driver keeps its
 * metadata struct (which embeds struct BitMap) in mntgfx-gcc.c and
 * passes plain integers in here.
 */

#ifndef ZZ9000_OFFSCREEN_BITMAP_H
#define ZZ9000_OFFSCREEN_BITMAP_H

#include <stdint.h>

#define ZZ_OFFSCREEN_MAGIC 0x5A5A424Du /* 'ZZBM' */

/* The blitter programs pitches in longword units (BytesPerRow >> 2),
 * so off-screen strides must be multiples of 4 bytes. */
#define ZZ_OFFSCREEN_PITCH_ALIGN 4u

static inline uint32_t zz_offscreen_pad_pitch(uint32_t bytesperrow)
{
	return (bytesperrow + (ZZ_OFFSCREEN_PITCH_ALIGN - 1)) &
		~(ZZ_OFFSCREEN_PITCH_ALIGN - 1);
}

/* GBMA_* GetBitMapAttr tags (mirror boardinfo.h: TAG_USER + n). */
#define ZZ_GBMA_MEMORY        0x80000000u
#define ZZ_GBMA_BASEMEMORY    0x80000001u
#define ZZ_GBMA_BYTESPERROW   0x80000002u
#define ZZ_GBMA_BYTESPERPIXEL 0x80000003u
#define ZZ_GBMA_BITSPERPIXEL  0x80000004u
#define ZZ_GBMA_RGBFORMAT     0x80000006u
#define ZZ_GBMA_WIDTH         0x80000007u
#define ZZ_GBMA_HEIGHT        0x80000008u
#define ZZ_GBMA_DEPTH         0x80000009u

/* Bytes per pixel for the RGBFB formats the card advertises
 * (ZZ_SUPPORTED_RGB_FORMATS); 0 for planar and everything else, which
 * makes AllocBitMap refuse and P96 fall back to system RAM. */
static inline uint32_t zz_rgbformat_bytes_per_pixel(uint32_t rgbformat)
{
	switch (rgbformat) {
		case 1:  return 1; /* RGBFB_CLUT */
		case 9:  return 4; /* RGBFB_B8G8R8A8 */
		case 10: return 2; /* RGBFB_R5G6B5 */
		case 11: return 2; /* RGBFB_R5G5B5 */
	}

	return 0;
}

static inline uint32_t zz_rgbformat_bits_per_pixel(uint32_t rgbformat)
{
	switch (rgbformat) {
		case 1:  return 8;
		case 9:  return 32;
		case 10: return 16;
		case 11: return 15;
	}

	return 0;
}

/* Does (magic, pixels) identify a bitmap we allocated in card VRAM?
 * The range check guards against a stale/forged magic: our pixel data
 * always lives inside the board memory window. */
static inline int zz_offscreen_is_ours(uint32_t magic, uint32_t pixels,
	uint32_t membase, uint32_t memsize)
{
	return magic == ZZ_OFFSCREEN_MAGIC &&
		pixels >= membase && pixels - membase < memsize;
}

/* Everything GetBitMapAttr can be asked about, pre-resolved by the
 * caller either from our metadata or from plain planar BitMap fields. */
struct ZZBitMapAttrs {
	uint32_t memory;        /* pixel data address (Planes[0]) */
	uint32_t basememory;    /* board MemoryBase */
	uint32_t bytesperrow;
	uint32_t bytesperpixel;
	uint32_t bitsperpixel;
	uint32_t rgbformat;
	uint32_t width;
	uint32_t height;
	uint32_t depth;
};

static inline uint32_t zz_offscreen_attr(const struct ZZBitMapAttrs *attrs,
	uint32_t attr)
{
	switch (attr) {
		case ZZ_GBMA_MEMORY:        return attrs->memory;
		case ZZ_GBMA_BASEMEMORY:    return attrs->basememory;
		case ZZ_GBMA_BYTESPERROW:   return attrs->bytesperrow;
		case ZZ_GBMA_BYTESPERPIXEL: return attrs->bytesperpixel;
		case ZZ_GBMA_BITSPERPIXEL:  return attrs->bitsperpixel;
		case ZZ_GBMA_RGBFORMAT:     return attrs->rgbformat;
		case ZZ_GBMA_WIDTH:         return attrs->width;
		case ZZ_GBMA_HEIGHT:        return attrs->height;
		case ZZ_GBMA_DEPTH:         return attrs->depth;
	}

	return 0;
}

#endif
