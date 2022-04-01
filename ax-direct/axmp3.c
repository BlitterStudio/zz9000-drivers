/*
 * MNT ZZ9000AX Amiga MP3 Player Example (Hardware Accelerated)
 *
 * Copyright (C) 2022, Lukas F. Hartmann <lukas@mntre.com>
 *                     MNT Research GmbH, Berlin
 *                     https://mntre.com
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

#include <clib/alib_protos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/expansion.h>

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <limits.h>

static const char version[] = "$VER: axmp3 1.0\n\r";
static const char procname[] = "axmp3";

#define ZZ_BYTES_PER_PERIOD 3840
#define AUDIO_BUFSZ ZZ_BYTES_PER_PERIOD*8 // TODO: query from hardware
#define WORKER_PRIORITY 127 // 19 would be nicer
#define INTB_EXTER (13)     // External interrupt
#define INTB_VERTB (5)

#define REG_ZZ_CONFIG        0x04
#define REG_ZZ_AUDIO_SWAB    0x70
#define REG_ZZ_AUDIO_SCALE   0x74
#define REG_ZZ_AUDIO_PARAM   0x76
#define REG_ZZ_AUDIO_VAL     0x78
#define REG_ZZ_DECODER_PARAM 0x7A
#define REG_ZZ_DECODER_VAL   0x7C
#define REG_ZZ_DECODE		     0x7E
#define REG_ZZ_AUDIO_CONFIG  0xF4

struct z9ax {
	struct Task *t_mainproc;
	struct Process *worker_process;
  struct Interrupt irq;
  uint32_t hw_addr;
  uint32_t audio_hw_buf_addr;
	int8_t mainproc_signal;
	int8_t enable_signal;
	int8_t worker_signal;
  uint8_t zorro_version;

  uint32_t mp3_addr;
  size_t mp3_bytes;
  uint32_t encoded_offset;
  uint32_t decode_offset;
  uint32_t decode_chunk_sz;
};

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

    *((volatile uint16_t*)(ax->hw_addr+REG_ZZ_AUDIO_SCALE)) = buf_samples;

    *((volatile uint16_t*)(ax->hw_addr+REG_ZZ_DECODER_PARAM)) = 4;
    *((volatile uint16_t*)(ax->hw_addr+REG_ZZ_DECODER_VAL)) = (ax->decode_offset+buf_offset)>>16;
    *((volatile uint16_t*)(ax->hw_addr+REG_ZZ_DECODER_PARAM)) = 5;
    *((volatile uint16_t*)(ax->hw_addr+REG_ZZ_DECODER_VAL)) = (ax->decode_offset+buf_offset)&0xffff;
    *((volatile uint16_t*)(ax->hw_addr+REG_ZZ_DECODER_PARAM)) = 0;
    // ZZ_DECODE (task)
    *((volatile uint16_t*)(ax->hw_addr+REG_ZZ_DECODE)) = 1;

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
  AddIntServer(INTB_EXTER, &ax->irq);
  Permit();

  // enable HW interrupt
  USHORT hw_config = *(USHORT*)(ax->hw_addr + REG_ZZ_AUDIO_CONFIG);
  hw_config |= 1;
  *(volatile USHORT*)(ax->hw_addr + REG_ZZ_AUDIO_CONFIG) = hw_config;
}

void destroy_interrupt(struct z9ax* ax) {
  // disable HW interrupt
  *(volatile USHORT*)(ax->hw_addr + REG_ZZ_AUDIO_CONFIG) = 0;

  Forbid();
  RemIntServer(INTB_EXTER, &ax->irq);
  Permit();
}

struct z9ax glob_ax;

void clean_up() {
  fprintf(stderr, "Cleaning up.\n");
  Signal((struct Task *)glob_ax.worker_process, SIGBREAKF_CTRL_C);
  Wait(1L << glob_ax.mainproc_signal);
  destroy_interrupt(&glob_ax);
  FreeSignal(glob_ax.mainproc_signal);
}

int main(int argc, char* argv[]) {
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
  glob_ax.encoded_offset = 0x100000;
  glob_ax.decode_offset = 0x03f00000;
  glob_ax.decode_chunk_sz = 1920; // 16 bit sample pairs

  if (argc != 2) {
    fprintf(stderr, "Usage: %s soundfile.mp3\n", argv[0]);
    return RETURN_ERROR;
  }

  mp3_file = fopen(argv[1], "rb");
  if (!mp3_file) {
    fprintf(stderr, "Error opening input file.\n");
    return RETURN_ERROR;
  }

  fseek(mp3_file, 0L, SEEK_END);
  size_t sz = ftell(mp3_file);
  rewind(mp3_file);

  fprintf(stderr, "File size: %u bytes.\n", sz);

  glob_ax.mp3_addr = hw_addr + 0x10000 + glob_ax.encoded_offset;
  glob_ax.mp3_bytes = sz;

  fprintf(stderr, "Loading the first 64k...\n");

  size_t mp3_bytes_read = fread((void*)glob_ax.mp3_addr, 1, 64*1024, mp3_file);
  size_t mp3_bytes_total = mp3_bytes_read;

  fprintf(stderr, "Playing...\n");

  glob_ax.audio_hw_buf_addr = hw_addr + 0x10000 + 0x03f00000;

  // set tx buffer address to 127 MB offset
  *((volatile uint16_t*)(hw_addr+REG_ZZ_AUDIO_PARAM)) = 0;
  *((volatile uint16_t*)(hw_addr+REG_ZZ_AUDIO_VAL)) = glob_ax.decode_offset>>16;
  *((volatile uint16_t*)(hw_addr+REG_ZZ_AUDIO_PARAM)) = 1;
  *((volatile uint16_t*)(hw_addr+REG_ZZ_AUDIO_VAL)) = glob_ax.decode_offset&0xffff;

  // set decoder params
  *((volatile uint16_t*)(hw_addr+REG_ZZ_DECODER_PARAM)) = 0;
  *((volatile uint16_t*)(hw_addr+REG_ZZ_DECODER_VAL)) = glob_ax.encoded_offset>>16;
  *((volatile uint16_t*)(hw_addr+REG_ZZ_DECODER_PARAM)) = 1;
  *((volatile uint16_t*)(hw_addr+REG_ZZ_DECODER_VAL)) = glob_ax.encoded_offset&0xffff;
  *((volatile uint16_t*)(hw_addr+REG_ZZ_DECODER_PARAM)) = 2;
  *((volatile uint16_t*)(hw_addr+REG_ZZ_DECODER_VAL)) = glob_ax.mp3_bytes>>16;
  *((volatile uint16_t*)(hw_addr+REG_ZZ_DECODER_PARAM)) = 3;
  *((volatile uint16_t*)(hw_addr+REG_ZZ_DECODER_VAL)) = glob_ax.mp3_bytes&0xffff;
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
  *((volatile uint16_t*)(hw_addr+REG_ZZ_DECODE)) = 0;

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

      atexit(clean_up);

      // stream the rest of the file

      while (mp3_bytes_read > 0) {
        fprintf(stderr, "Streaming... [%d/%d]\n", mp3_bytes_total, glob_ax.mp3_bytes);
        mp3_bytes_read = fread((void*)glob_ax.mp3_addr+mp3_bytes_total, 1, 128*1024, mp3_file);
        mp3_bytes_total += mp3_bytes_read;
      }
      fprintf(stderr, "Done, waiting for Ctrl+C.\n");
      Wait(SIGBREAKF_CTRL_C);
    }
  }

  fclose(mp3_file);
  return RETURN_OK;
}
