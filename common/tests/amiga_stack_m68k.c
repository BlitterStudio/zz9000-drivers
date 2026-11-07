/*
 * m68k harness for amiga_stack_m68k.sh: run amiga_stack_run() with more
 * stack than the task has, so it must swap to a private stack and back.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <exec/tasks.h>
#include <proto/exec.h>
#include <stdio.h>

#include "amiga_stack.h"

struct probe {
    int calls;
    ULONG sp;
};

static int entry(void *ctx)
{
    struct probe *p = ctx;
    volatile UBYTE local = 0;

    p->calls++;
    p->sp = (ULONG)&local;
    return 37;
}

int main(void)
{
    struct probe p = { 0, 0 };
    struct Task *task = FindTask(NULL);
    ULONG lower = (ULONG)task->tc_SPLower;
    ULONG upper = (ULONG)task->tc_SPUpper;
    volatile UBYTE here = 0;
    int rc;

    rc = amiga_stack_run("amiga_stack_m68k", upper - lower + 65536, entry, &p);
    if (rc != 37 || p.calls != 1) {
        printf("FAIL: rc %d, entry ran %d time(s)\n", rc, p.calls);
        return 20;
    }
    if (p.sp >= lower && p.sp < upper) {
        printf("FAIL: entry ran on the caller's stack\n");
        return 20;
    }
    if ((ULONG)&here < lower || (ULONG)&here >= upper ||
        (ULONG)task->tc_SPLower != lower || (ULONG)task->tc_SPUpper != upper) {
        printf("FAIL: the caller's stack was not restored\n");
        return 20;
    }
    printf("amiga_stack m68k: entry ran on a private stack and returned 37\n");
    return 0;
}
