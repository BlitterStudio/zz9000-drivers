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
#define MNTVA_COLOR_NO_USE   255

typedef volatile struct MNTZZ9KRegs {
  u16 fw_version; // 00
  u16 mode;       // 02
  u16 config;     // 04 misc config bits
  u16 sprite_x;   // 06
  u16 sprite_y;   // 08

  u16 pan_ptr_hi; // 0a
  u16 pan_ptr_lo; // 0c
  u16 videocap_vmode; // 0e
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
  u16 blitter_op_draw_line; // 3a
  u16 blitter_op_p2d; // 3c
  u16 blitter_op_invertrect; // 3e

  // Reusing other register-accessible variables was getting a bit cluttered, and somewhat
  // of a coding hazard. Four additional user values should help for the time being.
  u16 blitter_user1; // 40
  u16 blitter_user2; // 42
  u16 blitter_user3; // 44
  u16 blitter_user4; // 46

  u16 sprite_bitmap; // 48
  u16 sprite_colors; // 4a
  u16 vblank_status; // 4c

  //u16 un_3[0x17]; // 4e..7e
  u16 un_4e;
  u16 un_50;
  u16 un_52;
  u16 un_54;
  u16 un_56;
  u16 un_58;
  u16 blitter_dma_op; // 5a
  u16 blitter_acc_op; // 5c
  u16 blitter_set_split_pos; // 5e
  u16 set_feature_status;
  u16 un_62;
  u16 un_64;
  u16 un_66;
  u16 un_68;
  u16 un_6A;
  u16 un_6C;
  u16 un_6E;
  u16 un_70;
  u16 un_72;
  u16 un_74;
  u16 un_76;
  u16 un_78;
  u16 un_7A;
  u16 un_7C;
  u16 un_7E;

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

enum zz_reg_offsets {
  REG_ZZ_HW_VERSION     = 0x00,
  REG_ZZ_MODE           = 0x02,
  REG_ZZ_CONFIG         = 0x04,
  REG_ZZ_SPRITE_X       = 0x06,
  REG_ZZ_SPRITE_Y       = 0x08,
  REG_ZZ_PAN_HI         = 0x0A,
  REG_ZZ_PAN_LO         = 0x0C,
  REG_ZZ_VCAP_MODE      = 0x0E,

  REG_ZZ_X1             = 0x10,
  REG_ZZ_Y1             = 0x12,
  REG_ZZ_X2             = 0x14,
  REG_ZZ_Y2             = 0x16,
  REG_ZZ_ROW_PITCH      = 0x18,
  REG_ZZ_X3             = 0x1A,
  REG_ZZ_Y3             = 0x1C,
  REG_ZZ_RGB_HI         = 0x1E,

  REG_ZZ_RGB_LO         = 0x20,
  REG_ZZ_FILLRECT       = 0x22,
  REG_ZZ_COPYRECT       = 0x24,
  REG_ZZ_FILLTEMPLATE   = 0x26,
  REG_ZZ_BLIT_SRC_HI    = 0x28,
  REG_ZZ_BLIT_SRC_LO    = 0x2A,
  REG_ZZ_BLIT_DST_HI    = 0x2C,
  REG_ZZ_BLIT_DST_LO    = 0x2E,

  REG_ZZ_COLORMODE      = 0x30,
  REG_ZZ_SRC_PITCH      = 0x32,
  REG_ZZ_RGB2_HI        = 0x34,
  REG_ZZ_RGB2_LO        = 0x36,
  REG_ZZ_P2C            = 0x38,
  REG_ZZ_DRAWLINE       = 0x3A,
  REG_ZZ_P2D            = 0x3C,
  REG_ZZ_INVERTRECT     = 0x3E,

  REG_ZZ_USER1          = 0x40,
  REG_ZZ_USER2          = 0x42,
  REG_ZZ_USER3          = 0x44,
  REG_ZZ_USER4          = 0x46,
  REG_ZZ_SPRITE_BITMAP  = 0x48,
  REG_ZZ_SPRITE_COLORS  = 0x4A,
  REG_ZZ_VBLANK_STATUS  = 0x4C,
  REG_ZZ_UNUSED_REG4E   = 0x4E,

  REG_ZZ_UNUSED_REG50   = 0x50,
  REG_ZZ_UNUSED_REG52   = 0x52,
  REG_ZZ_UNUSED_REG54   = 0x54,
  REG_ZZ_UNUSED_REG56   = 0x56,
  REG_ZZ_UNUSED_REG58   = 0x58,
  REG_ZZ_DMA_OP         = 0x5A,
  REG_ZZ_ACC_OP         = 0x5C,
  REG_ZZ_SET_SPLIT_POS  = 0x5E,

  REG_ZZ_SET_FEATURE    = 0x60,
  REG_ZZ_UNUSED_REG62   = 0x62,
  REG_ZZ_UNUSED_REG64   = 0x64,
  REG_ZZ_UNUSED_REG66   = 0x66,
  REG_ZZ_UNUSED_REG68   = 0x68,
  REG_ZZ_UNUSED_REG6A   = 0x6A,
  REG_ZZ_UNUSED_REG6C   = 0x6C,
  REG_ZZ_UNUSED_REG6E   = 0x6E,

  REG_ZZ_UNUSED_REG70   = 0x70,
  REG_ZZ_UNUSED_REG72   = 0x72,
  REG_ZZ_UNUSED_REG74   = 0x74,
  REG_ZZ_UNUSED_REG76   = 0x76,
  REG_ZZ_UNUSED_REG78   = 0x78,
  REG_ZZ_UNUSED_REG7A   = 0x7A,
  REG_ZZ_UNUSED_REG7C   = 0x7C,
  REG_ZZ_UNUSED_REG7E   = 0x7E,

  REG_ZZ_ETH_TX         = 0x80,
  REG_ZZ_ETH_RX         = 0x82,
  REG_ZZ_ETH_MAC_HI     = 0x84,
  REG_ZZ_ETH_MAC_HI2    = 0x86,
  REG_ZZ_ETH_MAC_LO     = 0x88,
  REG_ZZ_UNUSED_REG8A   = 0x8A,
  REG_ZZ_UNUSED_REG8C   = 0x8C,
  REG_ZZ_UNUSED_REG8E   = 0x8E,

  REG_ZZ_ARM_RUN_HI     = 0x90,
  REG_ZZ_ARM_RUN_LO     = 0x92,
  REG_ZZ_ARM_ARGC       = 0x94,
  REG_ZZ_ARM_ARGV0      = 0x96,
  REG_ZZ_ARM_ARGV1      = 0x98,
  REG_ZZ_ARM_ARGV2      = 0x9A,
  REG_ZZ_ARM_ARGV3      = 0x9C,
  REG_ZZ_ARM_ARGV4      = 0x9E,

  REG_ZZ_ARM_ARGV5      = 0xA0,
  REG_ZZ_ARM_ARGV6      = 0xA2,
  REG_ZZ_ARM_ARGV7      = 0xA4,
  REG_ZZ_UNUSED_REGA6   = 0xA6,
  REG_ZZ_UNUSED_REGA8   = 0xA8,
  REG_ZZ_UNUSED_REGAA   = 0xAA,
  REG_ZZ_UNUSED_REGAC   = 0xAC,
  REG_ZZ_UNUSED_REGAE   = 0xAE,

  REG_ZZ_ARM_EV_SERIAL  = 0xB0,
  REG_ZZ_ARM_EV_CODE    = 0xB2,
  REG_ZZ_UNUSED_REGB4   = 0xB4,
  REG_ZZ_UNUSED_REGB6   = 0xB6,
  REG_ZZ_UNUSED_REGB8   = 0xB8,
  REG_ZZ_UNUSED_REGBA   = 0xBA,
  REG_ZZ_UNUSED_REGBC   = 0xBC,
  REG_ZZ_UNUSED_REGBE   = 0xBE,

  REG_ZZ_FW_VERSION     = 0xC0,
  REG_ZZ_UNUSED_REGC2   = 0xC2,
  REG_ZZ_UNUSED_REGC4   = 0xC4,
  REG_ZZ_UNUSED_REGC6   = 0xC6,
  REG_ZZ_UNUSED_REGC8   = 0xC8,
  REG_ZZ_UNUSED_REGCA   = 0xCA,
  REG_ZZ_UNUSED_REGCC   = 0xCC,
  REG_ZZ_UNUSED_REGCE   = 0xCE,

  REG_ZZ_USBBLK_TX_HI   = 0xD0,
  REG_ZZ_USBBLK_TX_LO   = 0xD2,
  REG_ZZ_USBBLK_RX_HI   = 0xD4,
  REG_ZZ_USBBLK_RX_LO   = 0xD6,
  REG_ZZ_USB_STATUS     = 0xD8,
  REG_ZZ_USB_BUFSEL     = 0xDA,
  REG_ZZ_USB_CAPACITY   = 0xDC,
  REG_ZZ_UNUSED_REGDE   = 0xDE,

  REG_ZZ_UNUSED_REGE0   = 0xE0,
  REG_ZZ_UNUSED_REGE2   = 0xE2,
  REG_ZZ_UNUSED_REGE4   = 0xE4,
  REG_ZZ_UNUSED_REGE6   = 0xE6,
  REG_ZZ_UNUSED_REGE8   = 0xE8,
  REG_ZZ_UNUSED_REGEA   = 0xEA,
  REG_ZZ_UNUSED_REGEC   = 0xEC,
  REG_ZZ_UNUSED_REGEE   = 0xEE,

  REG_ZZ_UNUSED_REGF0   = 0xF0,
  REG_ZZ_UNUSED_REGF2   = 0xF2,
  REG_ZZ_UNUSED_REGF4   = 0xF4,
  REG_ZZ_UNUSED_REGF6   = 0xF6,
  REG_ZZ_UNUSED_REGF8   = 0xF8,
  REG_ZZ_UNUSED_REGFA   = 0xFA,
  REG_ZZ_DEBUG          = 0xFC,
  REG_ZZ_UNUSED_REGFE   = 0xFE,
};

enum zz9k_card_features {
  CARD_FEATURE_NONE,
  CARD_FEATURE_SECONDARY_PALETTE,
  CARD_FEATURE_NONSTANDARD_VSYNC,
  CARD_FEATURE_NUM,
};

enum gfx_dma_op {
  OP_NONE,
  OP_DRAWLINE,
  OP_FILLRECT,
  OP_COPYRECT,
  OP_COPYRECT_NOMASK,
  OP_RECT_TEMPLATE,
  OP_RECT_PATTERN,
  OP_P2C,
  OP_P2D,
  OP_INVERTRECT,
  OP_PAN,
  OP_SPRITE_XY,
  OP_SPRITE_COLOR,
  OP_SPRITE_BITMAP,
  OP_SPRITE_CLUT_BITMAP,
  OP_ETH_USB_OFFSETS,
  OP_SET_SPLIT_POS,
  OP_NUM,
};

enum gfx_acc_op {
  ACC_OP_NONE,
  ACC_OP_BUFFER_FLIP,
  ACC_OP_BUFFER_CLEAR,
  ACC_OP_BLIT_RECT,
  ACC_OP_ALLOC_SURFACE,
  ACC_OP_FREE_SURFACE,
  ACC_OP_SET_BPP_CONVERSION_TABLE,
  ACC_OP_DRAW_LINE,
  ACC_OP_FILL_RECT,
  ACC_OP_NUM,
};

enum gfxdata_offsets {
  GFXDATA_DST,
  GFXDATA_SRC,
};

enum gfxdata_u8_types {
  GFXDATA_U8_COLORMODE,
  GFXDATA_U8_DRAWMODE,
  GFXDATA_U8_LINE_PATTERN_OFFSET,
  GFXDATA_U8_LINE_PADDING,
};

#pragma pack(4)
struct GFXData {
  uint32 offset[2];
  uint32 rgb[2];
  uint16 x[4], y[4];
  uint16 user[4];
  uint16 pitch[4];
  uint8 u8_user[8];
  uint8 op, mask, minterm, u8offset;
  uint32_t u32_user[8];
  uint8 clut1[768];
  uint8 clut2[768];
  uint8 clut3[768];
  uint8 clut4[768];
};
