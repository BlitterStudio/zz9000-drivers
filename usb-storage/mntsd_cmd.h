#ifndef MNTSD_H
#define MNTSD_H

#define SD_HEADS         1
#define SD_CYLS          4096 /* 4096 MB */
#define SD_TRACK_SECTORS 2048 /* 1 MB */
#define SD_CYL_SECTORS   2048 /* TRACK_SECTORS * HEADS */
#define SD_SECTOR_BYTES  512
#define SD_SECTOR_SHIFT  9
#define SD_RETRY         10

#define SDERRF_TIMEOUT  (1 << 7)
#define SDERRF_PARAM    (1 << 6)
#define SDERRF_ADDRESS  (1 << 5)
#define SDERRF_ERASESEQ (1 << 4)
#define SDERRF_CRC      (1 << 3)
#define SDERRF_ILLEGAL  (1 << 2)
#define SDERRF_ERASERES (1 << 1)
#define SDERRF_IDLE     (1 << 0)

#define SD_UNITS    1       /* Only one chip select for now */

#define SDU_STACK_SIZE  (4096 / sizeof(ULONG))

#define CMD_NAME(x) if (cmd == x) return #x
#define TD_READ64     24
#define TD_WRITE64    25
#define TD_SEEK64     26
#define TD_FORMAT64   27

struct MNTUSBSRegs {
  volatile uint16_t tx_hi; // d0
  volatile uint16_t tx_lo; // d2
  volatile uint16_t rx_hi; // d4
  volatile uint16_t rx_lo; // d6
  volatile uint16_t status; // d8
  volatile uint16_t bufsel; // da
  volatile uint32_t capacity; // dc
};

struct SDBase {
  struct Device* sd_Device;
  struct SDUnit {
    struct Unit sdu_Unit;

    /*struct Unit {
        struct  MsgPort unit_MsgPort;
        UBYTE   unit_flags;
                UNITF_ACTIVE = 1
                UNITF_INTASK = 2
        UBYTE   unit_pad;
        UWORD   unit_OpenCnt;
    };*/

    BOOL        sdu_Enabled;

    void* sdu_Registers;

    BOOL sdu_Present;               /* Is a device detected? */
    BOOL sdu_Valid;                 /* Is the device ready for IO? */
    BOOL sdu_ReadOnly;              /* Is the device read-only? */
    BOOL sdu_Motor;                 /* TD_MOTOR state */
    ULONG sdu_ChangeNum;
  } sd_Unit[SD_UNITS];
};

uint16_t sdcmd_read_blocks(void* registers, uint8_t* data, uint32_t block, uint32_t len);
uint16_t sdcmd_write_blocks(void* registers, uint8_t* data, uint32_t block, uint32_t len);
uint16_t sdcmd_present();
uint16_t sdcmd_detect();
uint32_t sdcmd_capacity(void* registers);
void sd_reset(void* regs);

void debugstr(void* regs, char* str);
void debughex(void* regs, uint32_t val);

/*------------------------------------------------------------------------*/
/*
 * $Id: newstyle.h 1.1 1997/05/15 18:53:15 heinz Exp $
 *
 * Support header for the New Style Device standard
 *
 * (C)1996-1997 by Amiga International, Inc.
 *
 */
/*------------------------------------------------------------------------*/

#define     NSCMD_DEVICEQUERY       0x4000
#define     NSDEVTYPE_TRACKDISK     5   /* like trackdisk.device */

struct NSDeviceQueryResult {
    /*
    ** Standard information, must be reset for every query
    */
    ULONG   DevQueryFormat;         /* this is type 0               */
    ULONG   SizeAvailable;          /* bytes available              */

    /*
    ** Common information (READ ONLY!)
    */
    UWORD   DeviceType;             /* what the device does         */
    UWORD   DeviceSubType;          /* depends on the main type     */
    UWORD   *SupportedCommands;     /* 0 terminated list of cmd's   */

    /* May be extended in the future! Check SizeAvailable! */
};

#define NSCMD_TD_READ64     0xC000
#define NSCMD_TD_WRITE64    0xC001
#define NSCMD_TD_SEEK64     0xC002
#define NSCMD_TD_FORMAT64   0xC003

#endif
