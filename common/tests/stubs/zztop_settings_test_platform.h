/*
 * Host platform for ZZTop's settings model tests: AmigaOS types and the
 * few dos.library calls settings.c makes (ENV: variable access), served
 * from an in-memory file table by zztop_settings_test.c.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ZZTOP_SETTINGS_TEST_PLATFORM_H
#define ZZTOP_SETTINGS_TEST_PLATFORM_H

#include <exec/types.h>

typedef int BOOL;
typedef long BPTR;
typedef const char *CONST_STRPTR;
typedef char *STRPTR;

#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif

#define MODE_OLDFILE 1005

BPTR Open(CONST_STRPTR name, LONG mode);
LONG Close(BPTR file);
LONG Read(BPTR file, void *buffer, LONG length);
BOOL DeleteFile(CONST_STRPTR name);

#endif
