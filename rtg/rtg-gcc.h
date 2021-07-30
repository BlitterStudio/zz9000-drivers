/*
 * MNT ZZ9000 Amiga Graphics Card Driver (ZZ9000.card)
 * Copyright (C) 2016-2019, Lukas F. Hartmann <lukas@mntre.com>
 *                          MNT Research GmbH, Berlin
 *                          https://mntre.com
 *
 * More Info: https://mntre.com/zz9000
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * GNU General Public License v3.0 or later
 *
 * https://spdx.org/licenses/GPL-3.0-or-later.html
 */

#include <exec/lists.h>
#include <exec/interrupts.h>
#include <graphics/gfx.h>

#define int32 long
#define int16 short
#define int8  char

#define uint32 unsigned long
#define uint16 unsigned short
#define uint8  unsigned char

enum RTG_COLOR_MODES {
  rtg_color_planar,
  rtg_color_clut,
  rtg_color_16bit,
  rtg_color_24bit,
  rtg_color_32bit,
  rtg_color_num,
};

#define RTG_COLOR_FORMAT_PLANAR 0
#define RTG_COLOR_FORMAT_CLUT 1
#define RTG_COLOR_FORMAT_RGB888 2
#define RTG_COLOR_FORMAT_BGR888 4
#define RTG_COLOR_FORMAT_RGB565_WEIRD1 8
#define RTG_COLOR_FORMAT_RGB565_WEIRD2 16
#define RTG_COLOR_FORMAT_ARGB 32
#define RTG_COLOR_FORMAT_ABGR 64
#define RTG_COLOR_FORMAT_RGBA 128
#define RTG_COLOR_FORMAT_BGRA 256
#define RTG_COLOR_FORMAT_RGB565 512
#define RTG_COLOR_FORMAT_RGB555 1024
#define RTG_COLOR_FORMAT_BGR565_WEIRD3 2048
#define RTG_COLOR_FORMAT_BGR565_WEIRD4 4096
#define RTG_COLOR_FORMAT_32BIT (RTG_COLOR_FORMAT_ARGB|RTG_COLOR_FORMAT_ABGR|RTG_COLOR_FORMAT_RGBA|RTG_COLOR_FORMAT_BGRA)
