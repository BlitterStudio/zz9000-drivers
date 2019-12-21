#ifndef MNTSD_H
#define MNTSD_H

#define uint32 unsigned long
#define uint8 unsigned char
#define uint16 unsigned short

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
  volatile uint16 tx_hi; // d0
  volatile uint16 tx_lo; // d2
  volatile uint16 rx_hi; // d4
  volatile uint16 rx_lo; // d6
  volatile uint16 status; // d8
  volatile uint16 bufsel; // da
  volatile uint32 capacity; // dc
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

uint16 sdcmd_read_blocks(void* registers, uint8* data, uint32 block, uint32 len);
uint16 sdcmd_write_blocks(void* registers, uint8* data, uint32 block, uint32 len);
uint16 sdcmd_present();
uint16 sdcmd_detect();
uint32 sdcmd_capacity(void* registers);
void sd_reset(void* regs);

#endif
