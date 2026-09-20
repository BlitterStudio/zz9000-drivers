/*
 * Minimal exec/types.h for host-side tests of the shared Amiga sources.
 * Only the types zzcfg_amiga.c actually uses.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ZZ_TEST_EXEC_TYPES_H
#define ZZ_TEST_EXEC_TYPES_H

#include <stdint.h>

typedef uint8_t  UBYTE;
typedef uint16_t UWORD;
typedef uint32_t ULONG;
typedef int16_t  WORD;
typedef int32_t  LONG;
typedef short    BOOL;
typedef void     VOID;
typedef void    *APTR;

#ifndef TRUE
#define TRUE  1
#endif
#ifndef FALSE
#define FALSE 0
#endif

#endif
