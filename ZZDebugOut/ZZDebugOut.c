/*
 * ZZDebugOut - mirror exec RawPutChar output to the ZZ9000 UART.
 *
 * Usage: ZZDebugOut [OFF]
 *
 * Installs a tiny MEMF_PUBLIC m68k RawPutChar replacement.  The replacement
 * writes D0 as a 16-bit word to REG_ZZ_PRINT_CHR and returns immediately, so
 * it is safe when RawPutChar is called from interrupt, supervisor, Forbid or
 * Disable context.  It deliberately makes no Exec/DOS calls and does not
 * chain the replaced vector.  The allocated code is intentionally retained:
 * a resident patch cannot safely be freed while another task might be in it.
 */

#include <exec/memory.h>
#include <exec/types.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdio.h>
#include <string.h>

#include "zz9000_hw.h"

#define RAWPUTCHAR_LVO (-516L)
#define PATCH_WORDS 8U
#define PATCH_BYTES (PATCH_WORDS * sizeof(UWORD))
#define PATCH_MOVEW_D0_ABSL 0x33c0U
#define PATCH_RTS 0x4e75U
#define PATCH_MAGIC0 0x5a5aU
#define PATCH_MAGIC1 0x444fU

typedef ULONG (*ExecVector)(VOID);

static const char version[] __attribute__((used)) =
    "$VER: ZZDebugOut 1.0 (28.10.2026)\r\n";

static APTR rawputchar_target(void)
{
    /* exec.library vectors are JMP absolute-long instructions. */
    const UBYTE *vector = (const UBYTE *)SysBase + RAWPUTCHAR_LVO;

    return *(APTR const *)(vector + 2);
}

static int patch_is_ours(const UWORD *patch)
{
    return patch != NULL && patch[0] == PATCH_MOVEW_D0_ABSL &&
           patch[3] == PATCH_RTS && patch[4] == PATCH_MAGIC0 &&
           patch[5] == PATCH_MAGIC1;
}

static APTR patch_original(const UWORD *patch)
{
    ULONG address = ((ULONG)patch[6] << 16) | patch[7];

    return (APTR)address;
}

static void patch_set_original(UWORD *patch, APTR original)
{
    ULONG address = (ULONG)original;

    patch[6] = (UWORD)(address >> 16);
    patch[7] = (UWORD)address;
}

static UWORD *new_patch(ULONG board_addr)
{
    UWORD *patch = (UWORD *)AllocMem(PATCH_BYTES, MEMF_PUBLIC | MEMF_CLEAR);
    ULONG register_addr = board_addr + 0xf0UL;

    if (!patch)
        return NULL;
    patch[0] = PATCH_MOVEW_D0_ABSL;
    patch[1] = (UWORD)(register_addr >> 16);
    patch[2] = (UWORD)register_addr;
    patch[3] = PATCH_RTS;
    patch[4] = PATCH_MAGIC0;
    patch[5] = PATCH_MAGIC1;
    /* The 68040/060 copyback data cache may still hold the new code; push it
     * to memory and drop stale instruction-cache lines before anyone jumps
     * into it. */
    CacheClearU();
    return patch;
}

/* Send a line through the installed exec vector, so the UART shows that the
 * whole path (vector, patch, register) works before anything else uses it. */
static void raw_put_char(UBYTE c)
{
    register UBYTE d0 __asm("d0") = c;
    register struct ExecBase *a6 __asm("a6") = SysBase;

    __asm volatile("jsr -516(%%a6)"
                   :
                   : "r"(d0), "r"(a6)
                   : "d1", "a0", "a1", "cc", "memory");
}

static void raw_put_string(const char *text)
{
    while (*text)
        raw_put_char((UBYTE)*text++);
}

static int turn_off(void)
{
    UWORD *patch = (UWORD *)rawputchar_target();

    if (!patch_is_ours(patch)) {
        printf("ZZDebugOut is not installed\n");
        return RETURN_WARN;
    }
    (void)SetFunction((struct Library *)SysBase, RAWPUTCHAR_LVO,
                      (ExecVector)patch_original(patch));
    printf("ZZDebugOut disabled\n");
    return RETURN_OK;
}

static int turn_on(ULONG board_addr)
{
    UWORD *patch = (UWORD *)rawputchar_target();
    APTR original;

    if (patch_is_ours(patch)) {
        printf("ZZDebugOut is already enabled\n");
        return RETURN_OK;
    }
    patch = new_patch(board_addr);
    if (!patch) {
        printf("ERROR: cannot allocate resident patch\n");
        return RETURN_ERROR;
    }
    original = (APTR)SetFunction((struct Library *)SysBase, RAWPUTCHAR_LVO,
                                 (ExecVector)patch);
    patch_set_original(patch, original);
    CacheClearU();
    raw_put_string("\r\n[ZZDebugOut] RawPutChar now goes to the ZZ9000 UART\r\n");
    printf("ZZDebugOut enabled (RawPutChar -> ZZ9000 UART)\n");
    return RETURN_OK;
}

int main(int argc, char *argv[])
{
    ULONG board_addr;

    if (argc > 2 || (argc == 2 && strcmp(argv[1], "?") == 0)) {
        printf("Usage: %s [OFF]\n", argv[0]);
        return argc > 2 ? RETURN_ERROR : RETURN_OK;
    }
    if (argc == 2 && strcmp(argv[1], "OFF") == 0)
        return turn_off();
    if (argc == 2) {
        printf("ERROR: expected OFF\n");
        return RETURN_ERROR;
    }

    board_addr = zz9000_find_board(NULL);
    if (!board_addr) {
        printf("ERROR: ZZ9000 not found\n");
        return RETURN_ERROR;
    }
    return turn_on(board_addr);
}
