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

#include <string.h>
#include <stdint.h>

#include "mntsd_cmd.h"

void sd_reset(void* registers) {
  volatile struct MNTUSBSRegs* regs = (volatile struct MNTUSBSRegs*)registers;
  regs->status = 0; // reset command
}

#define BLOCKS_AT_ONCE 32

uint16_t sdcmd_read_blocks(void* registers, uint8_t* data, uint32_t block, uint32_t len) {
  uint32_t i=0;
  uint32_t offset=0;
  uint32_t num_blocks=1;
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

    CopyMem(registers-0xd0+0xa000, data+offset, num_blocks*512);

    i += num_blocks;
  }

  return 0;
}

uint16_t sdcmd_write_blocks(void* registers, uint8_t* data, uint32_t block, uint32_t len) {
  uint32_t i=0;
  uint32_t offset=0;
  uint32_t num_blocks=1;
  struct MNTUSBSRegs* regs = (struct MNTUSBSRegs*)registers;

  while (i<len) {
    offset = i<<SD_SECTOR_SHIFT;

    num_blocks = BLOCKS_AT_ONCE;
    if ((len-i)<BLOCKS_AT_ONCE) {
      num_blocks = len-i;
    }

    CopyMem(data+offset, registers-0xd0+0xa000, num_blocks*512);

    // TODO: do we need a fence here?
    //CacheClearU();

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

uint16_t sdcmd_present() {
  // FIXME
  return 1;
}

uint16_t sdcmd_detect() {
  return 0;
}

uint32_t sdcmd_capacity(void* registers) {
  struct MNTUSBSRegs* regs = (struct MNTUSBSRegs*)registers;

  if (regs->status == 0) {
    return 0;
  }
  return regs->capacity;
}
