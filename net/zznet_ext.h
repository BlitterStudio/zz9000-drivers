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

/* Frozen SANA-II buffer-management tag value (S2_Dummy = TAG_USER +
 * 0xB0000). Spelled out so the tests need no sana2.h; net/sana2.h
 * defines the same tag and the driver reads the copy hooks through
 * GetTagData there. */
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
/* Per-opener previous-delivered TCP record (KTD5): CONTINUES is decided
 * against the previous frame delivered to that opener — one record, not
 * a per-flow table. Cleared by any delivery that is not the
 * continuation, by a serial gap or overrun, by a bad frame, or by an
 * offline-online transition. */
struct zznet_cont {
    int   c_valid;
    UBYTE c_src[4];
    UBYTE c_dst[4];
    UWORD c_sport;
    UWORD c_dport;
    ULONG c_seq_end;   /* previous seq + payload length */
    ULONG c_ack;
    UWORD c_win;
    UBYTE c_flags;     /* predecessor's TCP flags (compared per contract) */
    UBYTE c_doff;      /* predecessor's TCP data offset (options break chains) */
};

/*
 * Negotiate the extension tags from one opener's buffer-management tag
 * list. `tags` is the caller's list exactly as OpenDevice received it
 * (before the driver replaces ios2_BufferManagement with its own
 * record).
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
	const struct zznet_tag *t = tags;

	/* Standard TagItem traversal semantics (utility.library rules):
	 * TAG_DONE ends the list, TAG_SKIP skips ti_Data further items,
	 * TAG_IGNORE is a no-op, and TAG_MORE continues at the pointer in
	 * ti_Data. A plain linear walk would read skipped items as live and
	 * run past a chained list's end into unrelated memory. */
	while (t && t->tag != 0 /* TAG_DONE */) {
		switch (t->tag) {
		case 2 /* TAG_SKIP */:
			t += (zznet_tag_data)t->data + 1;
			continue;
		case 3 /* TAG_IGNORE */:
			break;
		case 4 /* TAG_MORE */:
			t = (const struct zznet_tag *)(zznet_tag_data)t->data;
			continue;
		case ANXD_S2_RX_DIRECT:
			direct = (void *)(zznet_tag_data)t->data;
			break;
		case ANXD_S2_RX_FILLED:
			filled = (void *)(zznet_tag_data)t->data;
			break;
		case ANXD_S2_RX_LINK_HDR:
			linkhdr_ptr = (BOOL *)(zznet_tag_data)t->data;
			break;
		case ANXD_S2_RX_FLAGS:
			flags_ptr = (UBYTE *)(zznet_tag_data)t->data;
			flags_preload = flags_ptr ? *flags_ptr : 0;
			break;
		case ZZNET_S2_PacketFilter:
			filter = (void *)(zznet_tag_data)t->data;
			break;
		default:
			break;
		}
		t++;
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

	/* anxs2ext.h: "Without the tag a device sets SUMMED alone, whatever
	 * it could have said." A present tag with zero preload asks for
	 * everything supported ("A zero input retains the first published
	 * contract and asks for every flag the device supports"); a present
	 * preload yields the intersection. */
	if (!flags_ptr) {
		xe->xe_RxFlags = ANXD_S2_RXF_SUMMED;
	} else {
		xe->xe_RxFlags = flags_preload
			? (UBYTE)(flags_preload & ZZNET_EXT_RXF_SUPPORTED)
			: (UBYTE)ZZNET_EXT_RXF_SUPPORTED;
		/* Published rule: "CONTINUES requires VERIFIED; an opener may
		 * request VERIFIED alone." A preload of CONTINUES without
		 * VERIFIED is normalized down to what the contract allows,
		 * so RX_FILLED never reports an invalid combination. */
		if (!(xe->xe_RxFlags & ANXD_S2_RXF_VERIFIED)) {
			xe->xe_RxFlags &= (UBYTE)~ANXD_S2_RXF_CONTINUES;
		}
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


/* ---- VERIFIED and CONTINUES (KTD5) ------------------------------------
 *
 * zznet_rx_flags computes the flags the driver may set in RX_FILLED's
 * last argument for a delivered cooked IPv4 payload. `p` points at the
 * IP header in RAM (the direct-drain destination — the driver reads the
 * header region back from Fast RAM, near-free next to the MMIO drain);
 * `len` is the delivered payload length INCLUDING any Ethernet padding;
 * `cont` is the opener's previous-delivered record; `update` replaces
 * the record when this frame qualifies as a new predecessor.
 *
 * VERIFIED requires: IPv4, no options (ihl == 5), not a fragment, no
 * Ethernet padding past the IP total length, a correct IP header
 * checksum, and a correct TCP or UDP checksum (UDP zero checksum
 * excluded). CONTINUES additionally requires the previous delivered
 * frame to be the same stream at exactly the previous end sequence,
 * with the same ACK, window, and ACK-or-ACK+PSH flags, no TCP options,
 * both VERIFIED.
 *
 * Returns SUMMED plus whatever was earned. `cont` is replaced only
 * when this frame qualifies (update never partially merges). */

static UWORD zznet_cksum_fold(ULONG sum)
{
	while (sum >> 16)
		sum = (sum & 0xFFFF) + (sum >> 16);
	return (UWORD)sum;
}

static ULONG zznet_cksum_add_bytes(const UBYTE *p, ULONG n)
{
	ULONG sum = 0;
	while (n >= 2) {
		sum += ((ULONG)p[0] << 8) | p[1];
		p += 2; n -= 2;
	}
	if (n)
		sum += (ULONG)p[0] << 8;
	return sum;
}

static inline UBYTE zznet_rx_flags(const UBYTE *p, ULONG len,
                            struct zznet_cont *cont, int update)
{
	ULONG ip_hl, ip_total, trans_off, trans_len;
	ULONG ph_sum;
	UWORD frag, udpck, udp_len, doff;
	int is_tcp = 0;
	UBYTE out = ANXD_S2_RXF_SUMMED;

	/* KTD5 + the published contract: the record describes the frame
	 * delivered IMMEDIATELY before this one, so every delivery of this
	 * frame ends its validity — the update block below re-establishes
	 * it only when this frame qualifies as a verified options-free TCP
	 * segment. Early returns must clear it too (the frame was still
	 * delivered to the opener), hence the macro instead of plain
	 * returns. */
#define ZZNET_RXF_RETURN() \
	do { if (update && cont) cont->c_valid = 0; return out; } while (0)

	if (len < 20)                        ZZNET_RXF_RETURN();


	if ((p[0] >> 4) != 4)                ZZNET_RXF_RETURN();
	ip_hl = (ULONG)(p[0] & 0x0f) * 4;
	if (ip_hl != 20)                     ZZNET_RXF_RETURN(); /* options excluded */
	ip_total = ((ULONG)p[2] << 8) | p[3];
	if (ip_total != len)                 ZZNET_RXF_RETURN(); /* torn or padded */
	if (ip_total < 20)                   ZZNET_RXF_RETURN();
	frag = ((UWORD)p[6] << 8) | p[7];
	if (frag & 0x3fff)                   ZZNET_RXF_RETURN(); /* fragment (MF|offset) */
	if (p[9] != 6 && p[9] != 17)         ZZNET_RXF_RETURN();

	/* IP header checksum: sum of the 20 header bytes (checksum field
	 * included) must fold to 0xFFFF. */
	if (zznet_cksum_fold(zznet_cksum_add_bytes(p, 20)) != 0xFFFF)
		ZZNET_RXF_RETURN();

	trans_off = ip_hl;
	trans_len = ip_total - ip_hl;
	if (p[9] == 17) {
		if (trans_len < 8)               ZZNET_RXF_RETURN();
		udpck  = ((UWORD)p[trans_off + 6] << 8) | p[trans_off + 7];
		udp_len = ((UWORD)p[trans_off + 4] << 8) | p[trans_off + 5];
		if (udpck == 0)                  ZZNET_RXF_RETURN(); /* UDP zero csum */
		if (udp_len < 8 || udp_len > trans_len)
			ZZNET_RXF_RETURN();                   /* UDP length beyond frame */
		trans_len = udp_len;              /* checksum covers UDP length */
	} else {
		if (trans_len < 20)              ZZNET_RXF_RETURN();
		doff = (UWORD)(p[trans_off + 12] >> 4) * 4;
		if (doff < 20 || doff > trans_len) ZZNET_RXF_RETURN();
		is_tcp = 1;
	}

	/* Transport checksum: ones-complement sum of pseudo-header plus the
	 * whole transport segment (its checksum field included) folds to
	 * 0xFFFF — same property as the IP header. */
	ph_sum = zznet_cksum_add_bytes(p + 12, 8);          /* src + dst IP */
	ph_sum += (ULONG)p[9];                              /* zero byte + protocol */
	ph_sum += trans_len;
	{
		ULONG seg = zznet_cksum_fold(zznet_cksum_add_bytes(p + trans_off, trans_len));
		if (zznet_cksum_fold(ph_sum + seg) != 0xFFFF)
			ZZNET_RXF_RETURN();
	}

	out |= ANXD_S2_RXF_VERIFIED;

	if (is_tcp && cont && cont->c_valid) {
		UWORD sport = ((UWORD)p[20] << 8) | p[21];
		UWORD dport = ((UWORD)p[22] << 8) | p[23];
		ULONG seq   = ((ULONG)p[24] << 24) | ((ULONG)p[25] << 16) |
		              ((ULONG)p[26] << 8)  | (ULONG)p[27];
		ULONG ack   = ((ULONG)p[28] << 24) | ((ULONG)p[29] << 16) |
		              ((ULONG)p[30] << 8)  | (ULONG)p[31];
		UWORD win   = ((UWORD)p[34] << 8) | p[35];
		UWORD doff2 = (UWORD)(p[32] >> 4) * 4;
		UBYTE tfl   = p[33];

		/* Published conditions: same flags (ACK or ACK+PSH) and no TCP
		 * options on BOTH sides — c_doff records the predecessor's. */
		if ((tfl == 0x10 || tfl == 0x18) &&
		    cont->c_flags == tfl &&
		    doff2 == 20 && cont->c_doff == 20 &&
		    cont->c_seq_end == seq &&
		    cont->c_ack == ack &&
		    cont->c_win == win &&
		    cont->c_sport == sport && cont->c_dport == dport &&
		    cont->c_src[0] == p[12] && cont->c_src[1] == p[13] &&
		    cont->c_src[2] == p[14] && cont->c_src[3] == p[15] &&
		    cont->c_dst[0] == p[16] && cont->c_dst[1] == p[17] &&
		    cont->c_dst[2] == p[18] && cont->c_dst[3] == p[19]) {
			out |= ANXD_S2_RXF_CONTINUES;
		}
	}

	if (update && cont) {
		/* Every delivery ends the old record's validity (the entry
		 * macro handles rejected frames); only this block decides
		 * whether a NEW record is established. A verified UDP
		 * delivery reaches here without re-establishing anything, so
		 * the explicit else-clear is what invalidates the stale TCP
		 * record — without it the next TCP segment would chain
		 * across the intervening UDP frame. */
		if (p[9] == 6) {
			UWORD doff2 = (UWORD)(p[32] >> 4) * 4;
			ULONG seq   = ((ULONG)p[24] << 24) | ((ULONG)p[25] << 16) |
			              ((ULONG)p[26] << 8)  | (ULONG)p[27];
			cont->c_valid = (doff2 == 20) ? 1 : 0;
			cont->c_doff  = (UBYTE)doff2;
			cont->c_src[0] = p[12]; cont->c_src[1] = p[13];
			cont->c_src[2] = p[14]; cont->c_src[3] = p[15];
			cont->c_dst[0] = p[16]; cont->c_dst[1] = p[17];
			cont->c_dst[2] = p[18]; cont->c_dst[3] = p[19];
			cont->c_sport = ((UWORD)p[20] << 8) | p[21];
			cont->c_dport = ((UWORD)p[22] << 8) | p[23];
			cont->c_seq_end = seq + (trans_len - doff2);
			cont->c_ack = ((ULONG)p[28] << 24) | ((ULONG)p[29] << 16) |
			              ((ULONG)p[30] << 8)  | (ULONG)p[31];
			cont->c_win = ((UWORD)p[34] << 8) | p[35];
			cont->c_flags = p[33];
		} else {
			/* Verified non-TCP (UDP): delivered, so it is the new
			 * immediate predecessor — but never a valid one. */
			cont->c_valid = 0;
		}
	}

	return out;
#undef ZZNET_RXF_RETURN
}

#endif /* _INC_ZZNET_EXT_H */
