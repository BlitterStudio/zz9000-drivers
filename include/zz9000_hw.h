/*
 * Shared ZZ9000 hardware constants and small user-space helpers.
 *
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ZZ9000_HW_H
#define ZZ9000_HW_H

#include <exec/types.h>
#include <libraries/expansion.h>
#include <proto/exec.h>
#include <proto/expansion.h>

#define ZZ9000_MNT_MANUFACTURER  0x6d6e
#define ZZ9000_PRODUCT_Z2        3
#define ZZ9000_PRODUCT_Z3        4

#define ZZ_REG_HW_VERSION        0x00
#define ZZ_REG_MODE              0x02
#define ZZ_REG_CONFIG            0x04
#define ZZ_REG_VCAP_MODE         0x0E
#define ZZ_REG_VIDEOCAP_STATS    0x4E

#define ZZ_REG_VIDEOCAP_DIAG_MAGIC      0x60
#define ZZ_REG_VIDEOCAP_DIAG_FLAGS      0x62
#define ZZ_REG_VIDEOCAP_DIAG_Y3MAX      0x64
#define ZZ_REG_VIDEOCAP_DIAG_YSYNC_MAX  0x66
#define ZZ_REG_VIDEOCAP_DIAG_XLEN       0x68
#define ZZ_REG_VIDEOCAP_DIAG_PHASE      0x6A
#define ZZ_REG_VIDEOCAP_DIAG_RISELINES  0x6C
#define ZZ_REG_VIDEOCAP_DIAG_FALLLINES  0x6E

#define ZZ_REG_AUDIO_SWAB        0x70
#define ZZ_REG_AUDIO_SCALE       0x74
#define ZZ_REG_AUDIO_PARAM       0x76
#define ZZ_REG_AUDIO_VAL         0x78
#define ZZ_REG_DECODER_FIFO      0x72
#define ZZ_REG_DECODER_PARAM     0x7A
#define ZZ_REG_DECODER_VAL       0x7C
#define ZZ_REG_DECODE            0x7E

#define ZZ_REG_ETH_TX            0x80
#define ZZ_REG_ETH_RX            0x82
#define ZZ_REG_ETH_MAC_HI        0x84
#define ZZ_REG_ETH_MAC_HI2       0x86
#define ZZ_REG_ETH_MAC_LO        0x88
#define ZZ_REG_ETH_RX_STATUS     0x8C
#define ZZ_REG_ETH_RX_STATS      0x8E

#define ZZ_REG_FW_VERSION        0xC0

#define ZZ_REG_SD_BLK_TX_HI      0xB4
#define ZZ_REG_SD_BLK_TX_LO      0xB6
#define ZZ_REG_SD_BLK_RX_HI      0xB8
#define ZZ_REG_SD_BLK_RX_LO      0xBA
#define ZZ_REG_SD_STATUS         0xBC
#define ZZ_REG_SD_BOOT_CMD       0xC2
#define ZZ_REG_SD_BOOT_STATUS    0xC4
#define ZZ_REG_SD_CAPACITY       0xC8

#define ZZ_REG_FWUP_CMD          0xCA
#define ZZ_REG_FWUP_LEN          0xCC
#define ZZ_REG_FWUP_STATUS       0xCE

#define ZZ_REG_USB_BLK_TX_HI     0xD0
#define ZZ_REG_USB_BLK_TX_LO     0xD2
#define ZZ_REG_USB_BLK_RX_HI     0xD4
#define ZZ_REG_USB_BLK_RX_LO     0xD6
#define ZZ_REG_USB_STATUS        0xD8
#define ZZ_REG_USB_BUFSEL        0xDA
#define ZZ_REG_USB_CAPACITY      0xDC
#define ZZ_REG_USB_PROXY_CMD     0xDE

#define ZZ_REG_TEMPERATURE       0xE0
#define ZZ_REG_VOLTAGE_AUX       0xE2
#define ZZ_REG_VOLTAGE_INT       0xE4
#define ZZ_REG_AUDIO_CONFIG      0xF4
#define ZZ_REG_DEBUG             0xFC

/* Feature toggles (write ZZ_REG_USER1 = feature id, then the value here). */
#define ZZ_REG_USER1             0x40
#define ZZ_REG_SET_FEATURE       0x60
#define ZZ_CARD_FEATURE_NONSTANDARD_VSYNC 2

/* ZZ9000.CFG interface (firmware >= 2.3, issue #33). Key ids, file
 * statuses and the client API live in common/zzcfg_amiga.h. */
#define ZZ_REG_CONFIG_KEY        0xE8
#define ZZ_REG_CONFIG_PRESENT    0xEA
#define ZZ_REG_CONFIG_FILE       0xEC
#define ZZ_REG_CONFIG_FILE_LEN   0xEE

#define ZZ_SCANLINE_MODE_REG     0x100C
#define ZZ_SCANLINE_PARITY_REG   0x100E

#define ZZ_BUFFER_OFFSET         0xA000

#define ZZ_VCAP_LINES_MASK        0x03ff
#define ZZ_VCAP_PW_MAX_TIER_SHIFT 10
#define ZZ_VCAP_PW_MIN_TIER_SHIFT 12
#define ZZ_VCAP_EDGE_SHIFT        14
#define ZZ_VCAP_INTERLACE_SHIFT   15

#define ZZ_VCAP_DIAG_MAGIC         0x5643
#define ZZ_VCAP_DIAG_VERSION_SHIFT 10
#define ZZ_VCAP_DIAG_VERSION_MASK  0x000f

struct ZZ9000Board {
    ULONG address;
    UWORD product;
    UWORD zorro_version;
};

static inline UWORD zz9000_read_reg16(ULONG board_addr, ULONG offset)
{
    return *((volatile UWORD *)(board_addr + offset));
}

static inline void zz9000_write_reg16(ULONG board_addr, ULONG offset, UWORD value)
{
    *((volatile UWORD *)(board_addr + offset)) = value;
}

static inline ULONG zz9000_find_board(struct ZZ9000Board *board)
{
    struct Library *ExpansionBase;
    struct ConfigDev *cd;

    if (board) {
        board->address = 0;
        board->product = 0;
        board->zorro_version = 0;
    }

    ExpansionBase = OpenLibrary((CONST_STRPTR)"expansion.library", 0);
    if (!ExpansionBase) return 0;

    cd = FindConfigDev(NULL, ZZ9000_MNT_MANUFACTURER, ZZ9000_PRODUCT_Z3);
    if (cd) {
        if (board) {
            board->address = (ULONG)cd->cd_BoardAddr;
            board->product = ZZ9000_PRODUCT_Z3;
            board->zorro_version = 3;
        }
        CloseLibrary(ExpansionBase);
        return (ULONG)cd->cd_BoardAddr;
    }

    cd = FindConfigDev(NULL, ZZ9000_MNT_MANUFACTURER, ZZ9000_PRODUCT_Z2);
    if (cd) {
        if (board) {
            board->address = (ULONG)cd->cd_BoardAddr;
            board->product = ZZ9000_PRODUCT_Z2;
            board->zorro_version = 2;
        }
        CloseLibrary(ExpansionBase);
        return (ULONG)cd->cd_BoardAddr;
    }

    CloseLibrary(ExpansionBase);
    return 0;
}

#endif /* ZZ9000_HW_H */
