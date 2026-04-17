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

#define ZZSD_BOOTCMD_GETINFO  1
#define ZZSD_BOOTCMD_LOADFS   2

#define SD_STATUS_BUSY    0xFFFF

#define SDERRF_TIMEOUT   (1 << 7)
#define SDERRF_PARAM     (1 << 6)

#define ZZSD_BUFFER_OFFSET  0xA000

#define SD_BOOT_MAGIC        0x5344424F
#define SD_BOOT_MAX_PARTITIONS  16
#define SD_BOOT_MAX_FILESYSTEMS 4

struct sd_boot_partition {
    uint32_t start_block;
    uint32_t total_blocks;
    uint32_t dos_type;
    uint32_t flags;
    uint32_t size_block;
    uint32_t surfaces;
    uint32_t sectors_per_block;
    uint32_t blocks_per_track;
    uint32_t reserved;
    uint32_t pre_alloc;
    uint32_t interleave;
    uint32_t low_cyl;
    uint32_t high_cyl;
    uint32_t num_buffer;
    uint32_t buf_mem_type;
    uint32_t max_transfer;
    uint32_t mask;
    uint32_t boot_priority;
    uint32_t drive_name[8];
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
#define TD_READ64     24
#define TD_WRITE64    25
#define TD_SEEK64     26
#define TD_FORMAT64   27

struct SDBase {
    struct Device* sd_Device;
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
void debugstr(void* regs, char* str);
void debughex(void* regs, uint32_t val);

#endif
