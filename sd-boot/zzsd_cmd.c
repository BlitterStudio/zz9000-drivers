/*
 * ZZ9000 SD-boot driver — low-level block read/write via the Zorro
 * shared-buffer protocol.
 *
 * Copyright (C) 2026, MNT Research GmbH
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <exec/resident.h>
#include <exec/errors.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <string.h>
#include <stdint.h>
#include "zzsd_cmd.h"

extern struct ExecBase* SysBase;

/* 48 × 512 = 24 KB per round-trip — fills the full Zorro buffer window
 * (0xA000..0x10000) exactly. Larger chunks amortize the per-transfer
 * overhead (Zorro register writes, ARM cache flush, polling) across
 * more payload and are the biggest lever we have for bulk throughput.
 * Must stay <= SD_MAX_BLOCKS_AT_ONCE on the firmware side. */
#define BLOCKS_AT_ONCE 48
#define SD_POLL_TIMEOUT 500000

#ifdef ZZSD_DRIVER_DEBUG
void debugstr(void* boardaddr, char* str) {
    volatile uint16_t* chr_reg = (volatile uint16_t*)((uint8_t*)boardaddr + 0xF0);
    while (*str) {
        *chr_reg = (uint8_t)*str;
        str++;
    }
}

void debughex(void* boardaddr, uint32_t val) {
    volatile uint16_t* hex_reg = (volatile uint16_t*)((uint8_t*)boardaddr + 0xF2);
    *hex_reg = val >> 16;
    *hex_reg = val;
}
#endif

uint16_t zzsd_read_blocks(void* boardaddr, uint8_t* data, uint32_t block, uint32_t len) {
    uint8_t* base = (uint8_t*)boardaddr;
    volatile uint16_t* rx_hi = (volatile uint16_t*)(base + ZZSD_BLK_RX_HI);
    volatile uint16_t* rx_lo = (volatile uint16_t*)(base + ZZSD_BLK_RX_LO);
    volatile uint16_t* status = (volatile uint16_t*)(base + ZZSD_STATUS);
    uint8_t* buffer = base + ZZSD_BUFFER_OFFSET;
    uint32_t i = 0;

    while (i < len) {
        uint32_t num_blocks = BLOCKS_AT_ONCE;
        uint16_t st;
        uint32_t timeout;

        if ((len - i) < BLOCKS_AT_ONCE) num_blocks = len - i;

        *status = num_blocks;
        *rx_hi = (block + i) >> 16;
        *rx_lo = (block + i) & 0xffff;

        timeout = SD_POLL_TIMEOUT;
        do {
            st = *status;
            if (st != SD_STATUS_BUSY) break;
            timeout--;
        } while (timeout > 0);

        /* Firmware writes sd_status = sd_storage_read_blocks() return,
         * which is 0 on success and a non-zero error code otherwise
         * (0xFC..0xFF). SD_STATUS_BUSY == 0xFFFF is reserved for the
         * in-progress sentinel. */
        if (timeout == 0) return SDERRF_TIMEOUT;
        if (st != 0) return SDERRF_PARAM;

        CacheClearU();
        CopyMem(buffer, data + (i << SD_SECTOR_SHIFT), num_blocks * SD_SECTOR_BYTES);
        i += num_blocks;
    }

    return 0;
}

uint16_t zzsd_write_blocks(void* boardaddr, uint8_t* data, uint32_t block, uint32_t len) {
    uint8_t* base = (uint8_t*)boardaddr;
    volatile uint16_t* tx_hi = (volatile uint16_t*)(base + ZZSD_BLK_TX_HI);
    volatile uint16_t* tx_lo = (volatile uint16_t*)(base + ZZSD_BLK_TX_LO);
    volatile uint16_t* status = (volatile uint16_t*)(base + ZZSD_STATUS);
    uint8_t* buffer = base + ZZSD_BUFFER_OFFSET;
    uint32_t i = 0;

    while (i < len) {
        uint32_t num_blocks = BLOCKS_AT_ONCE;
        uint16_t st;
        uint32_t timeout;

        if ((len - i) < BLOCKS_AT_ONCE) num_blocks = len - i;

        CopyMem(data + (i << SD_SECTOR_SHIFT), buffer, num_blocks * SD_SECTOR_BYTES);
        CacheClearU();

        *status = num_blocks;
        *tx_hi = (block + i) >> 16;
        *tx_lo = (block + i) & 0xffff;

        timeout = SD_POLL_TIMEOUT;
        do {
            st = *status;
            if (st != SD_STATUS_BUSY) break;
            timeout--;
        } while (timeout > 0);

        if (timeout == 0) return SDERRF_TIMEOUT;
        if (st != 0) return SDERRF_PARAM;
        i += num_blocks;
    }

    return 0;
}

uint32_t zzsd_capacity(void* boardaddr) {
    volatile uint32_t* cap = (volatile uint32_t*)((uint8_t*)boardaddr + (ZZSD_CAPACITY & ~3));
    return *cap;
}
