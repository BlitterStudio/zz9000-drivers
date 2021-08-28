#include <exec/types.h>
#include <devices/trackdisk.h>
#include <devices/scsidisk.h>
#include <libraries/expansion.h>
#include <proto/expansion.h>
#include <stdint.h>
#include "mntsd_cmd.h"

// Based on https://github.com/captain-amygdala/pistorm/blob/main/platforms/amiga/piscsi/piscsi.h

#define RDB_BLOCK_LIMIT 16
#define BLOCK_SIZE 512
// RDSK
#define RDB_IDENTIFIER 0x5244534B
// PART
#define PART_IDENTIFIER 0x50415254
// FSHD
#define	FS_IDENTIFIER 0x46534844

struct RigidDiskBlock {
    uint32_t   rdb_ID;
    uint32_t   rdb_SummedLongs;
    int32_t    rdb_ChkSum;
    uint32_t   rdb_HostID;
    uint32_t   rdb_BlockBytes;
    uint32_t   rdb_Flags;
    /* block list heads */
    uint32_t   rdb_BadBlockList;
    uint32_t   rdb_PartitionList;
    uint32_t   rdb_FileSysHeaderList;
    uint32_t   rdb_DriveInit;
    uint32_t   rdb_Reserved1[6];
    /* physical drive characteristics */
    uint32_t   rdb_Cylinders;
    uint32_t   rdb_Sectors;
    uint32_t   rdb_Heads;
    uint32_t   rdb_Interleave;
    uint32_t   rdb_Park;
    uint32_t   rdb_Reserved2[3];
    uint32_t   rdb_WritePreComp;
    uint32_t   rdb_ReducedWrite;
    uint32_t   rdb_StepRate;
    uint32_t   rdb_Reserved3[5];
    /* logical drive characteristics */
    uint32_t   rdb_RDBBlocksLo;
    uint32_t   rdb_RDBBlocksHi;
    uint32_t   rdb_LoCylinder;
    uint32_t   rdb_HiCylinder;
    uint32_t   rdb_CylBlocks;
    uint32_t   rdb_AutoParkSeconds;
    uint32_t   rdb_HighRDSKBlock;
    uint32_t   rdb_Reserved4;
    /* drive identification */
    char    rdb_DiskVendor[8];
    char    rdb_DiskProduct[16];
    char    rdb_DiskRevision[4];
    char    rdb_ControllerVendor[8];
    char    rdb_ControllerProduct[16];
    char    rdb_ControllerRevision[4];
    char    rdb_DriveInitName[40];
};

struct PartitionBlock {
    uint32_t   pb_ID;
    uint32_t   pb_SummedLongs;
    int32_t    pb_ChkSum;
    uint32_t   pb_HostID;
    uint32_t   pb_Next;
    uint32_t   pb_Flags;
    uint32_t   pb_Reserved1[2];
    uint32_t   pb_DevFlags;
    uint8_t    pb_DriveName[32];
    uint32_t   pb_Reserved2[15];
    uint32_t   pb_Environment[20];
    uint32_t   pb_EReserved[12];
};

void debugstr(void* regs, char* str) {
  while (*str) {
    *((volatile uint16_t*)(regs+0xf0)) = *str;
    str++;
  }
}

void debughex(void* regs, uint32_t val) {
  *((volatile uint16_t*)(regs+0xf2)) = val>>16;
  *((volatile uint16_t*)(regs+0xf2)) = val;
}

void find_partitions(struct Library* ExpansionBase, struct ConfigDev* cd, struct RigidDiskBlock* rdb) {
    void* regs = (void*)cd->cd_BoardAddr;
    int cur_partition = 0;
    uint8_t tmp;

    uint32_t block[BLOCK_SIZE/4]; // shared storage for 1 block

    if (!rdb || rdb->rdb_PartitionList == 0) {
      debugstr(regs, "No partitions on disk.\r\n");
      return;
    }

    uint32_t cur_block = rdb->rdb_PartitionList;

next_partition:;
    sdcmd_read_blocks(regs+0xd0, (uint8_t*)block, cur_block, 1);

    debugstr(regs, "Block: ");
    debughex(regs, cur_block);
    debugstr(regs, "\r\n");

    uint32_t first = block[0];
    if (first != PART_IDENTIFIER) {
      debugstr(regs, "Not a valid partition.\r\n");
      return;
    }

    struct PartitionBlock *pb = (struct PartitionBlock *)block;
    tmp = pb->pb_DriveName[0];
    pb->pb_DriveName[tmp + 1] = 0x00;

    debughex(regs, cur_partition);
    debugstr(regs, "\t");
    debugstr(regs, (char*)pb->pb_DriveName + 1);
    debugstr(regs, "\r\n");
    debughex(regs, pb->pb_ChkSum);
    debugstr(regs, "\t");
    debughex(regs, pb->pb_HostID);
    debugstr(regs, "\r\n");
    debughex(regs, pb->pb_Flags);
    debugstr(regs, "\t");
    debughex(regs, pb->pb_DevFlags);
    debugstr(regs, "\r\n");

    char execName[] = "zzusb.device";
    char* dosName = pb->pb_DriveName + 1;

    ULONG parmPkt[] = {
        (ULONG) dosName,
        (ULONG) execName,
        0,                  /* unit number */
        0,                  /* OpenDevice flags */
        0,0,0,0,0,
        0,0,0,0,0,
        0,0,0,0,0,
        0,0,
    };

    for (int i=0; i<17; i++) {
      parmPkt[4+i] = pb->pb_Environment[i];
    }

    debugstr(regs, "TblSize: ");
    debughex(regs, parmPkt[4]);
    debugstr(regs, " LoCyl: ");
    debughex(regs, parmPkt[4+9]);
    debugstr(regs, " HiCyl: ");
    debughex(regs, parmPkt[4+10]);
    debugstr(regs, " DosType: ");
    debughex(regs, parmPkt[4+16]);
    debugstr(regs, "\r\n");

    struct DeviceNode *node = MakeDosNode(parmPkt);

    if (node) {
      debugstr(regs, "GlobalVec: ");
      debughex(regs, node->dn_GlobalVec);
      debugstr(regs, "\r\n");

      node->dn_GlobalVec = -1; // yet unclear if needed

      AddBootNode(0, 0, node, cd);
      debugstr(regs, "AddBootNode done.\r\n");
    }

    if (pb->pb_Next != 0xFFFFFFFF) {
        uint64_t next = pb->pb_Next;
        cur_block = next;

        cur_partition++;
        goto next_partition;
    }
    debugstr(regs, "No more partitions.\r\n");
    //d->num_partitions = cur_partition + 1;
    //d->fshd_offs = lseek64(fd, 0, SEEK_CUR);

    return;
}

int parse_rdb(struct Library* ExpansionBase, struct ConfigDev* cd) {
    void* regs = (void*)cd->cd_BoardAddr;
    int i = 0;

    uint32_t block[BLOCK_SIZE/4]; // shared storage for 1 block

    debugstr(regs, "Hello from parse_rdb!\r\n");

    for (i = 0; i < RDB_BLOCK_LIMIT; i++) {
      sdcmd_read_blocks(regs+0xd0, (uint8_t*)block, i, 1);
      uint32_t first = block[0];
      if (first == RDB_IDENTIFIER)
        goto rdb_found;
    }
    goto no_rdb_found;

rdb_found:;
    struct RigidDiskBlock *rdb = (struct RigidDiskBlock *)block;

    debugstr(regs, "RDB found at block:\t");
    debughex(regs, i);

    debugstr(regs, "\r\nCylinders:\t");
    debughex(regs, rdb->rdb_Cylinders);
    debugstr(regs, "\r\nHeads:\t");
    debughex(regs, rdb->rdb_Heads);
    debugstr(regs, "\r\nSectors:\t");
    debughex(regs, rdb->rdb_Sectors);
    debugstr(regs, "\r\nBlockBytes:\t");
    debughex(regs, rdb->rdb_BlockBytes);
    debugstr(regs, "\r\nPartitionList:\t");
    debughex(regs, rdb->rdb_PartitionList);
    debugstr(regs, "\r\n");

    //sprintf(rdb->rdb_DriveInitName, "zzusb.device");

    find_partitions(ExpansionBase, cd, rdb);
    return 0;

no_rdb_found:
    debugstr(regs, "RDB not found!\r\n");
    return -1;
}
