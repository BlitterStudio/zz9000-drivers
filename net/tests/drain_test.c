/*
 * drain_test.c — host-side tests for the single-copy direct drain's
 * published ones-complement sum (zznet_mmio.h, KTD4).
 *
 * Scenarios from the plan (U4): known buffers drain to the sum the
 * reference n68k_port_in_l_sum walk produces — including 1-, 2- and
 * 3-byte tails, the 1-mod-4 lone-byte payload, and end-around carries —
 * with a byte-exact copy.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "zznet_mmio.h"

static int failures = 0;

#define CHECK(cond, name) do { \
    if (cond) { printf("ok   %s\n", (name)); } \
    else      { printf("FAIL %s\n", (name)); failures++; } \
} while (0)

/* Reference walk: big-endian longwords over the delivered bytes, last
 * word zero-padded, end-around carry per longword — the published
 * n68k_port_in_l_sum semantics. */
static ULONG ref_sum(const UBYTE *p, ULONG n)
{
    ULONG sum = 0, v;

    while (n) {
        int cnt = n < 4 ? (int)n : 4;
        int j;
        v = 0;
        for (j = 0; j < cnt; j++)
            v = (v << 8) | p[j];
        if (cnt < 4)
            v <<= (4 - cnt) * 8; /* tail bytes occupy the high bytes */
        sum += v;
        if (sum < v) sum++;
        p += cnt;
        n -= (ULONG)cnt;
    }
    return sum;
}

int main(void)
{
    static UBYTE src[32];
    static UBYTE dst[32];
    int i;

    for (i = 0; i < 32; i++)
        src[i] = (UBYTE)(i * 37 + 11);

    /* Lengths 1..17 cover every tail class (0-3 mod 4) several times,
     * including the 1-mod-4 payloads where the lone tail byte sits at
     * the top of the final longword (bits 31-24). */
    for (i = 1; i <= 17; i++) {
        char sum_name[64], copy_name[64];
        ULONG sum;

        memset(dst, 0xAA, sizeof(dst));
        sum = zznet_mmio_read_sum((volatile UBYTE *)src, dst, (ULONG)i);
        sprintf(sum_name, "sum matches reference at length %d", i);
        CHECK(sum == ref_sum(src, (ULONG)i), sum_name);
        sprintf(copy_name, "copy byte-exact at length %d", i);
        CHECK(memcmp(dst, src, i) == 0, copy_name);
    }

    /* Lone-byte payload: the byte must land at bits 31-24. */
    {
        static UBYTE one[4] = { 0x42, 0, 0, 0 };
        static UBYTE one_dst[4];
        CHECK(zznet_mmio_read_sum((volatile UBYTE *)one, one_dst, 1) == 0x42000000UL,
              "lone byte payload sums to byte<<24");
        CHECK(one_dst[0] == 0x42, "lone byte copied");
    }

    /* End-around carry across the 0xFFFFFFFF boundary. */
    {
        static UBYTE allff[8], ff_dst[8];
        memset(allff, 0xFF, 8);
        CHECK(zznet_mmio_read_sum((volatile UBYTE *)allff, ff_dst, 8) == 0xFFFFFFFFUL,
              "end-around carry across 0xFFFFFFFF boundary");
        CHECK(zznet_mmio_read_sum((volatile UBYTE *)allff, ff_dst, 4) == 0xFFFFFFFFUL,
              "single 0xFFFFFFFF longword sums to 0xFFFFFFFF");
    }

    /* 2-mod-4 window position: the real direct path reads the payload
     * at window offset 2 mod 4; the sum must be payload-relative. */
    {
        static UBYTE win[40];
        UBYTE *payload = win + 2;
        static UBYTE w_dst[18];
        int j;

        for (j = 0; j < 18; j++)
            payload[j] = (UBYTE)(0xA0 + j);
        CHECK(zznet_mmio_read_sum((volatile UBYTE *)payload, w_dst, 18)
                  == ref_sum(payload, 18),
              "2-mod-4 src position: sum is payload-relative");
        CHECK(memcmp(w_dst, payload, 18) == 0,
              "2-mod-4 src position: copy byte-exact");
    }

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
