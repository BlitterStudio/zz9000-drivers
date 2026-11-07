/*
 * Private-stack runner for AmigaOS command and Workbench tools.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <exec/execbase.h>
#include <exec/memory.h>
#include <exec/tasks.h>
#include <proto/exec.h>
#include <stdint.h>
#include <stdio.h>

#include "amiga_stack.h"

static int current_stack_is_large_enough(ULONG required_bytes)
{
    struct Task *task = FindTask(NULL);
    uintptr_t lower;
    uintptr_t upper;

    if (!task) return 0;
    lower = (uintptr_t)task->tc_SPLower;
    upper = (uintptr_t)task->tc_SPUpper;
    return upper > lower && upper - lower >= required_bytes;
}

/*
 * StackSwap(swap); result = entry(ctx); StackSwap(swap) — in assembly.
 * In C, nothing that lives across the first StackSwap() may be addressed
 * relative to sp, and the compiler gives no such promise: GCC 6.5 kept the
 * StackSwapStruct behind a frame pointer, GCC 16 omits it and passed
 * sp+28 to the second call, which by then pointed into the new stack.
 * Here swap, entry and ctx stay in callee-saved registers; StackSwap()
 * (exec LVO -732) preserves them, and so does the C entry function.
 */
int amiga_stack_swap_call(struct StackSwapStruct *swap,
                          amiga_stack_entry_fn entry, void *ctx);
__asm__(
    "    .text\n"
    "    .even\n"
    "_amiga_stack_swap_call:\n"
    "    moveml d2/a2-a3/a6,sp@-\n"
    "    movel sp@(20),a2\n"      /* swap  */
    "    movel sp@(24),a3\n"      /* entry */
    "    movel sp@(28),d2\n"      /* ctx   */
    "    moveal 4:w,a6\n"
    "    moveal a2,a0\n"
    "    jsr a6@(-732)\n"         /* StackSwap: now on the private stack */
    "    movel d2,sp@-\n"
    "    jsr a3@\n"
    "    addql #4,sp\n"
    "    movel d0,d2\n"
    "    moveal 4:w,a6\n"
    "    moveal a2,a0\n"
    "    jsr a6@(-732)\n"         /* StackSwap: back on the caller's stack */
    "    movel d2,d0\n"
    "    moveml sp@+,d2/a2-a3/a6\n"
    "    rts\n");

int amiga_stack_run(const char *tool_name, ULONG required_bytes,
                    amiga_stack_entry_fn entry, void *ctx)
{
    APTR memory;
    struct StackSwapStruct swap;
    int result;

    if (!entry) return 20;
    if (current_stack_is_large_enough(required_bytes)) return entry(ctx);

    if (SysBase->LibNode.lib_Version < 37) goto no_stack;
    memory = AllocVec(required_bytes, MEMF_PUBLIC);
    if (!memory) goto no_stack;

    swap.stk_Lower = memory;
    swap.stk_Upper = (ULONG)(uintptr_t)((UBYTE *)memory + required_bytes);
    swap.stk_Pointer = (APTR)((UBYTE *)swap.stk_Upper - sizeof(ULONG));
    result = amiga_stack_swap_call(&swap, entry, ctx);
    FreeVec(memory);
    return result;

no_stack:
    printf("ERROR: %s needs %lu bytes of stack; run Stack %lu and try again.\n",
           tool_name, (unsigned long)required_bytes,
           (unsigned long)required_bytes);
    return 20;
}
