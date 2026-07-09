/*
 * Pure helpers for the P96 video window (PIP) Features API.
 *
 * No Amiga headers so the logic is host-testable; the driver keeps the
 * feature record and the register/tag plumbing in mntgfx-gcc.c and
 * passes plain integers in here. The contract is modeled on WinUAE's
 * uaegfx overlay implementation (picasso96_win.cpp), the only public
 * reference for how P96 drives these hooks.
 */

#ifndef ZZ9000_OVERLAY_FEATURE_H
#define ZZ9000_OVERLAY_FEATURE_H

#include <stdint.h>

/* Feature-attribute tags (mirror boardinfo.h: TAG_USER + n). */
#define ZZ_FA_ACTIVE        0x80000002u
#define ZZ_FA_LEFT          0x80000003u
#define ZZ_FA_TOP           0x80000004u
#define ZZ_FA_WIDTH         0x80000005u
#define ZZ_FA_HEIGHT        0x80000006u
#define ZZ_FA_FORMAT        0x80000007u
#define ZZ_FA_COLOR         0x80000008u
#define ZZ_FA_OCCLUSION     0x80000009u
#define ZZ_FA_SOURCEWIDTH   0x8000000Au
#define ZZ_FA_SOURCEHEIGHT  0x8000000Bu
#define ZZ_FA_MINWIDTH      0x8000000Cu
#define ZZ_FA_MINHEIGHT     0x8000000Du
#define ZZ_FA_MAXWIDTH      0x8000000Eu
#define ZZ_FA_MAXHEIGHT     0x8000000Fu
#define ZZ_FA_BITMAP        0x80000012u
#define ZZ_FA_BRIGHTNESS    0x80000013u
#define ZZ_FA_CLIPLEFT      0x80000021u
#define ZZ_FA_CLIPTOP       0x80000022u
#define ZZ_FA_CLIPWIDTH     0x80000023u
#define ZZ_FA_CLIPHEIGHT    0x80000024u

/* WinUAE's answers for the size limits P96 queries. */
#define ZZ_OVERLAY_MIN_DIM 16u
#define ZZ_OVERLAY_MAX_DIM 4096u

/* Overlay source formats accepted initially: the unambiguous packed
 * 4:2:2 orderings (RGBFB bit space, as in supported_rgb_format). The
 * PA/PAPC variants are kernel-ready but stay unadvertised until
 * hardware-validated. */
#define ZZ_OVERLAY_SOURCE_FORMATS \
	((1UL << 14) | (1UL << 17) | (1UL << 18))

/* RGBFB YUV format -> firmware yuv422_variant, -1 if unsupported. */
static inline int zz_overlay_variant(uint32_t rgbformat)
{
	switch (rgbformat) {
		case 14: return 0; /* YUV422CGX -> YUV422_VARIANT_CGX */
		case 17: return 1; /* YUV422    -> YUV422_VARIANT_STD */
		case 18: return 2; /* YUV422PC  -> YUV422_VARIANT_PC */
	}

	return -1;
}

/* CreateFeature source validation (WinUAE parity: >=16x16, <=4096,
 * supported format). The <=screen check is left to the firmware, which
 * clips at composite time anyway. */
static inline int zz_overlay_source_valid(uint32_t src_w, uint32_t src_h,
	uint32_t rgbformat)
{
	return src_w >= ZZ_OVERLAY_MIN_DIM && src_h >= ZZ_OVERLAY_MIN_DIM &&
		src_w <= ZZ_OVERLAY_MAX_DIM && src_h <= ZZ_OVERLAY_MAX_DIM &&
		zz_overlay_variant(rgbformat) >= 0;
}

/* The driver-side feature record; a pointer to it is the opaque cookie
 * handed to P96. */
struct ZZOverlayState {
	uint32_t magic;         /* cookie validation */
	uint32_t rgbformat;
	uint16_t src_w, src_h;
	int16_t dst_x, dst_y;
	int16_t dst_w, dst_h;
	uint32_t color_key;     /* as P96 gave it (FA_Color) */
	uint8_t occlusion;
	uint8_t active;
	/* stored but deliberately not applied (WinUAE parity) */
	int16_t clip_l, clip_t, clip_w, clip_h;
	uint32_t brightness;
};

#define ZZ_OVERLAY_MAGIC 0x5A5A4F56u /* 'ZZOV' */

/* Apply one SetFeatureAttrs/CreateFeature tag to the record. Returns 1
 * if the tag changed state the firmware must see (geometry/key/active),
 * 0 if it was stored-only or ignored. */
static inline int zz_overlay_apply_tag(struct ZZOverlayState *st,
	uint32_t tag, uint32_t data)
{
	switch (tag) {
		case ZZ_FA_ACTIVE:      st->active = data ? 1 : 0; return 1;
		case ZZ_FA_LEFT:        st->dst_x = (int16_t)(int32_t)data; return 1;
		case ZZ_FA_TOP:         st->dst_y = (int16_t)(int32_t)data; return 1;
		case ZZ_FA_WIDTH:       st->dst_w = (int16_t)(int32_t)data; return 1;
		case ZZ_FA_HEIGHT:      st->dst_h = (int16_t)(int32_t)data; return 1;
		case ZZ_FA_OCCLUSION:   st->occlusion = data ? 1 : 0; return 1;
		case ZZ_FA_COLOR:       st->color_key = data; return 1;
		case ZZ_FA_FORMAT:      st->rgbformat = data; return 1;
		case ZZ_FA_SOURCEWIDTH: st->src_w = (uint16_t)data; return 1;
		case ZZ_FA_SOURCEHEIGHT: st->src_h = (uint16_t)data; return 1;
		case ZZ_FA_CLIPLEFT:    st->clip_l = (int16_t)(int32_t)data; return 0;
		case ZZ_FA_CLIPTOP:     st->clip_t = (int16_t)(int32_t)data; return 0;
		case ZZ_FA_CLIPWIDTH:   st->clip_w = (int16_t)(int32_t)data; return 0;
		case ZZ_FA_CLIPHEIGHT:  st->clip_h = (int16_t)(int32_t)data; return 0;
		case ZZ_FA_BRIGHTNESS:  st->brightness = data; return 0;
	}

	return 0;
}

/* GetFeatureAttrs answer for one queried tag; returns 1 if answered.
 * `bitmap` is the source BitMap pointer, `have` whether a feature
 * exists. */
static inline int zz_overlay_query_tag(const struct ZZOverlayState *st,
	int have, uint32_t bitmap, uint32_t tag, uint32_t *out)
{
	switch (tag) {
		case ZZ_FA_MINWIDTH:
		case ZZ_FA_MINHEIGHT:  *out = ZZ_OVERLAY_MIN_DIM; return 1;
		case ZZ_FA_MAXWIDTH:
		case ZZ_FA_MAXHEIGHT:  *out = ZZ_OVERLAY_MAX_DIM; return 1;
	}

	if (!have)
		return 0;

	switch (tag) {
		case ZZ_FA_ACTIVE:       *out = st->active; return 1;
		case ZZ_FA_OCCLUSION:    *out = st->occlusion; return 1;
		case ZZ_FA_LEFT:         *out = (uint32_t)(int32_t)st->dst_x; return 1;
		case ZZ_FA_TOP:          *out = (uint32_t)(int32_t)st->dst_y; return 1;
		case ZZ_FA_WIDTH:        *out = (uint32_t)(int32_t)st->dst_w; return 1;
		case ZZ_FA_HEIGHT:       *out = (uint32_t)(int32_t)st->dst_h; return 1;
		case ZZ_FA_SOURCEWIDTH:  *out = st->src_w; return 1;
		case ZZ_FA_SOURCEHEIGHT: *out = st->src_h; return 1;
		case ZZ_FA_FORMAT:       *out = st->rgbformat; return 1;
		case ZZ_FA_COLOR:        *out = st->color_key; return 1;
		case ZZ_FA_BITMAP:       *out = bitmap; return 1;
		case ZZ_FA_BRIGHTNESS:   *out = st->brightness; return 1;
		case ZZ_FA_CLIPLEFT:     *out = (uint32_t)(int32_t)st->clip_l; return 1;
		case ZZ_FA_CLIPTOP:      *out = (uint32_t)(int32_t)st->clip_t; return 1;
		case ZZ_FA_CLIPWIDTH:    *out = (uint32_t)(int32_t)st->clip_w; return 1;
		case ZZ_FA_CLIPHEIGHT:   *out = (uint32_t)(int32_t)st->clip_h; return 1;
	}

	return 0;
}

#endif
