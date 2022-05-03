/*
 * MNT ZZ9000AX Amiga Audio Test Driver
 *
 * Copyright (C) 2021, Lukas F. Hartmann <lukas@mntre.com>
 *                     MNT Research GmbH, Berlin
 *                     https://mntre.com
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * GNU General Public License v3.0 or later
 *
 * https://spdx.org/licenses/GPL-3.0-or-later.html
 */

#include <proto/exec.h>
#include <proto/expansion.h>
#include <proto/dos.h>
#include <proto/input.h>
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/libraries.h>
#include <exec/execbase.h>
#include <exec/resident.h>
#include <exec/initializers.h>
#include <devices/inputevent.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>

#define ZZ9K_REGS 0x40000000
#define ZZ9K_AUDIO_TX_BUF 0x40010000
#define AUDIO_BUFSZ 48000*4

#define INTB_EXTER	(13)  /* External interrupt */

static struct Interrupt* db_interrupt;
static struct MsgPort* msgport;
static struct Message msg;

// ZZ9000 Interrupt Handler (INT6)
void dev_isr(/*__reg("a1") void* data*/) {
  USHORT status = *(USHORT*)(ZZ9K_REGS+0x04);

  // audio interrupt signal set?
  if (status & 2) {
    // ack/clear audio interrupt
    *(USHORT*)(ZZ9K_REGS+0x04) = 8|32;
    PutMsg(msgport, &msg);
  }
}

void init_interrupt() {
  if ((db_interrupt = AllocMem(sizeof(struct Interrupt), MEMF_PUBLIC|MEMF_CLEAR))) {
    db_interrupt->is_Node.ln_Type = NT_INTERRUPT;
    db_interrupt->is_Node.ln_Pri = -60;
    db_interrupt->is_Node.ln_Name = "ZZ9000AX";
    db_interrupt->is_Data = (APTR)0;
    db_interrupt->is_Code = dev_isr;

    Disable();
    AddIntServer(INTB_EXTER, db_interrupt);
    Enable();

    printf("ZZ9000AX: Interrupt server registered\n");

    // enable HW interrupt
    USHORT hw_config = *(USHORT*)(ZZ9K_REGS+0xf4);
    hw_config |= 1;
    *(volatile USHORT*)(ZZ9K_REGS+0xf4) = hw_config;

    printf("ZZ9000Net: ZZ interrupt enabled\n");
  } else {
    printf("ZZ9000Net: failed to alloc struct Interrupt\n");
  }
}

void destroy_interrupt() {
  // disable HW interrupt
  *(volatile USHORT*)(ZZ9K_REGS+0xf4) = 0;

  printf("ZZ interrupt disabled\n");

  Forbid();
  RemIntServer(INTB_EXTER, db_interrupt);
  db_interrupt = 0;
  Permit();
}

int main(int argc, char** argv) {
  void* audio_buf = (void*)ZZ9K_AUDIO_TX_BUF;
  char** args = argv; // gcc 6 bug workaround, https://eab.abime.net/showpost.php?p=1346138&postcount=1152

  printf("Hello ZZ9000AX!\n");

  msgport = CreateMsgPort();

  printf("msgport: %p\n", msgport);

  if (msgport) {
    msgport->mp_Node.ln_Name = "ZZ9000AX";
    msgport->mp_Node.ln_Pri = 0;

    AddPort(msgport);
  }

  msg.mn_Node.ln_Type = NT_MESSAGE;
  msg.mn_Length = sizeof(struct Message);
  msg.mn_ReplyPort = msgport;

  if (argc>1) {
    FILE* f;
    printf("argv0: %s\n", args[0]);
    printf("file: %s\n", args[1]);

    if ((f=fopen(args[1], "r"))) {
      printf("file opened.\n");
      init_interrupt();

      while (1) {
        //printf("stream...\n");

        // stream in the next buffer of audio
        int count = fread(audio_buf, AUDIO_BUFSZ, 1, f);
        if (count < 1) {
          break;
        }

        WaitPort(msgport);
        GetMsg(msgport);
      }

      printf("playback completed.\n");

      fclose(f);

      destroy_interrupt();

      memset(audio_buf, 0, AUDIO_BUFSZ);
    } else {
      printf("error: could not open file.\n");
    }
  }

  if (msgport) {
    RemPort(msgport);
    DeleteMsgPort(msgport);
  }

  return 0;
}
