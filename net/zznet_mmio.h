/*
 * zznet_mmio.h — single-copy direct drain (KTD4), host-testable.
 *
 * The same function runs against the FPGA RX window on the device
 * (volatile MMIO) and against a RAM buffer in net/tests/drain_test.c;
 * the contract is the payload-relative sum position and the word-
 * aligned reads at src.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef _INC_ZZNET_MMIO_H
#define _INC_ZZNET_MMIO_H

#include <exec/types.h>

/* The RX window is big-endian. On the device a word read of the window
 * is one Zorro cycle; the host-side tests run on little-endian RAM,
 * so the host build assembles the window words byte-wise instead.
 * The sum positions and the copy are endian-neutral either way. */
#ifdef ZZNET_HOST_TEST
static inline UWORD zznet_mmio_wread(const volatile UBYTE *p)
{
	return (UWORD)(((UWORD)p[0] << 8) | p[1]);
}
static inline void zznet_mmio_wwrite(volatile UBYTE *p, UWORD w)
{
	p[0] = (UBYTE)(w >> 8);
	p[1] = (UBYTE)(w & 0xff);
}
#else
#define zznet_mmio_wread(p) (*(const volatile UWORD *)(p))
#define zznet_mmio_wwrite(p, w) ((*(volatile UWORD *)(p) = (w)))
#endif

/* Single-copy direct drain (KTD4): MMIO → the opener's answered buffer,
 * accumulating the published ones-complement sum. Source is the FPGA
 * window (volatile, word-aligned reads); dst is opener RAM where
 * unaligned longs are legal on 68020+.
 *
 * Sum positions are relative to the PAYLOAD start, never to the MMIO
 * address: the published contract sums consecutive big-endian
 * longwords of the delivered bytes (bytes 0-3, 4-7, ...), so for
 * 01 02 03 04 05 06 the sum is 0x01020304 + 0x05060000. The payload
 * begins 2 mod 4 in the window, so a longword covering bytes 0-3 can
 * never be one aligned MMIO read; each published longword is assembled
 * from two word reads (each a single Zorro cycle) with end-around
 * carry per longword — exactly n68k_port_in_l_sum's per-longword
 * accumulation, at twice the word-cycle count of an address-aligned
 * walk. That is the price of a sum the opener can trust. */
static inline ULONG zznet_mmio_read_sum(volatile UBYTE *src, UBYTE *dst, ULONG n)
{
	ULONG sum = 0;

	while (n >= 4) {
		UWORD whi = zznet_mmio_wread(src);
		UWORD wlo = zznet_mmio_wread(src + 2);
		ULONG v;
		zznet_mmio_wwrite(dst, whi);
		zznet_mmio_wwrite(dst + 2, wlo);
		v = ((ULONG)whi << 16) | wlo;
		sum += v;
		if (sum < v) sum++;
		src += 4; dst += 4; n -= 4;
	}

	if (n) {
		/* Zero-padded BE tail: the remaining 1-3 bytes occupy the
		 * high bytes of a final longword — a 3-byte tail is
		 * word<<16 | byte<<8, a 2-byte tail word<<16, a lone byte
		 * (1 mod 4 payload) sits at the very top: byte<<24. */
		ULONG v = 0;
		int had_word = 0;
		if (n >= 2) {
			UWORD w = zznet_mmio_wread(src);
			zznet_mmio_wwrite(dst, w);
			src += 2; dst += 2; n -= 2;
			v |= (ULONG)w << 16;
			had_word = 1;
		}
		if (n) {
			UBYTE b = *src;
			*dst = b;
			v |= (ULONG)b << (had_word ? 8 : 24);
		}
		sum += v;
		if (sum < v) sum++;
	}

	return sum;
}

#endif /* _INC_ZZNET_MMIO_H */
