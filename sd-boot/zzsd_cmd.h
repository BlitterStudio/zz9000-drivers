/*
 * ZZ9000 SD-boot driver — shared definitions (register map, wire
 * format, SDBase device-library layout).
 *
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ZZSD_CMD_H
#define ZZSD_CMD_H

#include <exec/types.h>
#include <exec/io.h>
#include <exec/semaphores.h>
#include <libraries/expansion.h>
#include <stdint.h>

#define SD_SECTOR_BYTES  512
#define SD_SECTOR_SHIFT  9
#define SD_RETRY         10
#define SD_UNITS         1

#define ZZSD_BLK_TX_HI   0xB4
#define ZZSD_BLK_TX_LO   0xB6
#define ZZSD_BLK_RX_HI   0xB8
#define ZZSD_BLK_RX_LO   0xBA
#define ZZSD_STATUS      0xBC
#define ZZSD_CAPACITY    0xC8

#define ZZSD_BOOT_CMD    0xC2
#define ZZSD_BOOT_STATUS 0xC4

/* BOOT_CMD register layout (16-bit):
 *   bits  3..0  command code: 1=GETINFO, 2..9=LOADFS fs 0..7
 *   bits 15..4  chunk index (for LOADFS only; 0 for GETINFO)
 *
 * A single 16-bit register write (Z3 splits 32-bit writes into 16-bit
 * register ops on our side, so we can't deliver more than 16 bits
 * atomically). 12 bits * 16 KB = 64 MB max filesystem — comfortable
 * for PFS3 at ~60 KB. */
#define ZZSD_BOOTCMD_GETINFO  1
#define ZZSD_BOOTCMD_LOADFS   2
#define ZZSD_BOOTCMD_MASK     0x000F
#define ZZSD_BOOTCMD_CHUNK_SHIFT 4

#define SD_STATUS_BUSY    0xFFFF

#define SDERRF_TIMEOUT   (1 << 7)
#define SDERRF_PARAM     (1 << 6)

#define ZZSD_BUFFER_OFFSET  0xA000
/* Amiga reads the shared buffer via the FPGA's 0xA000..0x10000 Zorro
 * window = 24 KB; anything past that falls through to framebuffer
 * memory and returns zeros. Filesystem blobs (like pfs3aio at ~60 KB)
 * don't fit, so the driver streams them in 16 KB chunks. */
#define ZZSD_FS_CHUNK_SIZE  (16 * 1024)

#define SD_BOOT_MAGIC        0x5344424F
#define SD_BOOT_MAX_PARTITIONS  16
#define SD_BOOT_MAX_FILESYSTEMS 4

struct sd_boot_partition {
    uint32_t flags;               /* pb_Flags (bit 0 = bootable) */
    uint32_t environment[20];     /* pb_Environment (DOSEnvec) */
    uint32_t drive_name[8];       /* pb_DriveName (BSTR, 32 bytes) */
};

struct sd_boot_filesystem {
    uint32_t dos_type;
    uint32_t version;
    uint32_t patch_flags;
    uint32_t type;
    uint32_t task;
    uint32_t lock;
    uint32_t handler;
    uint32_t stack_size;
    uint32_t priority;
    uint32_t startup;
    uint32_t global_vec;
    uint32_t seg_list_size;
};

struct sd_boot_info {
    uint32_t magic;
    uint32_t version;
    uint32_t partition_count;
    uint32_t filesystem_count;
    uint32_t block_size;
    uint32_t rdb_start_block;
    uint32_t partition_blocks;
    struct sd_boot_partition partitions[SD_BOOT_MAX_PARTITIONS];
    struct sd_boot_filesystem filesystems[SD_BOOT_MAX_FILESYSTEMS];
};

#define FSE_TYPE        (1 << 0)
#define FSE_TASK        (1 << 1)
#define FSE_LOCK        (1 << 2)
#define FSE_HANDLER     (1 << 3)
#define FSE_STACKSIZE   (1 << 4)
#define FSE_PRIORITY    (1 << 5)
#define FSE_STARTUP     (1 << 6)
#define FSE_SEGLIST     (1 << 7)
#define FSE_GLOBALVEC   (1 << 8)

#define NSCMD_DEVICEQUERY       0x4000
#define NSDEVTYPE_TRACKDISK     5

struct NSDeviceQueryResult {
    ULONG   DevQueryFormat;
    ULONG   SizeAvailable;
    UWORD   DeviceType;
    UWORD   DeviceSubType;
    UWORD   *SupportedCommands;
};

#define NSCMD_TD_READ64     0xC000
#define NSCMD_TD_WRITE64    0xC001
#define NSCMD_TD_SEEK64     0xC002
#define NSCMD_TD_FORMAT64   0xC003
/* TD_READ64/WRITE64/SEEK64/FORMAT64 are also defined by recent
 * trackdisk.h in the Amiga NDK. Guard so both toolchains (with and
 * without updated NDK) build cleanly. */
#ifndef TD_READ64
#define TD_READ64     24
#define TD_WRITE64    25
#define TD_SEEK64     26
#define TD_FORMAT64   27
#endif

/* struct Device must be embedded at offset 0 so AutoInit's library
 * header (lib_Node, lib_NegSize, lib_Version, lib_IdString, lib_OpenCnt)
 * lines up with the Kickstart-allocated device base. A previous layout
 * had `struct Device *sd_Device` as the first field with sd_Lock
 * immediately after — InitSemaphore() then zeroed offsets 4..47, which
 * overwrites lib_Node.ln_Name and every other lib_ field. Exec's
 * OpenDevice() could no longer find us by name, so PFS3 failed with
 * "Initialization failure" before ever calling our Open vector. */
struct SDBase {
    struct Device sd_Device;
    struct SignalSemaphore sd_Lock;
    struct SDUnit {
        struct Unit sdu_Unit;
        BOOL sdu_Enabled;
        void* sdu_Registers;
        BOOL sdu_Present;
        BOOL sdu_Valid;
        BOOL sdu_ReadOnly;
        BOOL sdu_Motor;
        ULONG sdu_ChangeNum;
    } sd_Unit[SD_UNITS];
    uint32_t sd_BaseOffset;
    uint32_t sd_PartBlocks;
};

uint16_t zzsd_read_blocks(void* boardaddr, uint8_t* data, uint32_t block, uint32_t len);
uint16_t zzsd_write_blocks(void* boardaddr, uint8_t* data, uint32_t block, uint32_t len);
uint32_t zzsd_capacity(void* boardaddr);
void zzsd_boot_init(void* boardaddr, struct SDBase *sdb, struct ConfigDev *cd);
/* Debug output goes through the ZZ9000 firmware's REG_ZZ_PRINT_CHR /
 * REG_ZZ_PRINT_HEX so trace lines land in the Zynq serial console.
 * Compiled out by default because each call bloats the driver ~6
 * insns and the driver has to fit inside the 8 KB FPGA-decoded ROM
 * window at cardbase+0x6000..0x8000. Re-enable via DEBUG=1 on the
 * build.sh command line when diagnosing. */
#ifdef ZZSD_DRIVER_DEBUG
void debugstr(void* regs, char* str);
void debughex(void* regs, uint32_t val);
#else
#define debugstr(regs, str) ((void)0)
#define debughex(regs, val) ((void)0)
#endif

#endif
