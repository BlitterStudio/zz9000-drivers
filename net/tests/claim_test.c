/*
 * claim_test.c — host-side tests for the AmiNetXDuo SANA-II extension
 * negotiation and direct-claim gating (zznet_ext.h).
 *
 * Mirrors the scenarios in docs/plans/2026-09-20-1201-perf-zz9000net-throughput-plan.md
 * unit U1: tag-pair validation, pointer write-back semantics, flag
 * intersection, and the claim decline rules (multi-taker, packet filter,
 * raw request, non-negotiating opener).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "zznet_ext.h"

static int failures = 0;

#define CHECK(cond, name) do { \
    if (cond) { printf("ok   %s\n", (name)); } \
    else      { printf("FAIL %s\n", (name)); failures++; } \
} while (0)

/* Sentinel values proving the driver never writes through a tag pointer
 * it did not accept (AE1: non-negotiating openers see untouched memory). */
#define SENTINEL_BOOL 0x5A
#define SENTINEL_UBYTE 0xA5

static void tags_init(struct zznet_tag *tags, size_t max) {
    memset(tags, 0, max * sizeof(*tags));
}

int main(void) {
    struct zznet_tag tags[8];
    struct zznet_ext xe;

    /* --- 1. RX_FILLED without RX_DIRECT: no extensions, no write-backs --- */
    {
        BOOL linkhdr = SENTINEL_BOOL;
        UBYTE rxflags = SENTINEL_UBYTE;

        tags_init(tags, 8);
        tags[0].tag = ANXD_S2_RX_FILLED;  tags[0].data = 0xDEAD0001;
        tags[1].tag = ANXD_S2_RX_LINK_HDR; tags[1].data = (zznet_tag_data)&linkhdr;
        tags[2].tag = ANXD_S2_RX_FLAGS;   tags[2].data = (zznet_tag_data)&rxflags;
        /* terminator: tag 0 == TAG_DONE */

        memset(&xe, 0, sizeof(xe));
        zznet_ext_negotiate(tags, &xe);

        CHECK(xe.xe_HasPair == 0, "pair: fill-without-direct declines");
        CHECK(xe.xe_RxDirect == NULL && xe.xe_RxFilled == NULL, "pair: hooks not captured");
        CHECK(linkhdr == SENTINEL_BOOL, "pair: BOOL write-back suppressed");
        CHECK(rxflags == SENTINEL_UBYTE, "pair: flags byte write-back suppressed");
        CHECK(xe.xe_RxLinkHdr == 0, "pair: link-header not accepted");
        CHECK(xe.xe_RxFlags == 0, "pair: no flag intersection recorded");
    }

    /* --- 2. RX_DIRECT without RX_FILLED: symmetric decline --- */
    {
        tags_init(tags, 8);
        tags[0].tag = ANXD_S2_RX_DIRECT; tags[0].data = 0xDEAD0002;

        memset(&xe, 0, sizeof(xe));
        zznet_ext_negotiate(tags, &xe);
        CHECK(xe.xe_HasPair == 0, "pair: direct-without-fill declines");
    }

    /* --- 3. Full pair: accepted, hooks captured, BOOL set TRUE --- */
    {
        BOOL linkhdr = 0;

        tags_init(tags, 8);
        tags[0].tag = ANXD_S2_RX_DIRECT;  tags[0].data = 0x1000;
        tags[1].tag = ANXD_S2_RX_FILLED;  tags[1].data = 0x2000;
        tags[2].tag = ANXD_S2_RX_LINK_HDR; tags[2].data = (zznet_tag_data)&linkhdr;

        memset(&xe, 0, sizeof(xe));
        zznet_ext_negotiate(tags, &xe);

        CHECK(xe.xe_HasPair == 1, "pair: direct+fill accepted");
        CHECK(xe.xe_RxDirect == (void *)0x1000, "pair: direct hook captured");
        CHECK(xe.xe_RxFilled == (void *)0x2000, "pair: fill hook captured");
        CHECK(linkhdr == TRUE, "pair: link-header BOOL set TRUE");
        CHECK(xe.xe_RxLinkHdr == 1, "pair: link-header recorded");
    }

    /* --- 4. LINK_HDR without the pair: ignored entirely (scenario 2) --- */
    {
        BOOL linkhdr = SENTINEL_BOOL;

        tags_init(tags, 8);
        tags[0].tag = ANXD_S2_RX_LINK_HDR; tags[0].data = (zznet_tag_data)&linkhdr;

        memset(&xe, 0, sizeof(xe));
        zznet_ext_negotiate(tags, &xe);
        CHECK(linkhdr == SENTINEL_BOOL, "linkhdr-without-pair: BOOL untouched");
    }

    /* --- 5. FLAGS preloaded with VERIFIED|CONTINUES: intersection --- */
    {
        UBYTE rxflags = ANXD_S2_RXF_VERIFIED | ANXD_S2_RXF_CONTINUES;

        tags_init(tags, 8);
        tags[0].tag = ANXD_S2_RX_DIRECT; tags[0].data = 0x1000;
        tags[1].tag = ANXD_S2_RX_FILLED; tags[1].data = 0x2000;
        tags[2].tag = ANXD_S2_RX_FLAGS;  tags[2].data = (zznet_tag_data)&rxflags;

        memset(&xe, 0, sizeof(xe));
        zznet_ext_negotiate(tags, &xe);
        CHECK(rxflags == (ANXD_S2_RXF_VERIFIED | ANXD_S2_RXF_CONTINUES),
              "flags: preloaded subset returned as intersection");
        CHECK(xe.xe_RxFlags == (ANXD_S2_RXF_VERIFIED | ANXD_S2_RXF_CONTINUES),
              "flags: intersection recorded");
    }

    /* --- 6. FLAGS preloaded 0: asks for everything supported --- */
    {
        UBYTE rxflags = 0;

        tags_init(tags, 8);
        tags[0].tag = ANXD_S2_RX_DIRECT; tags[0].data = 0x1000;
        tags[1].tag = ANXD_S2_RX_FILLED; tags[1].data = 0x2000;
        tags[2].tag = ANXD_S2_RX_FLAGS;  tags[2].data = (zznet_tag_data)&rxflags;

        memset(&xe, 0, sizeof(xe));
        zznet_ext_negotiate(tags, &xe);
        CHECK(rxflags == (ANXD_S2_RXF_SUMMED | ANXD_S2_RXF_VERIFIED |
                          ANXD_S2_RXF_CONTINUES),
              "flags: zero preload returns full support set");
    }

    /* --- 7. Packet filter captured, forces staging --- */
    {
        tags_init(tags, 8);
        tags[0].tag = ZZNET_S2_PacketFilter; tags[0].data = 0x3000;
        tags[1].tag = ANXD_S2_RX_DIRECT;     tags[1].data = 0x1000;
        tags[2].tag = ANXD_S2_RX_FILLED;     tags[2].data = 0x2000;

        memset(&xe, 0, sizeof(xe));
        zznet_ext_negotiate(tags, &xe);
        CHECK(xe.xe_PacketFilter == (void *)0x3000, "filter: S2_PacketFilter captured");
        CHECK(zznet_ext_can_claim(1 /*takers*/, 0 /*raw*/, &xe) == 0,
              "filter: claim declined for filtered opener");
    }

    /* --- 8. Claim decline matrix --- */
    {
        struct zznet_ext full;
        memset(&full, 0, sizeof(full));
        full.xe_HasPair = 1;
        full.xe_RxDirect = (void *)0x1000;
        full.xe_RxFilled = (void *)0x2000;

        CHECK(zznet_ext_can_claim(1, 0, &full) == 1, "claim: single taker accepted");
        CHECK(zznet_ext_can_claim(2, 0, &full) == 0, "claim: two takers decline (staging for both)");
        CHECK(zznet_ext_can_claim(1, 1, &full) == 0, "claim: raw request declines");
        CHECK(zznet_ext_can_claim(1, 0, NULL) == 0, "claim: opener without pair declines");
    }

    /* --- 9. Empty tag list (AE1: non-negotiating opener) --- */
    {
        tags_init(tags, 8); /* just the terminator */

        memset(&xe, 0, sizeof(xe));
        zznet_ext_negotiate(tags, &xe);
        CHECK(xe.xe_HasPair == 0 && xe.xe_RxFlags == 0 &&
              xe.xe_PacketFilter == NULL,
              "AE1: no tags -> no extensions, no side effects");
        CHECK(zznet_ext_can_claim(1, 0, &xe) == 0, "AE1: claim declined without pair");
    }

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
