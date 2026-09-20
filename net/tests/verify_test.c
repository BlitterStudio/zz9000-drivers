/*
 * verify_test.c — host-side tests for VERIFIED (checksum certification)
 * and CONTINUES (GRO continuation) computation in zznet_ext.h.
 *
 * Scenarios from the plan (U5): checksum vectors including every
 * published exclusion, continuation adjacency, interleaved flows, and
 * gap suppression.
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

/* ---- frame builders ---------------------------------------------------- */

static void put16(UBYTE *p, int off, UWORD v) {
    p[off]     = (UBYTE)(v >> 8);
    p[off + 1] = (UBYTE)(v & 0xff);
}
static void put32(UBYTE *p, int off, ULONG v) {
    p[off]     = (UBYTE)(v >> 24);
    p[off + 1] = (UBYTE)(v >> 16);
    p[off + 2] = (UBYTE)(v >> 8);
    p[off + 3] = (UBYTE)(v & 0xff);
}

/* Ones-complement checksum of a byte range. */
static UWORD csum(const UBYTE *p, int len) {
    ULONG sum = 0;
    int i;
    for (i = 0; i + 1 < len; i += 2)
        sum += ((ULONG)p[i] << 8) | p[i + 1];
    if (len & 1)
        sum += (ULONG)p[len - 1] << 8;
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    return (UWORD)~sum;
}

/* Build a cooked IPv4/TCP or UDP packet at buf. Returns its length. */
static int build_ip_tcp(UBYTE *buf, int tcp_payload_len, UWORD flags,
                        ULONG seq, ULONG ack, UWORD win, int data_offset_words)
{
    int ip_hl = 20;
    int tcp_hl = data_offset_words * 4;
    int total = ip_hl + tcp_hl + tcp_payload_len;

    memset(buf, 0, total);
    buf[0] = 0x45;                 /* v4, ihl 5 */
    put16(buf, 2, (UWORD)total);   /* total length */
    buf[8] = 64;                   /* TTL */
    buf[9] = 6;                    /* TCP */
    put32(buf, 12, 0x0a000001);    /* src */
    put32(buf, 16, 0x0a000002);    /* dst */

    /* TCP header at 20 */
    put16(buf, 20, 1234);          /* sport */
    put16(buf, 22, 80);            /* dport */
    put32(buf, 24, seq);
    put32(buf, 28, ack);
    buf[32] = (UBYTE)(data_offset_words << 4);
    buf[33] = (UBYTE)flags;
    put16(buf, 34, win);

    /* payload */
    {
        int i;
        for (i = 0; i < tcp_payload_len; i++)
            buf[ip_hl + tcp_hl + i] = (UBYTE)(0x40 + i);
    }

    /* TCP checksum: pseudo-header + segment */
    {
        UBYTE ph[12];
        ULONG sum;
        put32(ph, 0, 0x0a000001);
        put32(ph, 4, 0x0a000002);
        ph[8] = 0; ph[9] = 6;
        put16(ph, 10, (UWORD)(total - ip_hl));
        sum = ((ULONG)csum(ph, 12) << 16) >> 16; /* keep 16-bit */
        /* combine via ones-complement addition of both complements */
        {
            UWORD phc = csum(ph, 12);            /* complement of pseudo sum */
            UWORD segc = csum(buf + ip_hl, total - ip_hl); /* compl. of segment (csum field 0) */
            ULONG s2 = (ULONG)phc + (ULONG)segc;
            while (s2 >> 16) s2 = (s2 & 0xffff) + (s2 >> 16);
            put16(buf, ip_hl + 16, (UWORD)s2);   /* TCP checksum: s2 is already the complement */
        }
        (void)sum;
    }
    put16(buf, 10, csum(buf, ip_hl)); /* IP header checksum */
    return total;
}

int main(void) {
    UBYTE pkt[1600];
    struct zznet_cont cont;
    UBYTE flags;
    int len;

    /* --- 1. Valid TCP frame verifies --- */
    len = build_ip_tcp(pkt, 16, 0x10 /*ACK*/, 1000, 5, 8000, 5);
    memset(&cont, 0, sizeof(cont));
    flags = zznet_rx_flags(pkt, (ULONG)len, &cont, 1 /*update record*/);
    CHECK(flags & ANXD_S2_RXF_VERIFIED, "valid TCP sets VERIFIED");
    CHECK(flags & ANXD_S2_RXF_SUMMED, "SUMMED always set");
    CHECK(!(flags & ANXD_S2_RXF_CONTINUES), "no predecessor: no CONTINUES");
    CHECK(cont.c_valid, "record updated");

    /* --- 2. Corrupted payload: no VERIFIED --- */
    pkt[40] ^= 0xff;
    memset(&cont, 0, sizeof(cont));
    flags = zznet_rx_flags(pkt, (ULONG)len, &cont, 1);
    CHECK(!(flags & ANXD_S2_RXF_VERIFIED), "corrupted TCP clears VERIFIED");
    CHECK(!cont.c_valid, "no record for unverified frame");
    pkt[40] ^= 0xff;

    /* --- 3. IP options (ihl 6): never VERIFIED --- */
    {
        /* Rebuild then patch ihl to 6 with options bytes present. */
        len = build_ip_tcp(pkt, 8, 0x10, 1, 1, 100, 5);
        memmove(pkt + 24, pkt + 20, len - 20); /* shift TCP right by 4 */
        len += 4;
        pkt[0] = 0x46;
        put16(pkt, 2, (UWORD)len);
        memset(pkt + 20, 0, 4); /* NOP options */
        put16(pkt, 10, csum(pkt, 24));
        memset(&cont, 0, sizeof(cont));
        flags = zznet_rx_flags(pkt, (ULONG)len, &cont, 1);
        CHECK(!(flags & ANXD_S2_RXF_VERIFIED), "IP options exclude VERIFIED");
    }

    /* --- 4. Fragment: never VERIFIED --- */
    len = build_ip_tcp(pkt, 8, 0x10, 1, 1, 100, 5);
    put16(pkt, 6, 0x2001); /* MF set + offset 1 */
    put16(pkt, 10, csum(pkt, 20));
    memset(&cont, 0, sizeof(cont));
    flags = zznet_rx_flags(pkt, (ULONG)len, &cont, 1);
    CHECK(!(flags & ANXD_S2_RXF_VERIFIED), "fragment excludes VERIFIED");

    /* --- 5. Ethernet padding past IP total: never VERIFIED --- */
    len = build_ip_tcp(pkt, 8, 0x10, 1, 1, 100, 5);
    memset(&cont, 0, sizeof(cont));
    flags = zznet_rx_flags(pkt, (ULONG)len + 6 /* pretend padded */, &cont, 1);
    CHECK(!(flags & ANXD_S2_RXF_VERIFIED), "padding excludes VERIFIED");

    /* --- 6. Continuation chain --- */
    len = build_ip_tcp(pkt, 16, 0x10, 1000, 5, 8000, 5);
    memset(&cont, 0, sizeof(cont));
    (void)zznet_rx_flags(pkt, (ULONG)len, &cont, 1);        /* first */
    build_ip_tcp(pkt, 16, 0x10, 1000 + 16, 5, 8000, 5);     /* next seq */
    flags = zznet_rx_flags(pkt, (ULONG)len, &cont, 1);
    CHECK(flags & ANXD_S2_RXF_CONTINUES, "seq-continuous segment chains");

    /* Retransmit (same seq): no CONTINUES. */
    memset(&cont, 0, sizeof(cont));
    (void)zznet_rx_flags(pkt, (ULONG)len, &cont, 1);
    flags = zznet_rx_flags(pkt, (ULONG)len, &cont, 1);
    CHECK(!(flags & ANXD_S2_RXF_CONTINUES), "retransmit does not chain");

    /* Window change: no CONTINUES. */
    memset(&cont, 0, sizeof(cont));
    build_ip_tcp(pkt, 16, 0x10, 1000, 5, 8000, 5);
    (void)zznet_rx_flags(pkt, (ULONG)len, &cont, 1);
    build_ip_tcp(pkt, 16, 0x10, 1016, 5, 4000, 5);
    flags = zznet_rx_flags(pkt, (ULONG)len, &cont, 1);
    CHECK(!(flags & ANXD_S2_RXF_CONTINUES), "window change breaks chain");

    /* Flags change (ACK -> ACK+PSH is allowed; ACK -> SYN is not). */
    memset(&cont, 0, sizeof(cont));
    build_ip_tcp(pkt, 16, 0x10, 1000, 5, 8000, 5);
    (void)zznet_rx_flags(pkt, (ULONG)len, &cont, 1);
    build_ip_tcp(pkt, 16, 0x02 /*SYN*/, 1016, 5, 8000, 5);
    flags = zznet_rx_flags(pkt, (ULONG)len, &cont, 1);
    CHECK(!(flags & ANXD_S2_RXF_CONTINUES), "flags change breaks chain");

    /* ACK+PSH continuation is allowed. */
    memset(&cont, 0, sizeof(cont));
    build_ip_tcp(pkt, 16, 0x10, 1000, 5, 8000, 5);
    (void)zznet_rx_flags(pkt, (ULONG)len, &cont, 1);
    build_ip_tcp(pkt, 16, 0x18 /*ACK+PSH*/, 1016, 5, 8000, 5);
    flags = zznet_rx_flags(pkt, (ULONG)len, &cont, 1);
    CHECK(flags & ANXD_S2_RXF_CONTINUES, "ACK+PSH chain allowed");

    /* TCP options (data offset 6): no CONTINUES. */
    memset(&cont, 0, sizeof(cont));
    build_ip_tcp(pkt, 16, 0x10, 1000, 5, 8000, 5);
    (void)zznet_rx_flags(pkt, (ULONG)len, &cont, 1);
    build_ip_tcp(pkt, 12, 0x10, 1016, 5, 8000, 6 /*options*/);
    flags = zznet_rx_flags(pkt, (ULONG)len, &cont, 1);
    CHECK(!(flags & ANXD_S2_RXF_CONTINUES), "TCP options break chain");

    /* Interleaved flow: same opener, different stream — replaces record,
     * no CONTINUES on the second. */
    memset(&cont, 0, sizeof(cont));
    build_ip_tcp(pkt, 16, 0x10, 1000, 5, 8000, 5);
    (void)zznet_rx_flags(pkt, (ULONG)len, &cont, 1);
    {
        UBYTE other[1600];
        int olen = build_ip_tcp(other, 16, 0x10, 9000, 7, 9000, 5);
        (void)zznet_rx_flags(other, (ULONG)olen, &cont, 1); /* other flow */
    }
    build_ip_tcp(pkt, 16, 0x10, 1016, 5, 8000, 5);
    flags = zznet_rx_flags(pkt, (ULONG)len, &cont, 1);
    CHECK(!(flags & ANXD_S2_RXF_CONTINUES),
          "interleaved flow breaks chain (previous replaced)");

    /* --- 7. Non-IPv4 / short frames: nothing set, no crash --- */
    memset(pkt, 0, 32);
    memset(&cont, 0, sizeof(cont));
    flags = zznet_rx_flags(pkt, 32, &cont, 1);
    CHECK(!(flags & (ANXD_S2_RXF_VERIFIED | ANXD_S2_RXF_CONTINUES)),
          "non-IPv4 sets nothing");

    /* --- 8. Valid UDP frame verifies --- */
    {
        int ip_hl = 20, udp_hl = 8, plen = 12, total = ip_hl + udp_hl + plen;
        memset(pkt, 0, total);
        pkt[0] = 0x45;
        put16(pkt, 2, (UWORD)total);
        pkt[8] = 64; pkt[9] = 17; /* UDP */
        put32(pkt, 12, 0x0a000001);
        put32(pkt, 16, 0x0a000002);
        put16(pkt, 20, 5353); put16(pkt, 22, 5353);
        put16(pkt, 24, (UWORD)(udp_hl + plen));
        {
            UBYTE ph[12];
            UWORD phc, segc;
            ULONG s2;
            put32(ph, 0, 0x0a000001);
            put32(ph, 4, 0x0a000002);
            ph[8] = 0; ph[9] = 17;
            put16(ph, 10, (UWORD)(udp_hl + plen));
            phc = csum(ph, 12);
            segc = csum(pkt + ip_hl, udp_hl + plen);
            s2 = (ULONG)phc + (ULONG)segc;
            while (s2 >> 16) s2 = (s2 & 0xffff) + (s2 >> 16);
            put16(pkt, ip_hl + 6, (UWORD)s2);
        }
        put16(pkt, 10, csum(pkt, ip_hl));
        memset(&cont, 0, sizeof(cont));
        flags = zznet_rx_flags(pkt, (ULONG)total, &cont, 1);
        CHECK(flags & ANXD_S2_RXF_VERIFIED, "valid UDP sets VERIFIED");

        /* UDP zero checksum: excluded. */
        put16(pkt, ip_hl + 6, 0);
        memset(&cont, 0, sizeof(cont));
        flags = zznet_rx_flags(pkt, (ULONG)total, &cont, 1);
        CHECK(!(flags & ANXD_S2_RXF_VERIFIED), "UDP zero checksum excluded");
    }

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
