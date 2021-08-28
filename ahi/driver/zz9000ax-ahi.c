/*
 * MNT ZZ9000AX Amiga Audio Test Driver
 *
 * Copyright (C) 2021, Lukas F. Hartmann <lukas@mntre.com>
 *                     MNT Research GmbH, Berlin
 *                     https://mntre.com
 *
 * Based on code by _Bnu (thanks a ton!) and AHI example drivers.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * GNU General Public License v3.0 or later
 *
 * https://spdx.org/licenses/GPL-3.0-or-later.html
 */

#include <exec/exec.h>
#include <exec/memory.h>

#include <graphics/gfx.h>
#include <graphics/gfxbase.h>
#include <graphics/view.h>
#include <proto/graphics.h>
#include <clib/graphics_protos.h>

#include <dos/dos.h>
#include <dos/dostags.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/utility.h>
#include <proto/expansion.h>

//#include <proto/ahi.h>
#include <proto/ahi_sub.h>
#include <clib/ahi_sub_protos.h>
#include <clib/debug_protos.h>

#include <math.h>
#include <string.h>
#include <stdint.h>

#include "zz9000ax-ahi.h"

#define STR(s) #s
#define XSTR(s) STR(s)

#define DEVICE_NAME "zz9000ax.audio"
#define DEVICE_DATE "(22 Aug 2021)"
#define DEVICE_ID_STRING "ZZ9000AX " XSTR(DEVICE_VERSION) "." XSTR(DEVICE_REVISION) " " DEVICE_DATE
#define DEVICE_VERSION 4
#define DEVICE_REVISION 14
#define DEVICE_PRIORITY 0

#define REAL_HARDWARE 1

struct ExecBase     *SysBase;
struct UtilityBase  *UtilityBase;
struct Library      *AHIsubBase  = NULL;
struct DosLibrary   *DOSBase     = NULL;
struct GfxBase *GraphicsBase = NULL;

int __attribute__((no_reorder)) _start()
{
  return -1;
}

asm("romtag:                                \n"
    "       dc.w    "XSTR(RTC_MATCHWORD)"   \n"
    "       dc.l    romtag                  \n"
    "       dc.l    endcode                 \n"
    "       dc.b    "XSTR(RTF_AUTOINIT)"    \n"
    "       dc.b    "XSTR(DEVICE_VERSION)"  \n"
    "       dc.b    "XSTR(NT_LIBRARY)"      \n"
    "       dc.b    "XSTR(DEVICE_PRIORITY)" \n"
    "       dc.l    _device_name            \n"
    "       dc.l    _device_id_string       \n"
    "       dc.l    _auto_init_tables       \n"
    "endcode:                               \n");


inline void WRITESHORT(uint32_t b) {
  //kprintf((uint8_t*)"%lx\n",b);

  *((volatile uint16_t*)(0x400000f2)) = b;
  *((volatile uint16_t*)(0x400000f0)) = 0xa;
}

inline void WRITELONG(uint32_t b) {
  //kprintf((uint8_t*)"%lx\n",b);

  //*((volatile uint16_t*)(0x400000f2)) = b>>16;
  //*((volatile uint16_t*)(0x400000f2)) = b;
  //*((volatile uint16_t*)(0x400000f0)) = 0xa;
}

const char device_name[] = DEVICE_NAME;
const char device_id_string[] = DEVICE_ID_STRING;

const uint16_t freqs[] = {
  8000,
  12000,
  24000,
  48000,
};

// REMEMBER: never use global variables (except const!)
// they are not initialized properly and will corrupt AHI code/data

#define debugmsg(v) WRITESHORT(v)

static uint32_t __attribute__((used)) init (BPTR seg_list asm("a0"), struct Library *dev asm("d0"))
{
  SysBase = *(struct ExecBase **)4L;

  if(!(DOSBase = (struct DosLibrary *)OpenLibrary((STRPTR)"dos.library",0)))
    return 0;

  if(!(UtilityBase = (struct UtilityBase *)OpenLibrary((STRPTR)"utility.library",0)))
    return 0;

  debugmsg(0x5fff);

  return (uint32_t)dev;
}

static uint8_t* __attribute__((used)) expunge(struct Library *libbase asm("a6"))
{
  if(DOSBase)       { CloseLibrary((struct Library *)DOSBase); DOSBase = NULL; }
  if(UtilityBase)   { CloseLibrary((struct Library *)UtilityBase); UtilityBase = NULL; }

  return 0;
}

static uint8_t __attribute__((used)) null()
{
  return 0;
}

static void __attribute__((used)) open(struct Library *dev asm("a6"), struct IORequest *iotd asm("a1"), uint32_t num asm("d0"), uint32_t flags asm("d1"))
{
  if (!AHIsubBase) {
    AHIsubBase = dev;
  }

  iotd->io_Error = 0;
  dev->lib_OpenCnt++;
}

static uint8_t* __attribute__((used)) close(struct Library *dev asm("a6"), struct IORequest *iotd asm("a1"))
{
  return 0;
}

static void __attribute__((used)) begin_io(struct Library *dev asm("a6"), struct IORequest *io asm("a1"))
{
  if (io == NULL)
    return;

  if (!(io->io_Flags & IOF_QUICK)) {
    ReplyMsg(&io->io_Message);
  }
}

static uint32_t __attribute__((used)) abort_io(struct Library *dev asm("a6"), struct IORequest *io asm("a1"))
{
  if (!io) return IOERR_NOCMD;
  io->io_Error = IOERR_ABORTED;

  return IOERR_ABORTED;
}

static uint32_t __attribute__((used)) SoundFunc(struct Hook *hook asm("a0"), struct AHIAudioCtrlDrv *actrl asm("a2"), struct AHISoundMessage *chan asm("a1"))
{
  return 0;
}

#define AUDIO_BUFSZ 3840*8
#define INTB_EXTER (13)  /* External interrupt */
#define INTB_VERTB (5)

void WorkerProcess()
{
  struct Process* proc = (struct Process *) FindTask(NULL);
  struct z9ax* ahi_data = proc->pr_Task.tc_UserData;
  struct AHIAudioCtrlDrv* AudioCtrl = ahi_data->audioctrl;

#ifndef REAL_HARDWARE
  uint8_t* glob_buf = AllocVec(3840*2, MEMF_ANY);
#endif

  ahi_data->worker_signal = AllocSignal(-1);
  ahi_data->enable_signal = AllocSignal(-1);

  uint32_t signals = 0;
  uint32_t buf_offset = 0;
  uint32_t pfreq = 0;

  Signal(ahi_data->t_mainproc, 1L << ahi_data->mainproc_signal);

  for(;;) {
    signals = Wait(SIGBREAKF_CTRL_C | (1L<<ahi_data->enable_signal));

    if (signals & SIGBREAKF_CTRL_C) {
      break;
    }

    for (int i=0; i<1; i++) {
      CallHookPkt(AudioCtrl->ahiac_PlayerFunc, AudioCtrl, NULL);

      //debugmsg(0x1100 | AudioCtrl->ahiac_Flags);
      //debugmsg(0x1200 | AudioCtrl->ahiac_Channels);

      if (AudioCtrl->ahiac_PreTimer && AudioCtrl->ahiac_MixerFunc) {
        if (!(*AudioCtrl->ahiac_PreTimer)()) {
#ifdef REAL_HARDWARE
          CallHookPkt(AudioCtrl->ahiac_MixerFunc, AudioCtrl, ahi_data->audio_buf_addr+buf_offset);
#else
          CallHookPkt(AudioCtrl->ahiac_MixerFunc, AudioCtrl, glob_buf);
          uint32_t* xbuf = (uint32_t*)glob_buf;
          kprintf((uint8_t*)"%lx %lx %lx %lx\n", xbuf[0], xbuf[1], xbuf[2], xbuf[3]);
#endif
        }
        (*AudioCtrl->ahiac_PostTimer)();
      }

      uint32_t bytes = 2*AudioCtrl->ahiac_BuffSamples*(AudioCtrl->ahiac_Flags & AHIACF_STEREO ? 2 : 1);

      if (AudioCtrl->ahiac_PlayerFreq != pfreq) {
        debugmsg(0x1000);
        debugmsg(AudioCtrl->ahiac_PlayerFreq>>16);
        debugmsg(AudioCtrl->ahiac_PlayerFreq);
        debugmsg(bytes);
        pfreq = AudioCtrl->ahiac_PlayerFreq;
      }

      uint32_t scale = 48000/AudioCtrl->ahiac_MixFreq;
      // scale
      *((volatile uint16_t*)(ahi_data->hw_addr+0x74)) = scale;

#ifdef REAL_HARDWARE
      // byteswap buffer
      *((volatile uint16_t*)(ahi_data->hw_addr+0x70)) = buf_offset>>8; // (/256)

      int overrun = *((volatile uint16_t*)(ahi_data->hw_addr+0x70));
      if (overrun == 1) {
        memset((void*)ahi_data->audio_buf_addr, 0, AUDIO_BUFSZ);
        buf_offset = 0;
      }
#endif

      buf_offset+=bytes*scale;

      if (buf_offset>=AUDIO_BUFSZ) {
        //debugmsg(0xcafe);
        buf_offset = 0;
      }
    }
  }

  Forbid();
  FreeSignal(ahi_data->enable_signal);
  ahi_data->enable_signal = -1;
  FreeSignal(ahi_data->worker_signal);
  ahi_data->worker_signal = -1;

#ifndef REAL_HARDWARE
  FreeVec(glob_buf);
#endif

  ahi_data->worker_process = NULL;
  Signal((struct Task *)ahi_data->t_mainproc, 1L << ahi_data->mainproc_signal);

  // Multitaking will resume when we are dead.
  debugmsg(0x999f);
}

uint32_t dev_isr(struct z9ax* data asm("a1")) {
  USHORT status = *(USHORT*)(data->hw_addr+0x04);

  // audio interrupt signal set?
  if (status & 2) {
    // ack/clear audio interrupt
    *(USHORT*)(data->hw_addr+0x04) = 8|32;
    //debugmsg(0x999a);

    if (data->worker_process && !data->disable_cnt) {
      //debugmsg(0x9999);
      Signal((struct Task*)data->worker_process, 1L<<data->enable_signal);
    }
  }

  if (status == 2) {
    return 1;
  } else {
    return 0;
  }
}

void init_interrupt(struct z9ax* ahi_data) {
  debugmsg(0xe000);

  struct Interrupt* irq = &ahi_data->irq;

  irq->is_Node.ln_Type = NT_INTERRUPT;
  irq->is_Node.ln_Pri = -60;
  irq->is_Node.ln_Name = "ZZ9000AX";
  irq->is_Data = ahi_data;
  irq->is_Code = dev_isr;

  Forbid();
#ifdef REAL_HARDWARE
  AddIntServer(INTB_EXTER, irq);
#else
  AddIntServer(INTB_VERTB, irq); // for debugging
#endif
  Permit();

#ifdef REAL_HARDWARE
  // enable HW interrupt
  USHORT hw_config = *(USHORT*)(ahi_data->hw_addr+0xf4);
  hw_config |= 1;
  *(volatile USHORT*)(ahi_data->hw_addr+0xf4) = hw_config;
#endif
}

void destroy_interrupt(struct z9ax* ahi_data) {
  struct Interrupt* irq = &ahi_data->irq;

#ifdef REAL_HARDWARE
  // disable HW interrupt
  *(volatile USHORT*)(ahi_data->hw_addr+0xf4) = 0;
#endif

  Forbid();
  RemIntServer(INTB_EXTER, irq);
  Permit();
}

static uint32_t __attribute__((used)) intAHIsub_AllocAudio(struct TagItem *tagList asm("a1"), struct AHIAudioCtrlDrv *AudioCtrl asm("a2"))
{
  SysBase = *(struct ExecBase**)4L;

  if (!DOSBase) {
    DOSBase = (struct DosLibrary*)OpenLibrary((STRPTR)"dos.library",37);
  }
  if (!UtilityBase) {
    // needed for CallHookPkt
    UtilityBase = (struct UtilityBase*)OpenLibrary((STRPTR)"utility.library",37);
  }

  struct ConfigDev* cd;
  uint32_t hw_addr = 0;

  if ((ExpansionBase = (struct ExpansionBase*) OpenLibrary("expansion.library", 0)) ) {
    // Find Z2 or Z3 model of MNT ZZ9000
    if ((cd = (struct ConfigDev*)FindConfigDev(cd,0x6d6e,0x4)) || (cd = (struct ConfigDev*)FindConfigDev(cd,0x6d6e,0x3))) {
      hw_addr = (uint32_t)cd->cd_BoardAddr;

      // TODO: query for ZZ9000AX
    } else {
      // hardware not found
      CloseLibrary(ExpansionBase);
      CloseLibrary(UtilityBase);
      CloseLibrary(DOSBase);
      return 0; // FIXME correct code?
    }
  } else {
    CloseLibrary(UtilityBase);
    CloseLibrary(DOSBase);
    return 0; // FIXME correct code?
  }

  debugmsg(0x6000);
  struct z9ax *ahi_data = AllocVec(sizeof(struct z9ax), MEMF_PUBLIC | MEMF_ANY | MEMF_CLEAR);

  AudioCtrl->ahiac_DriverData = ahi_data;

  ahi_data->hw_addr = hw_addr;
  ahi_data->audio_buf_addr = hw_addr+0x10000; // FIXME
  ahi_data->audioctrl = AudioCtrl;
  ahi_data->ahi_base = AHIsubBase;
  ahi_data->worker_signal = -1;
  ahi_data->enable_signal = -1;

  ahi_data->t_mainproc = FindTask(NULL);
  ahi_data->mainproc_signal = AllocSignal(-1);

  if (ahi_data->mainproc_signal != -1) {
    Forbid();
    if (ahi_data->worker_process = CreateNewProcTags(NP_Entry, (uint32_t)&WorkerProcess, NP_Name, (uint32_t)device_name, NP_Priority, 127, TAG_DONE)) {
      ahi_data->worker_process->pr_Task.tc_UserData = ahi_data;
    }
    Permit();

    if (ahi_data->worker_process) {
      Wait(1L << ahi_data->mainproc_signal);
      if (ahi_data->worker_process != NULL) {
      }
    }

    init_interrupt(ahi_data);
  }

  uint32_t scale = 48000/AudioCtrl->ahiac_MixFreq;
  AudioCtrl->ahiac_BuffSamples = (3840/4)/scale;

  // none of that weird timing plox
  return AHISF_KNOWSTEREO | AHISF_MIXING; // | AHISF_TIMING;
}

static void __attribute__((used)) intAHIsub_FreeAudio(struct AHIAudioCtrlDrv *AudioCtrl asm("a2")) {
  debugmsg(0x6100);

  if (AudioCtrl->ahiac_DriverData) {
    struct z9ax *ahi_data = AudioCtrl->ahiac_DriverData;

    destroy_interrupt(ahi_data);

    if (ahi_data->worker_process) {
      Signal((struct Task *)ahi_data->worker_process, SIGBREAKF_CTRL_C);
      debugmsg(0x999e);
      Wait(1L << ahi_data->mainproc_signal);
      ahi_data->worker_process = NULL;
    }

    FreeSignal(ahi_data->mainproc_signal);

    FreeVec(AudioCtrl->ahiac_DriverData);
    AudioCtrl->ahiac_DriverData = NULL;
  } else {
    debugmsg(0x6101);
  }
}

static uint32_t __attribute__((used)) intAHIsub_Stop(uint32_t Flags asm("d0"), struct AHIAudioCtrlDrv *AudioCtrl asm("a2")) {
  debugmsg(0x7000);
  if (Flags & AHISF_PLAY) {
    struct z9ax *ahi_data = (struct z9ax*)AudioCtrl->ahiac_DriverData;
    debugmsg(0x7004);
    //suspend_playback = 1;

    memset((void*)ahi_data->audio_buf_addr, 0, AUDIO_BUFSZ);
  }

  return AHIE_OK;
}

static uint32_t __attribute__((used)) intAHIsub_Start(uint32_t flags asm("d0"), struct AHIAudioCtrlDrv *AudioCtrl asm("a2")) {
  debugmsg(0x4000);
  intAHIsub_Stop(flags, AudioCtrl);

  if (flags & AHISF_PLAY) {
    struct z9ax *ahi_data = (struct z9ax*)AudioCtrl->ahiac_DriverData;
    // enable playback
    debugmsg(0x4004);
    //suspend_playback = 0;

    memset((void*)ahi_data->audio_buf_addr, 0, AUDIO_BUFSZ);
  }

  return AHIE_OK;
}

static int32_t __attribute__((used)) intAHIsub_GetAttr(uint32_t attr_ asm("d0"), int32_t arg_ asm("d1"), int32_t def_ asm("d2"), struct TagItem *tagList asm("a1"), struct AHIAudioCtrlDrv *AudioCtrl asm("a2")) {
  uint32_t attr = attr_;
  int32_t arg = arg_, def = def_;

  //debugmsg(0xc000);
  //debugmsg(attr);

  switch(attr)
    {
    case AHIDB_Bits:
      return 16;
    case AHIDB_Frequencies:
      return 4;
    case AHIDB_Frequency:
      return freqs[arg];
    case AHIDB_Index:
      // FIXME!
      for (int i = 0; i < 4; i++) {
        if (freqs[i] >= arg)
          return i;
      }
      return 3;
    case AHIDB_Author:
      return (int32_t) "ZZ9000AX";
    case AHIDB_Copyright:
      return (int32_t) "MNT Research GmbH";
    case AHIDB_Version:
      return (int32_t) device_id_string;
    case AHIDB_Annotation:
      return (int32_t) "https://mntre.com/zz9000";
    case AHIDB_Record:
      return FALSE;
    case AHIDB_FullDuplex:
      return TRUE;
    case AHIDB_Realtime:
      return TRUE;
    case AHIDB_MaxChannels:
      return 4;
    case AHIDB_MaxPlaySamples:
      return 3840;
    case AHIDB_MaxRecordSamples:
      return 0;
    case AHIDB_MinMonitorVolume:
      return 0x0;
    case AHIDB_MaxMonitorVolume:
      return 0x10000;
    case AHIDB_MinInputGain:
      return 0x0;
    case AHIDB_MaxInputGain:
      return 0x0;
    case AHIDB_MinOutputVolume:
      return 0x0;
    case AHIDB_MaxOutputVolume:
      return 0x10000;
    case AHIDB_Inputs:
      return 0;
    case AHIDB_Input:
      return 0;
    case AHIDB_Outputs:
      return 1;
    case AHIDB_Output:
      return (int32_t) "OUT 1";
    default:
      return def;
    }
}

static int32_t __attribute__((used)) intAHIsub_HardwareControl(uint32_t attr asm("d0"), uint32_t arg asm("d1"), struct AHIAudioCtrlDrv *AudioCtrl asm("a2"))
{
  return 0;
}

static uint32_t __attribute__((used)) intAHIsub_SetEffect(uint8_t *effect asm("a0"), struct AHIAudioCtrlDrv *AudioCtrl asm("a2"))
{
  return AHIS_UNKNOWN;
}

static uint32_t __attribute__((used)) intAHIsub_LoadSound(uint16_t sound asm("d0"), uint32_t type asm("d1"), struct AHISampleInfo *info asm("a0"), struct AHIAudioCtrlDrv *AudioCtrl asm("a2"))
{
  return AHIS_UNKNOWN;
}

static uint32_t __attribute__((used)) intAHIsub_UnloadSound(uint16_t sound asm("d0"), struct AHIAudioCtrlDrv *AudioCtrl asm("a2"))
{
  return AHIS_UNKNOWN;
}

static void __attribute__((used)) intAHIsub_Enable(struct AHIAudioCtrlDrv *AudioCtrl asm("a2"))
{
  struct z9ax *ahi_data = AudioCtrl->ahiac_DriverData;
  if (ahi_data->disable_cnt > 0) {
    ahi_data->disable_cnt--;
  }
}

static void __attribute__((used)) intAHIsub_Disable(struct AHIAudioCtrlDrv *AudioCtrl asm("a2"))
{
  struct z9ax *ahi_data = AudioCtrl->ahiac_DriverData;
  ahi_data->disable_cnt++;
  //memset((void*)ahi_data->audio_buf_addr, 0, AUDIO_BUFSZ);
}

static void __attribute__((used)) intAHIsub_Update(uint32_t flags asm("d0"), struct AHIAudioCtrlDrv *AudioCtrlDrv asm("a2"))
{
}

static uint32_t __attribute__((used)) intAHIsub_SetVol(uint16_t channel asm("d0"), uint32_t volume asm("d1"), uint32_t pan asm("d2"), struct AHIAudioCtrlDrv *AudioCtrl asm("a2"), uint32_t flags asm("d3"))
{
  return AHIS_UNKNOWN;
}

static uint32_t __attribute__((used)) intAHIsub_SetFreq(uint16_t channel asm("d0"), uint32_t freq asm("d1"), struct AHIAudioCtrlDrv *AudioCtrl asm("a2"), uint32_t flags asm("d2"))
{
  return AHIS_UNKNOWN;
}

static uint32_t __attribute__((used)) intAHIsub_SetSound(uint16_t channel asm("d0"), uint16_t sound asm("d1"), uint32_t offset asm("d2"), int32_t length asm("d3"), struct AHIAudioCtrlDrv *AudioCtrl asm("a2"), uint32_t flags asm("d4"))
{
  return AHIS_UNKNOWN;
}

static uint32_t function_table[] = {
  (uint32_t)open,
  (uint32_t)close,
  (uint32_t)expunge,
  (uint32_t)null,
  (uint32_t)intAHIsub_AllocAudio,             // AllocAudio
  (uint32_t)intAHIsub_FreeAudio,              // FreeAudio
  (uint32_t)intAHIsub_Disable,                // Disable
  (uint32_t)intAHIsub_Enable,                 // Enable
  (uint32_t)intAHIsub_Start,                  // Start
  (uint32_t)intAHIsub_Update,                 // Update
  (uint32_t)intAHIsub_Stop,                   // Stop
  (uint32_t)intAHIsub_SetVol,                 // SetVol
  (uint32_t)intAHIsub_SetFreq,                // SetFreq
  (uint32_t)intAHIsub_SetSound,               // SetSound
  (uint32_t)intAHIsub_SetEffect,              // SetEffect
  (uint32_t)intAHIsub_LoadSound,              // LoadSound
  (uint32_t)intAHIsub_UnloadSound,            // UnloadSound
  (uint32_t)intAHIsub_GetAttr,                // GetAttr
  (uint32_t)intAHIsub_HardwareControl,        // HardwareControl
  (uint32_t)null,
  (uint32_t)null,
  (uint32_t)null,
  -1
};

const uint32_t auto_init_tables[4] = {
  sizeof(struct z9ax),
  (uint32_t)function_table,
  0,
  (uint32_t)init,
};
