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
    StackSwap(&swap);
    result = entry(ctx);
    StackSwap(&swap);
    FreeVec(memory);
    return result;

no_stack:
    printf("ERROR: %s needs %lu bytes of stack; run Stack %lu and try again.\n",
           tool_name, (unsigned long)required_bytes,
           (unsigned long)required_bytes);
    return 20;
}
