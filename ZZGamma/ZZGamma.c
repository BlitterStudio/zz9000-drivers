/*
 * ZZGamma - live output gamma control for the ZZ9000 (issue #130).
 *
 * Usage: ZZGamma [<value>|OFF]
 *
 *   value : output gamma 0.20-5.00 with at most two decimals
 *           (1.8, 0.75, 2). Values above 1.00 brighten the picture,
 *           values below 1.00 darken it.
 *   OFF   : return to 1.00 (identity, filter off).
 *
 * Without an argument the current setting is printed.
 *
 * Support is detected through the ZZ9000.CFG query interface: key 35
 * returns the runtime applied gamma, 0 when the firmware or the loaded
 * bitstream has no gamma LUT. Applying a value writes the hundredths to
 * the dedicated write-only ZZ_REG_GAMMA register and reads key 35 back.
 * It deliberately avoids the USER1 feature-toggle path, whose selector
 * the RTG driver caches for its blits.
 *
 * The live setting is not persisted: to keep it, write
 * gamma = x.xx to ZZ9000.CFG, or set it on ZZTop's Display page and Save.
 *
 * Build: m68k-amigaos-gcc -O2 -noixemul -I../include
 *        -o ZZGamma ZZGamma.c -lamiga
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <libraries/expansion.h>
#include <libraries/expansionbase.h>
#include <proto/exec.h>
#include <proto/expansion.h>
#include <proto/dos.h>
#include <stdio.h>

#include "zz9000_hw.h"
#include "zzcfg_query.h"
#include "zz9000_gamma.h"

static const char version[] __attribute__((used)) =
    "$VER: ZZGamma 1.0 (28.10.2026)\r\n";

static void print_gamma(UWORD hundredths)
{
    printf("Gamma %u.%02u%s\n", (unsigned)(hundredths / 100),
        (unsigned)(hundredths % 100),
        hundredths == ZZGAMMA_HUNDREDTHS_IDENTITY ? " (off)" : "");
}

int main(int argc, char *argv[])
{
    ULONG board_addr;
    UWORD applied;
    int hundredths = ZZGAMMA_HUNDREDTHS_IDENTITY;

    if (argc > 2 || (argc == 2 && argv[1][0] == '?')) {
        printf("Usage: %s [<value>|OFF]\n", argv[0]);
        printf("  value : gamma 0.20-5.00, e.g. 1.8, 0.75 or 2\n");
        printf("  OFF   : return to 1.00 (gamma filter off)\n");
        printf("No argument prints the current gamma.\n");
        return argc > 2 ? RETURN_ERROR : RETURN_OK;
    }

    if (argc == 2 && !zzgamma_parse(argv[1], &hundredths)) {
        printf("ERROR: value must be 0.20-5.00 (e.g. 1.8) or OFF\n");
        return RETURN_ERROR;
    }

    board_addr = zz9000_find_board(NULL);
    if (!board_addr) { printf("ERROR: ZZ9000 not found\n"); return RETURN_ERROR; }

    printf("ZZ9000 found at 0x%08lx\n", (unsigned long)board_addr);

    /* Key 35 reports the applied gamma: 0 means old firmware or a
     * bitstream without the LUT stage. */
    applied = zzcfg_query(board_addr, ZZ_CFG_KEY_GAMMA, NULL);
    if (!applied) {
        printf("Gamma filter not available: the firmware or the loaded "
               "bitstream has no gamma LUT\n");
        return RETURN_WARN;
    }

    if (argc == 2) {
        zz9000_write_reg16(board_addr, ZZ_REG_GAMMA, (UWORD)hundredths);

        applied = zzcfg_query(board_addr, ZZ_CFG_KEY_GAMMA, NULL);
        if (!applied) {
            printf("ERROR: firmware rejected the gamma value\n");
            return RETURN_ERROR;
        }
        print_gamma(applied);
        printf("Note: live only - add 'gamma = %u.%02u' to ZZ9000.CFG "
               "to keep it after power-cycle\n",
            (unsigned)(applied / 100), (unsigned)(applied % 100));
    } else {
        print_gamma(applied);
    }

    return RETURN_OK;
}
