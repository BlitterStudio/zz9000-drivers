/*
 * MNT ZZ9000AX Amiga MP3 Player Example (Hardware Accelerated)
 *
 * Copyright (C) 2022, Lukas F. Hartmann <lukas@mntre.com>
 *                     MNT Research GmbH, Berlin
 *                     https://mntre.com
 * Additional work contributed by Thomas Wenzel
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * GNU General Public License v3.0 or later
 *
 * https://spdx.org/licenses/GPL-3.0-or-later.html
 */

#include <exec/exec.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/dostags.h>
#include <utility/hooks.h>
#include <hardware/intbits.h>

#include <clib/alib_protos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/expansion.h>

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <limits.h>

#define DEVF_INT2MODE 1

static const char version[]  __attribute__((used)) = "$VER: axmp3 1.12\n\r";
static const char procname[] __attribute__((used)) = "axmp3";

#define ZZ_BYTES_PER_PERIOD 3840
#define AUDIO_BUFSZ ZZ_BYTES_PER_PERIOD*8 // TODO: query from hardware
#define WORKER_PRIORITY 127 // 19 would be nicer

#define REG_ZZ_CONFIG        0x04
#define REG_ZZ_AUDIO_SWAB    0x70
#define REG_ZZ_DECODER_FIFO  0x72
#define REG_ZZ_AUDIO_SCALE   0x74
#define REG_ZZ_AUDIO_PARAM   0x76
#define REG_ZZ_AUDIO_VAL     0x78
#define REG_ZZ_DECODER_PARAM 0x7A
#define REG_ZZ_DECODER_VAL   0x7C
#define REG_ZZ_DECODE        0x7E
#define REG_ZZ_AUDIO_CONFIG  0xF4

#define ID3V2_HEADER_LENGTH 10

//#define FIFOSIZE 1152*4
#define FIFOSIZE (16*1024+1)

typedef enum {
	FIFO_PREFILL,
	FIFO_OPERATIONAL
} FIFO_MODE;

typedef enum {
	DECODE_CLEAR,
	DECODE_INIT,
	DECODE_RUN
} DECODE_COMMAND;

#define BSWAP_S(x) ((uint16_t) ((((x) >> 8) & 0xff) | (((x) & 0xff) << 8)))
#define BSWAP_L(x) (((((uint32_t)x) & 0xff000000u) >> 24) | ((((uint32_t)x) & 0x00ff0000u) >> 8) | ((((uint32_t)x) & 0x0000ff00u) << 8) | ((((uint32_t)x) & 0x000000ffu) << 24))
#define BSWAP_P(x) (void*)(((((uint32_t)x) & 0xff000000u) >> 24) | ((((uint32_t)x) & 0x00ff0000u) >> 8) | ((((uint32_t)x) & 0x0000ff00u) << 8) | ((((uint32_t)x) & 0x000000ffu) << 24))

struct z9ax {
  struct Task *t_mainproc;
  struct Process *worker_process;
  struct Interrupt irq;
  uint32_t hw_addr;
  int8_t mainproc_signal;
  int8_t enable_signal;
  int8_t worker_signal;
  uint8_t zorro_version;

  uint32_t mp3_addr;
  size_t mp3_bytes;
  uint32_t encoded_offset;
  uint32_t decode_offset;
  uint32_t decode_chunk_sz;

  uint8_t flags;
  FILE* mp3_file;
};


struct z9ax glob_ax;

/* ************ */
/*  BEGIN FIFO  */
/* ************ */
// We are the master of FifoWriteIdx.
static unsigned short FifoWriteIdx = 0;

// Clear FIFO on both sides.
static void clearFifo(unsigned long aHWAddr) {
	FifoWriteIdx = 0;
	// ZZ_DECODE (clear)
	*((volatile uint16_t*)(aHWAddr+REG_ZZ_DECODE)) = DECODE_CLEAR;
}

// Fill FIFO by reading from aFile.
static void fillFifo(FILE *aFile, unsigned long aHWAddr, FIFO_MODE aMode) {
	static unsigned char FileReadBuffer[FIFOSIZE]; // This can be huge. Better leave it on the heap.
	unsigned char *Buffer = (unsigned char *)glob_ax.mp3_addr;
	long Space = 0;
	unsigned short FifoReadIdx;
	unsigned long i;

	// 1. Get FIFO Read Index from ZZ9k (we are the slave).
	FifoReadIdx = *((volatile uint16_t*)(aHWAddr+REG_ZZ_DECODER_FIFO));
	if(FifoWriteIdx >= FifoReadIdx) {
		Space = FIFOSIZE-(FifoWriteIdx-FifoReadIdx);
	}
	else {
		Space = FifoReadIdx-FifoWriteIdx;
	}

	// 2. Calculate space left in FIFO.
	// In prefill mode fill the FIFO completely.
	if(aMode == FIFO_PREFILL) {
		Space -= 1; // Note: Fill level limited for technical rasons.
	}
	// In operational mode fill it only half way to leave data for seeking back.
	else {
		Space -= FIFOSIZE/2;
	}
	if(Space <= 0) return;

	// 3. Fill the FIFO
	size_t mp3_bytes_read = fread(FileReadBuffer, 1, Space, aFile);

	for(i=0; i<mp3_bytes_read; i++) {
		if(Space) {
			Buffer[FifoWriteIdx++] = FileReadBuffer[i];
			if(FifoWriteIdx >= FIFOSIZE) FifoWriteIdx = 0;
			Space--;
		}
	}

	// 4. Set FIFO Write Index in ZZ9k (we are the master).
	*((volatile uint16_t*)(aHWAddr+REG_ZZ_DECODER_FIFO)) = FifoWriteIdx;
}
/* ********** */
/*  END FIFO  */
/* ********** */

void WorkerProcess()
{
  struct Process* proc = (struct Process *) FindTask(NULL);
  struct z9ax* ax = proc->pr_Task.tc_UserData;

  ax->worker_signal = AllocSignal(-1);
  ax->enable_signal = AllocSignal(-1);

  uint32_t signals = 0;
  uint32_t buf_offset = 0;
  uint32_t buf_samples = ZZ_BYTES_PER_PERIOD/4;

  Signal(ax->t_mainproc, 1L << ax->mainproc_signal);

  for(;;) {
    signals = Wait(SIGBREAKF_CTRL_C | (1L << ax->enable_signal));
    if (signals & SIGBREAKF_CTRL_C) break;
    
    fillFifo(ax->mp3_file, ax->hw_addr, FIFO_OPERATIONAL);
    
    *((volatile uint16_t*)(ax->hw_addr+REG_ZZ_AUDIO_SCALE)) = buf_samples;

    *((volatile uint16_t*)(ax->hw_addr+REG_ZZ_DECODER_PARAM)) = 4;
    *((volatile uint16_t*)(ax->hw_addr+REG_ZZ_DECODER_VAL)) = (ax->decode_offset+buf_offset)>>16;
    *((volatile uint16_t*)(ax->hw_addr+REG_ZZ_DECODER_PARAM)) = 5;
    *((volatile uint16_t*)(ax->hw_addr+REG_ZZ_DECODER_VAL)) = (ax->decode_offset+buf_offset)&0xffff;
    *((volatile uint16_t*)(ax->hw_addr+REG_ZZ_DECODER_PARAM)) = 0;
    // ZZ_DECODE (task)
    *((volatile uint16_t*)(ax->hw_addr+REG_ZZ_DECODE)) = DECODE_RUN;

    // play buffer
    *((volatile uint16_t*)(ax->hw_addr+REG_ZZ_AUDIO_SWAB)) = (1<<15) | (buf_offset >> 8); // no byteswap, offset/256
    int overrun = *((volatile uint16_t*)(ax->hw_addr+REG_ZZ_AUDIO_SWAB));

    if (overrun == 1) {
      buf_offset = 0;
    } else {
      buf_offset += ZZ_BYTES_PER_PERIOD;
    }

    if (buf_offset >= AUDIO_BUFSZ) {
      buf_offset = 0;
    }

    // finished playing?
    int decoded = *((volatile uint16_t*)(ax->hw_addr+REG_ZZ_DECODE));
    if (!decoded) {
      Signal((struct Task *)ax->t_mainproc, SIGBREAKF_CTRL_C);
      break;
    }
  }

  FreeSignal(ax->enable_signal);
  ax->enable_signal = -1;
  FreeSignal(ax->worker_signal);
  ax->worker_signal = -1;
  Signal((struct Task *)ax->t_mainproc, 1L << ax->mainproc_signal);
}

uint32_t dev_isr(struct z9ax* ax asm("a1")) {
  USHORT status = *(USHORT*)(ax->hw_addr+REG_ZZ_CONFIG);

  // audio interrupt signal set?
  if (status & 2) {
    // ack/clear audio interrupt
    *(USHORT*)(ax->hw_addr+REG_ZZ_CONFIG) = 8|32;

    if (ax->worker_process) {
      Signal((struct Task*)ax->worker_process, 1L << ax->enable_signal);
    }
  }

  if (status == 2) {
    return 1;
  } else {
    return 0;
  }
}

void init_interrupt(struct z9ax* ax) {
  ax->irq.is_Node.ln_Type = NT_INTERRUPT;
  ax->irq.is_Node.ln_Pri = -60;
  ax->irq.is_Node.ln_Name = "ZZ9000AX DIRECT";
  ax->irq.is_Data = ax;
  ax->irq.is_Code = (void*)dev_isr;

  Forbid();
  if (ax->flags & DEVF_INT2MODE) {
    AddIntServer(INTB_PORTS, &ax->irq);
  } else {
    AddIntServer(INTB_EXTER, &ax->irq);
  }
  Permit();

  // enable HW interrupt
  *(volatile USHORT*)(ax->hw_addr + REG_ZZ_AUDIO_CONFIG) = 1;
}

void destroy_interrupt(struct z9ax* ax) {
  // disable HW interrupt
  *(volatile USHORT*)(ax->hw_addr + REG_ZZ_AUDIO_CONFIG) = 0;

  Forbid();
  if (ax->flags & DEVF_INT2MODE) {
    RemIntServer(INTB_PORTS, &ax->irq);
  } else {
    RemIntServer(INTB_EXTER, &ax->irq);
  }
  Permit();
}

uint32_t GetID3v2Length(unsigned char *ID3v2test, unsigned char ID3v2version) {
  uint32_t ID3v2length = 0;
  if((ID3v2test[0] == 0x49)
  && (ID3v2test[1] == 0x44)
  && (ID3v2test[2] == 0x33)
  && (ID3v2test[4] <  0xFF)
  && (ID3v2test[6] <  0x80)
  && (ID3v2test[7] <  0x80)
  && (ID3v2test[8] <  0x80)
  && (ID3v2test[9] <  0x80)) {
    if((ID3v2version == 0)
    || (ID3v2version == ID3v2test[3])) {
      ID3v2length = ((uint32_t)ID3v2test[6] << 21)
                  + ((uint32_t)ID3v2test[7] << 14)
                  + ((uint32_t)ID3v2test[8] << 7 )
                  + ((uint32_t)ID3v2test[9]      );
    }
  }
  return ID3v2length;
}

int main(int argc, char* argv[]) {
  uint8_t ID3v2test[ID3V2_HEADER_LENGTH];
  uint32_t ID3v2length;
  uint32_t DataOffset = 0;
  FILE* mp3_file;
  struct ConfigDev* cd;
  uint32_t hw_addr = 0;
  int zorro = 0;

  if ((ExpansionBase = (struct ExpansionBase*) OpenLibrary((STRPTR)"expansion.library", 0))) {
    // Find Z2 or Z3 model of MNT ZZ9000
    // TODO: query for ZZ9000AX
    if ((cd = (struct ConfigDev*)FindConfigDev(cd,0x6d6e,0x4))) {
      // ZORRO 3
      zorro = 3;
      fprintf(stderr, "ZZ9000 Zorro 3 Version detected.\n");
    }
    else if ((cd = (struct ConfigDev*)FindConfigDev(cd,0x6d6e,0x3))) {
      // ZORRO 2
      zorro = 2;
      fprintf(stderr, "ZZ9000 Zorro 2 Version detected.\n");
    } else {
      fprintf(stderr, "Error: ZZ9000 not detected.\n");
      exit(1);
    }
  } else {
    fprintf(stderr, "Error: Can't open expansion.library.\n");
    exit(1);
  }

  CloseLibrary((struct Library*)ExpansionBase);
  hw_addr = (uint32_t)cd->cd_BoardAddr;
  int ax_present = *((volatile uint16_t*)(hw_addr+REG_ZZ_AUDIO_CONFIG));
  if (ax_present) {
    fprintf(stderr, "ZZ9000AX detected.\n");
  } else {
    fprintf(stderr, "Error: ZZ9000AX not detected.\n");
    exit(1);
  }

  glob_ax.hw_addr = hw_addr;
  glob_ax.zorro_version = zorro;
  if (zorro == 3) {
    glob_ax.encoded_offset = 0x06000000;
  } else {
    glob_ax.encoded_offset = 0x100000;
  }
  glob_ax.decode_offset  = 0x07000000;
  glob_ax.decode_chunk_sz = 1920; // 16 bit sample pairs

  if (argc != 2) {
    fprintf(stderr, "Usage: %s soundfile.mp3\n", argv[0]);
    return RETURN_ERROR;
  }

  glob_ax.flags = 0;

  BPTR fh;
  if ((fh=Open((CONST_STRPTR)"ENV:ZZ9K_INT2",MODE_OLDFILE))) {
    printf("Using INT2 mode.\n");
    Close(fh);
    glob_ax.flags |= DEVF_INT2MODE;
  } else {
    printf("Using INT6 mode (default).\n");
  }

  mp3_file = fopen(argv[1], "rb");
  if (!mp3_file) {
    fprintf(stderr, "Error opening input file.\n");
    return RETURN_ERROR;
  }

  fseek(mp3_file, 0L, SEEK_END);
  size_t sz = ftell(mp3_file);
  rewind(mp3_file);

  if(fread(ID3v2test, 1, ID3V2_HEADER_LENGTH, mp3_file) == ID3V2_HEADER_LENGTH) {
    if((ID3v2length = GetID3v2Length(ID3v2test, 0))) {
      DataOffset = ID3V2_HEADER_LENGTH+ID3v2length;
	  fseek(mp3_file, DataOffset, SEEK_SET);
    }
  }

  fprintf(stderr, "File size: %u bytes, data offset: %zu bytes\n", sz, DataOffset);

  glob_ax.mp3_addr = hw_addr + 0x10000 + glob_ax.encoded_offset;
  glob_ax.mp3_file = mp3_file;
  glob_ax.mp3_bytes = sz;


  fprintf(stderr, "Prefilling FIFO...\n");
  clearFifo(hw_addr);
  fillFifo(mp3_file, hw_addr, FIFO_PREFILL);
  
  fprintf(stderr, "Playing...\n");

  // set tx buffer address to 127 MB offset
  *((volatile uint16_t*)(hw_addr+REG_ZZ_AUDIO_PARAM)) = 0;
  *((volatile uint16_t*)(hw_addr+REG_ZZ_AUDIO_VAL)) = glob_ax.decode_offset>>16;
  *((volatile uint16_t*)(hw_addr+REG_ZZ_AUDIO_PARAM)) = 1;
  *((volatile uint16_t*)(hw_addr+REG_ZZ_AUDIO_VAL)) = glob_ax.decode_offset&0xffff;

  // set LPF to 20KHz
  *((volatile uint16_t*)(hw_addr+REG_ZZ_AUDIO_PARAM)) = 9;
  *((volatile uint16_t*)(hw_addr+REG_ZZ_AUDIO_VAL)) = 20000;
  *((volatile uint16_t*)(hw_addr+REG_ZZ_AUDIO_PARAM)) = 0;

  // set decoder params
  *((volatile uint16_t*)(hw_addr+REG_ZZ_DECODER_PARAM)) = 0;
  *((volatile uint16_t*)(hw_addr+REG_ZZ_DECODER_VAL)) = glob_ax.encoded_offset>>16;
  *((volatile uint16_t*)(hw_addr+REG_ZZ_DECODER_PARAM)) = 1;
  *((volatile uint16_t*)(hw_addr+REG_ZZ_DECODER_VAL)) = glob_ax.encoded_offset&0xffff;
  *((volatile uint16_t*)(hw_addr+REG_ZZ_DECODER_PARAM)) = 2;
  *((volatile uint16_t*)(hw_addr+REG_ZZ_DECODER_VAL)) = FIFOSIZE>>16;
  *((volatile uint16_t*)(hw_addr+REG_ZZ_DECODER_PARAM)) = 3;
  *((volatile uint16_t*)(hw_addr+REG_ZZ_DECODER_VAL)) = FIFOSIZE&0xffff;
  *((volatile uint16_t*)(hw_addr+REG_ZZ_DECODER_PARAM)) = 4;
  *((volatile uint16_t*)(hw_addr+REG_ZZ_DECODER_VAL)) = glob_ax.decode_offset>>16;
  *((volatile uint16_t*)(hw_addr+REG_ZZ_DECODER_PARAM)) = 5;
  *((volatile uint16_t*)(hw_addr+REG_ZZ_DECODER_VAL)) = glob_ax.decode_offset&0xffff;
  *((volatile uint16_t*)(hw_addr+REG_ZZ_DECODER_PARAM)) = 6;
  *((volatile uint16_t*)(hw_addr+REG_ZZ_DECODER_VAL)) = glob_ax.decode_chunk_sz>>16;
  *((volatile uint16_t*)(hw_addr+REG_ZZ_DECODER_PARAM)) = 7;
  *((volatile uint16_t*)(hw_addr+REG_ZZ_DECODER_VAL)) = glob_ax.decode_chunk_sz&0xffff;
  *((volatile uint16_t*)(hw_addr+REG_ZZ_DECODER_PARAM)) = 0;

  // ZZ_DECODE (init)
  *((volatile uint16_t*)(hw_addr+REG_ZZ_DECODE)) = DECODE_INIT;

  glob_ax.t_mainproc = FindTask(NULL);
  glob_ax.mainproc_signal = AllocSignal(-1);

  if (glob_ax.mainproc_signal != -1) {
    Forbid();
    if (glob_ax.worker_process = CreateNewProcTags(NP_Entry, (uint32_t)&WorkerProcess,
                                              NP_Name, (uint32_t)procname,
                                              NP_Priority, WORKER_PRIORITY,
                                              TAG_DONE)) {
      glob_ax.worker_process->pr_Task.tc_UserData = &glob_ax;
    }
    Permit();

    if (glob_ax.worker_process) {
      Wait(1L << glob_ax.mainproc_signal);
      init_interrupt(&glob_ax);
  
      Wait(SIGBREAKF_CTRL_C);
  
      Signal((struct Task *)glob_ax.worker_process, SIGBREAKF_CTRL_C);
      Wait(1L << glob_ax.mainproc_signal);
      destroy_interrupt(&glob_ax);
    }
    FreeSignal(glob_ax.mainproc_signal);
  }

  fclose(mp3_file);
  return RETURN_OK;
}
