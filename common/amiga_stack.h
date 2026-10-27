/*
 * Private-stack runner for AmigaOS command and Workbench tools.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef AMIGA_STACK_H
#define AMIGA_STACK_H

#include <exec/types.h>

typedef int (*amiga_stack_entry_fn)(void *ctx);

int amiga_stack_run(const char *tool_name, ULONG required_bytes,
                    amiga_stack_entry_fn entry, void *ctx);

#endif
