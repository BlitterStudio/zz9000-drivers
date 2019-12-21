#include <exec/resident.h>
#include <exec/errors.h>
#include <exec/memory.h>
#include <exec/lists.h>
#include <exec/alerts.h>
#include <exec/tasks.h>
#include <exec/io.h>

#include <libraries/expansion.h>

#include <devices/trackdisk.h>
#include <devices/timer.h>
#include <devices/scsidisk.h>

#include <dos/filehandler.h>

#include <proto/exec.h>
#include <proto/disk.h>
#include <proto/expansion.h>

#include "mntsd_cmd.h"

//#define bug(x,args...) kprintf(x ,##args);
//#define debug(x,args...) bug("%s:%ld " x "\n", __func__, (unsigned long)__LINE__ ,##args)

void sd_reset(void* registers) {
  volatile struct MNTUSBSRegs* regs = (volatile struct MNTUSBSRegs*)registers;
  regs->status = 0; // reset command
}

#define BLOCKS_AT_ONCE 32

uint16 sdcmd_read_blocks(void* registers, uint8* data, uint32 block, uint32 len) {
  uint32 i=0, j=0;
  uint32 offset=0;
  uint32 num_blocks=1;
  volatile struct MNTUSBSRegs* regs = (volatile struct MNTUSBSRegs*)registers;
  
  while (i<len) {
    offset = i<<SD_SECTOR_SHIFT;

    num_blocks = BLOCKS_AT_ONCE;
    if ((len-i)<BLOCKS_AT_ONCE) {
      num_blocks = len-i;
    }

    regs->status = num_blocks;
    regs->rx_hi = ((block+i)>>16);
    regs->rx_lo = (block+i)&0xffff;

    // FIXME: more specific error?
    if (regs->status == 0) {
      return SDERRF_PARAM;
    }

    for (j=0; j<num_blocks; j++) {
      regs->bufsel = j;
      memcpy(data+offset+(j<<SD_SECTOR_SHIFT), registers-0xd0+0xa000, 512);
    }

    i += num_blocks;
  }
  return 0;
}

uint16 sdcmd_write_blocks(void* registers, uint8* data, uint32 block, uint32 len) {
  uint32 i=0, j=0;
  uint32 offset=0;
  uint16 status=0;
  uint32 num_blocks=1;
  struct MNTUSBSRegs* regs = (struct MNTUSBSRegs*)registers;
  
  while (i<len) {
    offset = i<<SD_SECTOR_SHIFT;
    
    num_blocks = BLOCKS_AT_ONCE;
    if ((len-i)<BLOCKS_AT_ONCE) {
      num_blocks = len-i;
    }

    for (j=0; j<num_blocks; j++) {
      regs->bufsel = j;
      memcpy(registers-0xd0+0xa000, data+offset+(j<<SD_SECTOR_SHIFT), 512);   
    }
    
    regs->status = num_blocks;
    regs->tx_hi = ((block+i)>>16);
    regs->tx_lo = (block+i)&0xffff;

    // FIXME: more specific error?
    if (regs->status == 0) {
      return SDERRF_PARAM;
    }
    
    i += num_blocks;
  }
  return 0;
}

uint16 sdcmd_present() {
  // FIXME
  return 1;
}

uint16 sdcmd_detect() {
  return 0;
}

uint32 sdcmd_capacity(void* registers) {
  struct MNTUSBSRegs* regs = (struct MNTUSBSRegs*)registers;
  
  if (regs->status == 0) {
    return 0;
  }
  return regs->capacity;
}
