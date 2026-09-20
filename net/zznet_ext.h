/*
 * zznet_ext.h — host-testable negotiation and claim gating for the
 * AmiNetXDuo SANA-II buffer-management extensions (anxs2ext.h).
 *
 * Everything here operates on caller-owned memory with no Exec calls, so
 * the same code builds inside ZZ9000Net.device and inside the host-side
 * tests under net/tests/ (the pattern netdev_direct.c in tinic/AmiNetXDuo
 * was split out for).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef _INC_ZZNET_EXT_H
#define _INC_ZZNET_EXT_H

#include "anxs2ext.h"

/* struct TagItem has the same layout everywhere AmigaOS runs: an array of
 * {ti_Tag, ti_Data} ULONG pairs terminated by TAG_DONE (0). Walking it by
 * hand keeps this header free of utility.library and host-testable.
 *
 * On a 32-bit Amiga the data word is ULONG and holds a pointer exactly.
 * Host builds are 64-bit, where stuffing a pointer through ULONG truncates
 * and crashes; the tag data type widens there so the SAME logic runs in
 * both places without ifdefs at the call sites. */
#ifdef ZZNET_HOST_TEST
typedef unsigned long zznet_tag_data;
#else
typedef ULONG zznet_tag_data;
#endif

struct zznet_tag {
	ULONG tag;
	zznet_tag_data data;
};
/* Frozen SANA-II buffer-management tag values (S2_Dummy = TAG_USER +
 * 0xB0000). Spelled out so the tests need no sana2.h; net/sana2.h defines
 * the same three and the driver uses either spelling interchangeably. */
#define ZZNET_S2_CopyToBuff     (0x80000000UL + 0xB0000UL + 1)
#define ZZNET_S2_CopyFromBuff   (0x80000000UL + 0xB0000UL + 2)
#define ZZNET_S2_PacketFilter   (0x80000000UL + 0xB0000UL + 3)

/* Per-opener negotiated extension state. Zeroed at open; filled by
 * zznet_ext_negotiate(). Embedded in struct BufferManagement (device.h). */
struct zznet_ext {
	int   xe_HasPair;      /* RX_DIRECT + RX_FILLED accepted together */
	void *xe_RxDirect;     /* AnxdS2RxDirect hook                    */
	void *xe_RxFilled;     /* AnxdS2RxFilled hook                    */
	int   xe_RxLinkHdr;    /* opener promised the 14 bytes before dst */
	UBYTE xe_RxFlags;      /* accepted flag intersection             */
	void *xe_PacketFilter; /* S2_PacketFilter hook; forces staging    */
};

/* Every flag this driver can set in RX_FILLED's last argument. CONTINUES
 * requires VERIFIED (published rule); SUMMED rides every drain. */
#define ZZNET_EXT_RXF_SUPPORTED \
	(ANXD_S2_RXF_SUMMED | ANXD_S2_RXF_VERIFIED | ANXD_S2_RXF_CONTINUES)

/*
 * Negotiate the extension tags from one opener's buffer-management tag
 * list. `tags` is the caller's list exactly as OpenDevice received it
 * (before the driver replaces ios2_BufferManagement with its own record).
 *
 * Write-back rules (anxs2ext.h):
 *   - The direct pair is accepted only when BOTH hooks are present; a
 *     partial set disables the extensions exactly as offering no tags,
 *     and no tag pointer is written in that case.
 *   - ANXD_S2_RX_LINK_HDR: the pointed-to BOOL is set TRUE only with the
 *     pair (the direct path is the only consumer of the link header).
 *   - ANXD_S2_RX_FLAGS: the pointed-to UBYTE is replaced with the
 *     intersection of the preloaded request and the supported set; a zero
 *     preload asks for everything. Without the pair the byte is left
 *     untouched (nothing can be delivered to it).
 *   - S2_PacketFilter is recorded (not written back; it is a plain hook
 *     tag) and forces the staging path.
 *
 * Returns 1 when the direct pair was accepted.
 */
static inline int zznet_ext_negotiate(const struct zznet_tag *tags,
                                      struct zznet_ext *xe)
{
	BOOL *linkhdr_ptr = 0;
	UBYTE *flags_ptr = 0;
	UBYTE flags_preload = 0;
	void *direct = 0, *filled = 0, *filter = 0;
	const struct zznet_tag *t;

	for (t = tags; t->tag != 0; t++) {
		switch (t->tag) {
		case ANXD_S2_RX_DIRECT:
			direct = (void *)t->data;
			break;
		case ANXD_S2_RX_FILLED:
			filled = (void *)t->data;
			break;
		case ANXD_S2_RX_LINK_HDR:
			linkhdr_ptr = (BOOL *)t->data;
			break;
		case ANXD_S2_RX_FLAGS:
			flags_ptr = (UBYTE *)t->data;
			flags_preload = flags_ptr ? *flags_ptr : 0;
			break;
		case ZZNET_S2_PacketFilter:
			filter = (void *)t->data;
			break;
		default:
			break;
		}
	}

	xe->xe_PacketFilter = filter;

	if (!direct || !filled) {
		/* Partial or absent pair: extensions off, no write-backs. */
		return 0;
	}

	xe->xe_HasPair  = 1;
	xe->xe_RxDirect = direct;
	xe->xe_RxFilled = filled;

	if (linkhdr_ptr) {
		*linkhdr_ptr = TRUE;
		xe->xe_RxLinkHdr = 1;
	}

	xe->xe_RxFlags = flags_preload
		? (UBYTE)(flags_preload & ZZNET_EXT_RXF_SUPPORTED)
		: (UBYTE)ZZNET_EXT_RXF_SUPPORTED;
	if (flags_ptr) {
		*flags_ptr = xe->xe_RxFlags;
	}

	return 1;
}

/*
 * Gate the direct-receive claim for one frame.
 *
 * `takers`      — openers (including this one) that have a read of this
 *                 frame's packet type posted right now.
 * `raw`         — the winning request carries SANA2IOF_RAW.
 * `xe`          — the winning opener's negotiated state, or NULL.
 *
 * The claim is allowed only for a single direct-capable taker without a
 * packet filter on a cooked request. Every decline takes the staging path
 * (which also covers openers without the pair). See anxs2ext.h and
 * netdev_direct.c: the raw destination starts at the frame, so the link
 * header and the 14-byte-back pointer make no sense; a filter hook must
 * see the whole frame before any copy; a second taker needs its own copy.
 */
static inline int zznet_ext_can_claim(int takers, int raw,
                                      const struct zznet_ext *xe)
{
	if (!xe || !xe->xe_HasPair)      return 0;
	if (xe->xe_PacketFilter)         return 0;
	if (raw)                         return 0;
	if (takers != 1)                 return 0;
	return 1;
}

#endif /* _INC_ZZNET_EXT_H */
