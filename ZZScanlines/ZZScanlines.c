/*
 * ZZScanlines v2 - Scanline mode/parity control for the ZZ9000
 * scandoubler.
 *
 * Scanlines V2 design and reference CLI by Xanxi.
 * This file is adapted from his reference implementation at
 * https://github.com/Xanxi-Amiga/zz9000-scanlines and kept essentially
 * verbatim; the drivers-repo-side adaptation is minimal.
 *
 * Usage: ZZScanlines <mode> [parity]
 *
 *   mode   : 0 = off
 *            1 = classic   (0% / 100% / 0% / 100%)
 *            2 = soft      (100% / 62% / 100% / 62%)
 *            3 = gradient  (100% / 75% / 50% / 75% / 100%)
 *   parity : 0 = odd lines dark (default)
 *            1 = even lines dark
 *
 * Active in AGA scandoubled modes and RTG resolutions below 350 lines;
 * disabled on interlaced modes and high-resolution RTG.
 *
 * Build: m68k-amigaos-gcc -O2 -noixemul -o ZZScanlines ZZScanlines.c -lamiga
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <libraries/expansion.h>
#include <libraries/expansionbase.h>
#include <proto/exec.h>
#include <proto/expansion.h>
#include <proto/dos.h>
#include <stdio.h>
#include <stdlib.h>

#define MNT_MANUFACTURER  0x6d6e
#define ZZ9000_PRODUCT_Z3 4
#define ZZ9000_PRODUCT_Z2 3

#define REG_MODE   0x100C
#define REG_PARITY 0x100E

static ULONG find_zz9000(void) {
    struct ExpansionBase *ExpBase;
    struct ConfigDev *cd = NULL;
    ULONG addr = 0;

    ExpBase = (struct ExpansionBase *)OpenLibrary("expansion.library", 0);
    if (!ExpBase) return 0;

    while ((cd = FindConfigDev(cd, MNT_MANUFACTURER, ZZ9000_PRODUCT_Z3)))
        { addr = (ULONG)cd->cd_BoardAddr; break; }

    if (!addr) {
        cd = NULL;
        while ((cd = FindConfigDev(cd, MNT_MANUFACTURER, ZZ9000_PRODUCT_Z2)))
            { addr = (ULONG)cd->cd_BoardAddr; break; }
    }

    CloseLibrary((struct Library *)ExpBase);
    return addr;
}

int main(int argc, char *argv[]) {
    if (argc < 2 || argc > 3 || argv[1][0] == '?') {
        printf("Usage: %s <mode> [parity]\n", argv[0]);
        printf("  mode   : 0=off  1=classic  2=soft  3=gradient\n");
        printf("  parity : 0=odd dark (default)  1=even dark\n");
        printf("  note   : active in AGA scandoubled modes and RTG below 350 lines\n");
        return 0;
    }

    int mode   = atoi(argv[1]);
    int parity = (argc == 3) ? atoi(argv[2]) : 0;

    if (mode < 0 || mode > 3)     { printf("ERROR: mode must be 0-3\n");     return 1; }
    if (parity < 0 || parity > 1) { printf("ERROR: parity must be 0 or 1\n"); return 1; }

    ULONG board_addr = find_zz9000();
    if (!board_addr) { printf("ERROR: ZZ9000 not found\n"); return 1; }

    printf("ZZ9000 found at 0x%08lx\n", board_addr);

    volatile UWORD * const reg_mode   = (volatile UWORD *)(board_addr + REG_MODE);
    volatile UWORD * const reg_parity = (volatile UWORD *)(board_addr + REG_PARITY);

    *reg_parity = (UWORD)parity;
    *reg_mode   = (UWORD)mode;

    if (mode == 0) {
        printf("Scanlines OFF\n");
    } else {
        const char *modes[] = {"", "classic", "soft", "gradient"};
        printf("Scanlines ON: mode=%d(%s) parity=%d\n", mode, modes[mode], parity);
    }

    return 0;
}
