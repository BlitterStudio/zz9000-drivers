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

#define uint8_t unsigned char
#define uint16_t unsigned short
#define uint32_t unsigned long

#define u16 uint16_t
#define u32 uint32_t

#define MNTVA_COLOR_8BIT     0
#define MNTVA_COLOR_16BIT565 1
#define MNTVA_COLOR_32BIT    2
#define MNTVA_COLOR_15BIT    3

typedef volatile struct MNTZZ9KRegs {
  u16 fw_version; // 00
  u16 mode;       // 02
  u16 vdiv;       // 04
  u16 screen_w;   // 06
  u16 screen_h;   // 08

  u16 pan_ptr_hi; // 0a
  u16 pan_ptr_lo; // 0c
  u16 hdiv;       // 0e
  u16 blitter_x1; // 10
  u16 blitter_y1; // 12
  u16 blitter_x2; // 14
  u16 blitter_y2; // 16
  u16 blitter_row_pitch; // 18 destination pitch
  u16 blitter_x3; // 1a
  u16 blitter_y3; // 1c
  u16 blitter_rgb_hi;       // 1e
  u16 blitter_rgb_lo;       // 20
  u16 blitter_op_fillrect;  // 22
  u16 blitter_op_copyrect;  // 24
  u16 blitter_op_filltemplate;   // 26
  
  u16 blitter_src_hi; // 28
  u16 blitter_src_lo; // 2a
  u16 blitter_dst_hi; // 2c
  u16 blitter_dst_lo; // 2e
  
  u16 blitter_colormode; // 30 destination colormode
  u16 blitter_src_pitch; // 32
  u16 blitter_rgb2_hi; // 34 background/secondary color
  u16 blitter_rgb2_lo; // 36
  u16 blitter_op_p2c; // 38
  u16 blitter_op_draw_line; // 3A
  
  u16 un_3[0x20]; // 3C..7e

  u16 eth_tx; // 80
  u16 eth_rx; // 82

  u16 un_4[6]; // 84,86,88,8a,8c,8e

  u16 arm_run_hi; // 90
  u16 arm_run_lo; // 92
  u16 arm_argc;   // 94
  u16 arm_arg[8]; // 96,98,9a,9c..a4

  u16 un_5[5]; // a6..ae
  
  u16 arm_event_serial; // b0
  u16 arm_event_code; // b2
} MNTZZ9KRegs;

typedef volatile struct MNTZZ9KCXRegs {
  u16 video_control_data_hi; // 00
  u16 video_control_data_lo; // 02
  u16 video_control_op;      // 04
  u16 videocap_mode;         // 06
} MNTZZ9KCXRegs;
