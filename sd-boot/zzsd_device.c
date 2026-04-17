#include <exec/resident.h>
#include <exec/errors.h>
#include <exec/memory.h>
#include <exec/lists.h>
#include <exec/alerts.h>
#include <exec/tasks.h>
#include <exec/io.h>
#include <exec/execbase.h>
#include <libraries/expansion.h>
#include <devices/trackdisk.h>
#include <devices/timer.h>
#include <devices/scsidisk.h>
#include <dos/filehandler.h>
#include <proto/exec.h>
#include <proto/disk.h>
#include <proto/expansion.h>
#include <stdint.h>
#include "zzsd_cmd.h"

struct ExecBase* SysBase;

#define STR(s) #s
#define XSTR(s) STR(s)

#define DEVICE_NAME "zzsd.device"
#define DEVICE_DATE "(12 Apr 2026)"
#define DEVICE_ID_STRING "zzsd.device " XSTR(DEVICE_VERSION) "." XSTR(DEVICE_REVISION) " " DEVICE_DATE
#define DEVICE_VERSION 43
#define DEVICE_REVISION 20
#define DEVICE_PRIORITY 0

#define SD_HEADS         1
#define SD_CYLS          4096
#define SD_TRACK_SECTORS 2048
#define SD_CYL_SECTORS   2048

asm("romtag:                                \n"
    "       dc.w    "XSTR(RTC_MATCHWORD)"   \n"
    "       dc.l    romtag                  \n"
    "       dc.l    endcode                 \n"
    "       dc.b    "XSTR(RTF_AUTOINIT)"    \n"
    "       dc.b    "XSTR(DEVICE_VERSION)"  \n"
    "       dc.b    "XSTR(NT_DEVICE)"       \n"
    "       dc.b    "XSTR(DEVICE_PRIORITY)" \n"
    "       dc.l    _device_name            \n"
    "       dc.l    _device_id_string       \n"
    "       dc.l    _auto_init_tables       \n"
    "endcode:                               \n");

int __attribute__((no_reorder)) _start() {
    return -1;
}

const char device_name[] = DEVICE_NAME;
const char device_id_string[] = DEVICE_ID_STRING;

#define debug(x,args...) while(0){}
#define kprintf(x,args...) while(0){}

static void zzsd_reset(void *boardaddr) {
    volatile uint16_t *status = (volatile uint16_t *)((uint8_t *)boardaddr + ZZSD_STATUS);
    *status = 0;
}

static uint32_t zzsd_capacity_or_default(void *boardaddr) {
    uint32_t capacity = zzsd_capacity(boardaddr);
    if (capacity == 0) capacity = SD_CYL_SECTORS * SD_CYLS;
    return capacity;
}

void SD_InitUnit(struct SDBase* DevBase, int id, uint8_t* boardaddr);
uint32_t SD_ReadWrite(struct SDBase *DevBase, struct SDUnit *sdu, struct IORequest *io, uint32_t offset, uint32_t offset_hi, BOOL is_write);
LONG SD_PerformIO(struct SDBase *DevBase, struct SDUnit *sdu, struct IORequest *io);
LONG SD_PerformSCSI(struct SDBase *DevBase, struct SDUnit *sdu, struct IORequest *io);

static struct Library __attribute__((used)) *init_device(uint8_t *seg_list asm("a0"), struct Library *dev asm("d0")) {
    struct Library* ExpansionBase;
    struct ConfigDev* cd = NULL;
    uint8_t* boardaddr = NULL;
    uint32_t i;
    struct SDBase* DevBase;

    SysBase = *(struct ExecBase **)4L;
    if (!(ExpansionBase = (struct Library*)OpenLibrary((uint8_t*)"expansion.library",0L))) return 0;

    if ((cd = (struct ConfigDev*)FindConfigDev(cd,0x6d6e,0x3))) {
        boardaddr = (uint8_t*)cd->cd_BoardAddr;
    } else if ((cd = (struct ConfigDev*)FindConfigDev(cd,0x6d6e,0x4))) {
        boardaddr = (uint8_t*)cd->cd_BoardAddr;
    } else {
        CloseLibrary(ExpansionBase);
        return 0;
    }

    DevBase = (struct SDBase*)dev;
    debugstr(boardaddr, "INIT dev=");
    debughex(boardaddr, (uint32_t)dev);
    debugstr(boardaddr, "seg=");
    debughex(boardaddr, (uint32_t)seg_list);

    if (!DevBase) {
        debugstr(boardaddr, "INIT: NULL dev!\r\n");
        CloseLibrary(ExpansionBase);
        return 0;
    }

    dev->lib_Node.ln_Type = NT_DEVICE;
    dev->lib_Node.ln_Name = (char *)device_name;
    dev->lib_Version = DEVICE_VERSION;
    dev->lib_Revision = DEVICE_REVISION;
    dev->lib_IdString = (char *)device_id_string;

    DevBase->sd_Device = (struct Device*)dev;
    InitSemaphore(&DevBase->sd_Lock);
    cd->cd_Driver = (struct Device *)dev;

    for (i = 0; i < SD_UNITS; i++) SD_InitUnit(DevBase, i, boardaddr);
    zzsd_boot_init(boardaddr, DevBase, cd);

    CloseLibrary(ExpansionBase);
    debugstr(boardaddr, "INIT returning dev\r\n");
    return dev;
}

static uint8_t* __attribute__((used)) expunge(struct Library *dev asm("a6")) {
    return 0;
}

static void __attribute__((used)) open(struct Library *dev asm("a6"), struct IOExtTD *iotd asm("a1"), uint32_t unitnum asm("d0"), uint32_t flags asm("d1")) {
    int io_err = IOERR_OPENFAIL;
    struct SDBase* DevBase = (struct SDBase*)dev;

    if (DevBase && DevBase->sd_Unit[0].sdu_Registers) {
        debugstr(DevBase->sd_Unit[0].sdu_Registers, "OPEN u=");
        debughex(DevBase->sd_Unit[0].sdu_Registers, unitnum);
        debugstr(DevBase->sd_Unit[0].sdu_Registers, " fl=");
        debughex(DevBase->sd_Unit[0].sdu_Registers, flags);
        debugstr(DevBase->sd_Unit[0].sdu_Registers, "\r\n");
    }

    if (iotd) {
        if (unitnum == 0 && DevBase && DevBase->sd_Unit[0].sdu_Registers) {
            io_err = 0;
            iotd->iotd_Req.io_Unit = (struct Unit*)&DevBase->sd_Unit[unitnum];
            iotd->iotd_Req.io_Unit->unit_flags = UNITF_ACTIVE;
            iotd->iotd_Req.io_Unit->unit_OpenCnt = 1;
            ((struct Library *)DevBase->sd_Device)->lib_OpenCnt++;
        } else if (unitnum != 0) {
            /* HDToolBox scans multiple units/LUNs. Returning TDERR_BadUnitNum
               tells it "this unit doesn't exist" so it keeps scanning.
               Returning IOERR_OPENFAIL would make it stop scanning entirely. */
            io_err = TDERR_BadUnitNum;
        }
        iotd->iotd_Req.io_Error = io_err;
    }
}

static uint8_t* __attribute__((used)) close(struct Library *dev asm("a6"), struct IOExtTD *iotd asm("a1")) {
    struct SDBase* DevBase = (struct SDBase*)dev;
    ((struct Library *)DevBase->sd_Device)->lib_OpenCnt--;
    return 0;
}

static void __attribute__((used)) begin_io(struct Library *dev asm("a6"), struct IORequest *io asm("a1")) {
    struct SDBase* DevBase = (struct SDBase*)dev;
    struct SDUnit* sdu;

    if (!DevBase || !io) return;
    sdu = (struct SDUnit*)io->io_Unit;
    if (!sdu) {
        io->io_Error = IOERR_OPENFAIL;
        if (!(io->io_Flags & IOF_QUICK)) ReplyMsg(&io->io_Message);
        return;
    }

    if (DevBase->sd_Unit[0].sdu_Registers) {
        debugstr(DevBase->sd_Unit[0].sdu_Registers, "IO c=");
        debughex(DevBase->sd_Unit[0].sdu_Registers, io->io_Command);
    }

    ObtainSemaphore(&DevBase->sd_Lock);
    io->io_Error = SD_PerformIO(DevBase, sdu, io);
    ReleaseSemaphore(&DevBase->sd_Lock);

    if (io->io_Error && DevBase->sd_Unit[0].sdu_Registers) {
        debugstr(DevBase->sd_Unit[0].sdu_Registers, " err=");
        debughex(DevBase->sd_Unit[0].sdu_Registers, io->io_Error);
        debugstr(DevBase->sd_Unit[0].sdu_Registers, "\r\n");
    }

    if (!(io->io_Flags & IOF_QUICK)) ReplyMsg(&io->io_Message);
}

static uint32_t __attribute__((used)) abort_io(struct Library *dev asm("a6"), struct IORequest *io asm("a1")) {
    if (!io) return IOERR_NOCMD;
    io->io_Error = IOERR_ABORTED;
    return IOERR_ABORTED;
}

void SD_InitUnit(struct SDBase* DevBase, int id, uint8_t* boardaddr) {
    struct SDUnit *sdu = &DevBase->sd_Unit[id];
    if (id == 0) {
        sdu->sdu_Registers = (void*)boardaddr;
        sdu->sdu_Enabled = 1;
        sdu->sdu_Present = 1;
        sdu->sdu_Valid = 1;
        sdu->sdu_ChangeNum++;
        zzsd_reset(boardaddr);
    }
}

uint32_t SD_ReadWrite(struct SDBase *DevBase, struct SDUnit *sdu, struct IORequest *io, uint32_t offset, uint32_t offset_hi, BOOL is_write) {
    struct IOStdReq *iostd = (struct IOStdReq *)io;
    struct IOExtTD *iotd = (struct IOExtTD *)io;
    uint8_t* data;
    uint32_t len;
    uint32_t num_blocks;
    uint32_t block;
    uint16_t sderr;

    if (!sdu || !io) return 0;
    data = iotd->iotd_Req.io_Data;
    len = iotd->iotd_Req.io_Length;

    if (data == 0) return IOERR_BADADDRESS;
    if (len < SD_SECTOR_BYTES) {
        iostd->io_Actual = 0;
        return IOERR_BADLENGTH;
    }

    block = (offset >> SD_SECTOR_SHIFT) | (offset_hi << (32 - SD_SECTOR_SHIFT));
    block += DevBase->sd_BaseOffset;
    num_blocks = len >> SD_SECTOR_SHIFT;
    sderr = 0;

    if (is_write) {
        uint32_t retries = SD_RETRY;
        do {
            sderr = zzsd_write_blocks(sdu->sdu_Registers, data, block, num_blocks);
            if (sderr) zzsd_reset(sdu->sdu_Registers);
            retries--;
        } while (sderr && retries > 0);
    } else {
        uint32_t retries = SD_RETRY;
        do {
            sderr = zzsd_read_blocks(sdu->sdu_Registers, data, block, num_blocks);
            if (sderr) zzsd_reset(sdu->sdu_Registers);
            retries--;
        } while (sderr && retries > 0);
    }

    if (sderr) {
        iostd->io_Actual = 0;
        if (sderr & SDERRF_TIMEOUT) return TDERR_DiskChanged;
        if (sderr & SDERRF_PARAM) return TDERR_SeekError;
        return TDERR_SeekError;
    }

    iostd->io_Actual = len;
    return 0;
}

uint16_t ns_support[] = {
    NSCMD_DEVICEQUERY,
    CMD_RESET,
    CMD_READ,
    CMD_WRITE,
    CMD_UPDATE,
    CMD_CLEAR,
    CMD_START,
    CMD_STOP,
    CMD_FLUSH,
    TD_MOTOR,
    TD_SEEK,
    TD_FORMAT,
    TD_REMOVE,
    TD_CHANGENUM,
    TD_CHANGESTATE,
    TD_PROTSTATUS,
    TD_GETDRIVETYPE,
    TD_GETGEOMETRY,
    HD_SCSICMD,
    NSCMD_TD_READ64,
    NSCMD_TD_WRITE64,
    NSCMD_TD_SEEK64,
    NSCMD_TD_FORMAT64,
    0,
};

LONG SD_PerformIO(struct SDBase *DevBase, struct SDUnit *sdu, struct IORequest *io) {
    struct IOStdReq *iostd = (struct IOStdReq *)io;
    struct IOExtTD *iotd = (struct IOExtTD *)io;
    uint32_t offset, offset_hi;
    uint32_t err = IOERR_NOCMD;

    if (!io || !sdu) return err;
    if (!sdu->sdu_Enabled) return IOERR_OPENFAIL;
    if (io->io_Error == IOERR_ABORTED) return io->io_Error;

    switch (io->io_Command) {
    case NSCMD_DEVICEQUERY:
    {
        struct NSDeviceQueryResult *res = (struct NSDeviceQueryResult *)iotd->iotd_Req.io_Data;
        res->DevQueryFormat = 0;
        res->SizeAvailable = 16;
        res->DeviceType = NSDEVTYPE_TRACKDISK;
        res->DeviceSubType = 0;
        res->SupportedCommands = ns_support;
        iostd->io_Actual = 16;
        err = 0;
        break;
    }
    case CMD_CLEAR:
    case CMD_UPDATE:
    case TD_REMOVE:
    case TD_CHANGESTATE:
        iostd->io_Actual = 0;
        err = 0;
        break;
    case TD_PROTSTATUS:
        iostd->io_Actual = sdu->sdu_ReadOnly ? 1 : 0;
        err = 0;
        break;
    case TD_CHANGENUM:
        iostd->io_Actual = sdu->sdu_ChangeNum;
        err = 0;
        break;
    case TD_GETDRIVETYPE:
        iostd->io_Actual = DG_DIRECT_ACCESS;
        err = 0;
        break;
    case TD_GETGEOMETRY:
    {
        struct DriveGeometry *dg = (struct DriveGeometry *)iostd->io_Data;
        if (dg && iostd->io_Length >= sizeof(struct DriveGeometry)) {
            uint32_t capacity = DevBase->sd_PartBlocks;
            if (capacity == 0) capacity = zzsd_capacity_or_default(sdu->sdu_Registers);
            dg->dg_SectorSize = SD_SECTOR_BYTES;
            dg->dg_TotalSectors = capacity;
            dg->dg_Heads = 1;
            dg->dg_TrackSectors = 1;
            dg->dg_CylSectors = dg->dg_Heads * dg->dg_TrackSectors;
            dg->dg_Cylinders = capacity / dg->dg_CylSectors;
            dg->dg_BufMemType = MEMF_PUBLIC;
            dg->dg_DeviceType = DG_DIRECT_ACCESS;
            dg->dg_Flags = 0;
            iostd->io_Actual = sizeof(struct DriveGeometry);
            err = 0;
        } else {
            err = IOERR_BADLENGTH;
        }
        break;
    }
    case TD_MOTOR:
        iostd->io_Actual = sdu->sdu_Motor;
        sdu->sdu_Motor = iostd->io_Length ? 1 : 0;
        err = 0;
        break;
    case TD_FORMAT:
    case CMD_WRITE:
        offset = iotd->iotd_Req.io_Offset;
        err = SD_ReadWrite(DevBase, sdu, io, offset, 0, 1);
        break;
    case CMD_READ:
        offset = iotd->iotd_Req.io_Offset;
        err = SD_ReadWrite(DevBase, sdu, io, offset, 0, 0);
        break;
    case TD_WRITE64:
    case NSCMD_TD_WRITE64:
    case TD_FORMAT64:
    case NSCMD_TD_FORMAT64:
        offset = iotd->iotd_Req.io_Offset;
        offset_hi = iotd->iotd_Req.io_Actual;
        err = SD_ReadWrite(DevBase, sdu, io, offset, offset_hi, 1);
        break;
    case TD_READ64:
    case NSCMD_TD_READ64:
        offset = iotd->iotd_Req.io_Offset;
        offset_hi = iotd->iotd_Req.io_Actual;
        err = SD_ReadWrite(DevBase, sdu, io, offset, offset_hi, 0);
        break;
    case HD_SCSICMD:
        err = SD_PerformSCSI(DevBase, sdu, io);
        break;
    default:
        err = IOERR_NOCMD;
        break;
    }

    return err;
}

LONG SD_PerformSCSI(struct SDBase *DevBase, struct SDUnit *sdu, struct IORequest *io) {
    struct IOStdReq *iostd = (struct IOStdReq *)io;
    struct SCSICmd *scsi = iostd->io_Data;
    uint8_t* boardaddr = sdu->sdu_Registers;
    uint8_t* data = (uint8_t*)scsi->scsi_Data;
    uint32_t i, block, blocks, maxblocks;
    long err;
    uint8_t r1;

    maxblocks = DevBase->sd_PartBlocks;
    if (maxblocks == 0) maxblocks = zzsd_capacity_or_default(boardaddr);
    if (scsi->scsi_CmdLength < 6) return IOERR_BADLENGTH;
    if (scsi->scsi_Command == NULL) return IOERR_BADADDRESS;
    scsi->scsi_Actual = 0;

    switch (scsi->scsi_Command[0]) {
    case 0x00:
        err = 0;
        break;
    case 0x12:
        for (i = 0; i < scsi->scsi_Length; i++) {
            uint8_t val;
            switch (i) {
            case 0: val = 0; break;
            case 1: val = (1 << 7); break;
            case 2: val = 0; break;
            case 3: val = 2; break;
            case 4: val = 31; break;
            default:
                if (i >= 8 && i < 16) val = "MNT     "[i - 8];
                else if (i >= 16 && i < 32) val = "ZZ9000 SD Disk  "[i - 16];
                else if (i >= 32 && i < 36) val = "1.9 "[i - 32];
                else if (i >= 36 && i < 44) val = '1';
                else val = 0;
                break;
            }
            data[i] = val;
        }
        scsi->scsi_Actual = i;
        err = 0;
        break;
    case 0x08:
        block = scsi->scsi_Command[1] & 0x1f;
        block = (block << 8) | scsi->scsi_Command[2];
        block = (block << 8) | scsi->scsi_Command[3];
        blocks = scsi->scsi_Command[4];
        if (block + blocks > maxblocks) { err = IOERR_BADADDRESS; break; }
        if (scsi->scsi_Length < (blocks << SD_SECTOR_SHIFT)) { err = IOERR_BADLENGTH; break; }
        if (data == NULL) { err = IOERR_BADADDRESS; break; }
        block += DevBase->sd_BaseOffset;
        {
            uint32_t retries = SD_RETRY;
            do {
                r1 = zzsd_read_blocks(boardaddr, data, block, blocks);
                if (r1) zzsd_reset(boardaddr);
                retries--;
            } while (r1 && retries > 0);
        }
        if (r1) { err = HFERR_BadStatus; break; }
        scsi->scsi_Actual = scsi->scsi_Length;
        err = 0;
        break;
    case 0x0a:
        block = scsi->scsi_Command[1] & 0x1f;
        block = (block << 8) | scsi->scsi_Command[2];
        block = (block << 8) | scsi->scsi_Command[3];
        blocks = scsi->scsi_Command[4];
        if (block + blocks > maxblocks) { err = IOERR_BADADDRESS; break; }
        if (scsi->scsi_Length < (blocks << SD_SECTOR_SHIFT)) { err = IOERR_BADLENGTH; break; }
        if (data == NULL) { err = IOERR_BADADDRESS; break; }
        block += DevBase->sd_BaseOffset;
        {
            uint32_t retries = SD_RETRY;
            do {
                r1 = zzsd_write_blocks(boardaddr, data, block, blocks);
                if (r1) zzsd_reset(boardaddr);
                retries--;
            } while (r1 && retries > 0);
        }
        if (r1) { err = HFERR_BadStatus; break; }
        scsi->scsi_Actual = scsi->scsi_Length;
        err = 0;
        break;
    case 0x25:
        if (scsi->scsi_CmdLength < 10) { err = HFERR_BadStatus; break; }
        block = *((uint32_t*)&scsi->scsi_Command[2]);
        if ((scsi->scsi_Command[8] & 1) || block != 0) { err = HFERR_BadStatus; break; }
        if (scsi->scsi_Length < 8) { err = IOERR_BADLENGTH; break; }
        ((uint32_t*)data)[0] = maxblocks - 1;
        ((uint32_t*)data)[1] = SD_SECTOR_BYTES;
        scsi->scsi_Actual = 8;
        err = 0;
        break;
    case 0x1a:
        data[0] = 3 + 8 + 0x16;
        data[1] = 0;
        data[2] = 0;
        data[3] = 8;
        if (maxblocks > (1 << 24)) blocks = 0xffffff; else blocks = maxblocks;
        data[4] = (blocks >> 16) & 0xff;
        data[5] = (blocks >> 8) & 0xff;
        data[6] = blocks & 0xff;
        data[7] = 0;
        data[8] = 0;
        data[9] = 0;
        data[10] = (SD_SECTOR_BYTES >> 8) & 0xff;
        data[11] = SD_SECTOR_BYTES & 0xff;
        switch (((UWORD)scsi->scsi_Command[2] << 8) | scsi->scsi_Command[3]) {
        case 0x0300:
            for (i = 0; i < scsi->scsi_Length - 12; i++) {
                UBYTE val;
                switch (i) {
                case 0: val = 0x03; break;
                case 1: val = 0x16; break;
                case 2: val = (SD_HEADS >> 8) & 0xff; break;
                case 3: val = SD_HEADS & 0xff; break;
                case 10: val = (SD_TRACK_SECTORS >> 8) & 0xff; break;
                case 11: val = SD_TRACK_SECTORS & 0xff; break;
                case 12: val = (SD_SECTOR_BYTES >> 8) & 0xff; break;
                case 13: val = SD_SECTOR_BYTES & 0xff; break;
                case 20: val = (1 << 6) | (1 << 5); break;
                default: val = 0; break;
                }
                data[12 + i] = val;
            }
            scsi->scsi_Actual = data[0] + 1;
            err = 0;
            break;
        case 0x0400:
        {
            uint32_t numcyls = maxblocks >> 23;
            for (i = 0; i < scsi->scsi_Length - 12; i++) {
                UBYTE val;
                switch (i) {
                case 0: val = 0x04; break;
                case 1: val = 0x16; break;
                case 2: val = (numcyls >> 16) & 0xff; break;
                case 3: val = (numcyls >> 8) & 0xff; break;
                case 4: val = numcyls & 0xff; break;
                case 5: val = SD_HEADS; break;
                default: val = 0; break;
                }
                data[12 + i] = val;
            }
            scsi->scsi_Actual = data[0] + 1;
            err = 0;
            break;
        }
        default:
            err = HFERR_BadStatus;
            break;
        }
        break;
    default:
        err = IOERR_NOCMD;
        break;
    }

    if (err == 0) iostd->io_Actual = sizeof(*scsi);
    else iostd->io_Actual = 0;
    return err;
}

static uint32_t device_vectors[] = {
    (uint32_t)open,
    (uint32_t)close,
    (uint32_t)expunge,
    0,
    (uint32_t)begin_io,
    (uint32_t)abort_io,
    -1
};

const uint32_t auto_init_tables[4] = {
    sizeof(struct SDBase),
    (uint32_t)device_vectors,
    0,
    (uint32_t)init_device
};
