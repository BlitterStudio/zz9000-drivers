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

struct ExecBase     *SysBase;
struct UtilityBase  *UtilityBase;
struct Library      *AHIsubBase  = NULL;
struct DosLibrary   *DOSBase     = NULL;
struct GfxBase *GraphicsBase = NULL;

inline void WRITESHORT(uint16_t b) {
  *((volatile uint16_t*)(0x400000f2)) = b;
  *((volatile uint16_t*)(0x400000f0)) = 0xa;
}

inline void WRITELONG(uint32_t b) {
  *((volatile uint16_t*)(0x400000f2)) = b>>16;
  *((volatile uint16_t*)(0x400000f2)) = b;
  *((volatile uint16_t*)(0x400000f0)) = 0xa;
}

int __attribute__((no_reorder)) _start()
{
  WRITESHORT(111);
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


const char device_name[] = DEVICE_NAME;
const char device_id_string[] = DEVICE_ID_STRING;

struct z9ax *z9ax_base = NULL;

const uint16_t freqs[] = {
  48000,
};

#define debugmsg(v) WRITESHORT(v)

static uint32_t __attribute__((used)) init (BPTR seg_list asm("a0"), struct Library *dev asm("d0"))
{
  SysBase = *(struct ExecBase **)4L;

  if (z9ax_base != NULL) {
    debugmsg(0x101);
    return 0;
  }

  char prefs[10] = "0";
  if (prefs[0] == '0') {} // TODO: prefs?

  if(!(DOSBase = (struct DosLibrary *)OpenLibrary((STRPTR)"dos.library",0)))
    return 0;

  if(!(UtilityBase = (struct UtilityBase *)OpenLibrary((STRPTR)"utility.library",0)))
    return 0;

  if (!z9ax_base) {
    z9ax_base = AllocVec(sizeof(struct z9ax), MEMF_PUBLIC | MEMF_CLEAR);
    z9ax_base->ahi_base = dev;
    AHIsubBase = dev;
  }

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
  debugmsg(4);
  if (z9ax_base == NULL || io == NULL)
    return;

  if (!(io->io_Flags & IOF_QUICK)) {
    ReplyMsg(&io->io_Message);
  }
}

static uint32_t __attribute__((used)) abort_io(struct Library *dev asm("a6"), struct IORequest *io asm("a1"))
{
  debugmsg(5);
  if (!io) return IOERR_NOCMD;
  io->io_Error = IOERR_ABORTED;

  return IOERR_ABORTED;
}

static uint32_t __attribute__((used)) SoundFunc(struct Hook *hook asm("a0"), struct AHIAudioCtrlDrv *actrl asm("a2"), struct AHISoundMessage *chan asm("a1"))
{
  return 0;
}

static void __attribute__((used)) PlayFunc() {
  struct z9ax *ahi_data = z9ax_base;
  debugmsg(0x3021);
}

static void __attribute__((used)) MixFunc() {
  struct z9ax *ahi_data = z9ax_base;
  debugmsg(0x3022);
}

#define ZZ9K_REGS 0x40000000
#define AUDIO_BUF ((void*)0x40010000)
#define AUDIO_BUFSZ 3840*8
#define INTB_EXTER (13)  /* External interrupt */
#define INTB_VERTB (5)

static void* glob_mixbuf = NULL;

static struct Interrupt* db_interrupt;

static uint32_t suspend_playback = 1;
static struct Process* glob_worker_proc = NULL;
static int8_t glob_enable_signal = -1;

static void WorkerProcess()
{
  glob_worker_proc = (struct Process *) FindTask(NULL);
  struct z9ax *ahi_data = z9ax_base;
  struct AHIAudioCtrlDrv *AudioCtrl = ahi_data->audioctrl;

  //struct DosLibrary *DOSBase = (struct DosLibrary *)OpenLibrary((STRPTR)"dos.library",37);
  //if (me) {} // TODO: Do something with me.

  //ahi_data->flags &= ~(1 | 2);

  ahi_data->worker_signal  = AllocSignal(-1);
  ahi_data->play_signal   = AllocSignal(-1);
  ahi_data->mix_signal    = AllocSignal(-1);
  ahi_data->enable_signal    = AllocSignal(-1);
  ahi_data->disable_signal   = AllocSignal(-1);

  uint32_t signals = 0;
  uint32_t buf_offset = 0;

  // Tell Master we're alive
  Signal(ahi_data->t_master, 1L << ahi_data->master_signal);

  glob_enable_signal = ahi_data->enable_signal;

  //AudioCtrl->ahiac_SoundFunc->h_Entry = (void *)SoundFunc;

  debugmsg(0x0fff);

  for(;;) {
    //debugmsg(0x1000);
    signals = Wait(SIGBREAKF_CTRL_C | (1L<<ahi_data->enable_signal));

    if (signals & SIGBREAKF_CTRL_C) {
      debugmsg(0x1001);
      break;
    }

    for (int i=0; i<1; i++) {
      uint32_t res = CallHookPkt(AudioCtrl->ahiac_PlayerFunc, AudioCtrl, NULL);
      res = CallHookPkt(AudioCtrl->ahiac_MixerFunc, AudioCtrl, AUDIO_BUF+buf_offset);

      // byteswap buffer
      *((volatile uint16_t*)(0x40000070)) = buf_offset>>8; // (/256)
      uint32_t bytes = 2*AudioCtrl->ahiac_BuffSamples*(AudioCtrl->ahiac_Flags & AHIACF_STEREO ? 2 : 1);

      /*switch (AudioCtrl->ahiac_BuffType) {
        case AHIST_M16S:
        debugmsg(0xeee0);
        break;
        case AHIST_M32S:
        debugmsg(0xeee1);
        break;
        case AHIST_S16S:
        debugmsg(0xeee2);
        break;
        case AHIST_S32S:
        debugmsg(0xeee3);
        break;
        }*/

      buf_offset+=bytes;

      // going too fast for AHI
      if (suspend_playback) {
        debugmsg(0x1999);
        //break;
      }

      if (buf_offset>=AUDIO_BUFSZ) {
        debugmsg(0xcafe);
        buf_offset = 0;
      }
    }
  }

  //Forbid();
  FreeSignal(ahi_data->enable_signal);    ahi_data->enable_signal   = -1;
  FreeSignal(ahi_data->disable_signal);      ahi_data->disable_signal     = -1;
  FreeSignal(ahi_data->worker_signal);    ahi_data->worker_signal   = -1;
  FreeSignal(ahi_data->play_signal);     ahi_data->play_signal    = -1;
  FreeSignal(ahi_data->mix_signal);      ahi_data->mix_signal     = -1;

  ahi_data->worker_process = NULL;
  //Signal((struct Task *)ahi_data->t_master, 1L << ahi_data->master_signal);

  // Multitaking will resume when we are dead.
  debugmsg(0x999f);
}

void dev_isr(/*__reg("a1") void* data*/) {
  USHORT status = *(USHORT*)(ZZ9K_REGS+0x04);

  //debugmsg(0x9999);

  // audio interrupt signal set?
  //if (status & 2) {
  // ack/clear audio interrupt
  //*(USHORT*)(ZZ9K_REGS+0x04) = 8|32;
  //debugmsg(0x999a);

  if (glob_worker_proc && !suspend_playback) {
    debugmsg(0x9999);
    Signal((struct Task*)glob_worker_proc, 1L<<glob_enable_signal);
  }
}

void init_interrupt() {
  if ((db_interrupt = AllocMem(sizeof(struct Interrupt), MEMF_PUBLIC|MEMF_CLEAR))) {
    debugmsg(0xe000);

    db_interrupt->is_Node.ln_Type = NT_INTERRUPT;
    db_interrupt->is_Node.ln_Pri = -60;
    db_interrupt->is_Node.ln_Name = "ZZ9000AX";
    db_interrupt->is_Data = (APTR)0;
    db_interrupt->is_Code = dev_isr;

    Disable();
    AddIntServer(INTB_EXTER, db_interrupt);
    Enable();

    // enable HW interrupt
    USHORT hw_config = *(USHORT*)(ZZ9K_REGS+0xf4);
    hw_config |= 1;
    *(volatile USHORT*)(ZZ9K_REGS+0xf4) = hw_config;
  }
}

void destroy_interrupt() {
  // disable HW interrupt
  *(volatile USHORT*)(ZZ9K_REGS+0xf4) = 0;

  Forbid();
  RemIntServer(INTB_EXTER, db_interrupt);
  db_interrupt = 0;
  Permit();
}

static uint32_t __attribute__((used)) intAHIsub_AllocAudio(struct TagItem *tagList asm("a1"), struct AHIAudioCtrlDrv *AudioCtrl asm("a2"))
{
  debugmsg(0x600);
  SysBase = *(struct ExecBase **)4L;

  if (!DOSBase) {
    DOSBase = (struct DosLibrary *)OpenLibrary((STRPTR)"dos.library",37);
  }
  if(!UtilityBase) {
    UtilityBase = (struct UtilityBase *)OpenLibrary((STRPTR)"utility.library",37);
  }

  if((AudioCtrl->ahiac_DriverData = AllocVec(sizeof(struct z9ax), MEMF_PUBLIC | MEMF_ANY | MEMF_CLEAR)))
    {
      struct z9ax *ahi_data = (struct z9ax*)AudioCtrl->ahiac_DriverData;
      ahi_data->audioctrl = AudioCtrl;
      if (z9ax_base != ahi_data) {
        z9ax_base = ahi_data;
      }
      ahi_data->ahi_base    = AHIsubBase;
      ahi_data->worker_signal   = -1;
      ahi_data->play_signal    = -1;
      ahi_data->enable_signal    = -1;
      ahi_data->mix_signal     = -1;

      ahi_data->t_master = FindTask(NULL);
      ahi_data->master_signal = AllocSignal(-1);
      if (ahi_data->master_signal != -1) {
        Forbid();
        if (ahi_data->worker_process = CreateNewProcTags(NP_Entry, (uint32_t)&WorkerProcess, NP_Name, (uint32_t)device_name, NP_Priority, 10, TAG_DONE)) {
          debugmsg(606);
          ahi_data->worker_process->pr_Task.tc_UserData = AudioCtrl;
        }
        Permit();
        debugmsg(0x607);

        if(ahi_data->worker_process) {
          debugmsg(608);
          Wait(1L << ahi_data->master_signal);   // Wait for worker to come alive
          if(ahi_data->worker_process != NULL) {
            debugmsg(0x609);
            ahi_data->flags |= 4;
          }
        }

        init_interrupt();
      }
    }

  struct z9ax *ahi_data = (struct z9ax*)AudioCtrl->ahiac_DriverData;
  ahi_data->ahi_base = AHIsubBase;
  ahi_data->mix_freq = AudioCtrl->ahiac_MixFreq;

  return AHISF_KNOWSTEREO | AHISF_MIXING | AHISF_TIMING;
}

static void __attribute__((used)) intAHIsub_FreeAudio(struct AHIAudioCtrlDrv *AudioCtrl asm("a2")) {
  debugmsg(0x6000);
  if (AudioCtrl->ahiac_DriverData) {
    struct z9ax *ahi_data = (struct z9ax*)AudioCtrl->ahiac_DriverData;

    if (ahi_data->worker_process) {
      Signal((struct Task *)ahi_data->worker_process, SIGBREAKF_CTRL_C);
      debugmsg(0x999e);
      //Wait(1L << ahi_data->master_signal);
      ahi_data->worker_process = NULL;
    }

    FreeSignal(ahi_data->master_signal);
    FreeVec(AudioCtrl->ahiac_DriverData);
    AudioCtrl->ahiac_DriverData = NULL;

    destroy_interrupt();
  }
}

static void __attribute__((used)) intAHIsub_Stop(uint32_t Flags asm("d0"), struct AHIAudioCtrlDrv *AudioCtrl asm("a2")) {
  debugmsg(0x7000);
  if (Flags & AHISF_PLAY) {
    struct z9ax *ahi_data = (struct z9ax*)AudioCtrl->ahiac_DriverData;

    FreeVec(glob_mixbuf);
    glob_mixbuf = NULL;

    debugmsg(0x7093);
  }
}

static uint32_t __attribute__((used)) intAHIsub_Start(uint32_t flags asm("d0"), struct AHIAudioCtrlDrv *AudioCtrl asm("a2")) {
  debugmsg(0x4000);
  intAHIsub_Stop(flags, AudioCtrl);

  if (flags & AHISF_PLAY) {
    struct z9ax *ahi_data = (struct z9ax*)AudioCtrl->ahiac_DriverData;
    debugmsg(0x4001);

    glob_mixbuf = AllocVec(3840*2, MEMF_PUBLIC | MEMF_ANY | MEMF_CLEAR);

    // enable playback
    debugmsg(0x4004);
    suspend_playback = 0;
    debugmsg(0x4005);
  }

  return AHIE_OK;
}

static uint32_t __attribute__((used)) intAHIsub_GetAttr(uint32_t attr_ asm("d0"), int32_t arg_ asm("d1"), int32_t def_ asm("d2"), struct TagItem *tagList asm("a1"), struct AHIAudioCtrlDrv *AudioCtrl asm("a2")) {
  uint32_t attr = attr_;
  int32_t arg = arg_, def = def_;

  switch(attr)
    {
    case AHIDB_Bits:
      debugmsg(1002);
      return 16;
    case AHIDB_Frequencies:
      debugmsg(1003);
      return 1;
    case AHIDB_Frequency:
      debugmsg(1004);
      return freqs[arg];
    case AHIDB_Index:
      debugmsg(1005);
      for (int i = 0; i < 6; i++) {
        if (freqs[i] == arg)
          return i;
      }
      return -1;
    case AHIDB_Author:
      debugmsg(1006);
      return (int32_t) "ZZ9000AX";
    case AHIDB_Copyright:
      debugmsg(1007);
      return (int32_t) "MNT Research GmbH";
    case AHIDB_Version:
      debugmsg(1008);
      return (int32_t) device_id_string;
    case AHIDB_Annotation:
      debugmsg(1009);
      return (int32_t) "https://mntre.com/zz9000";
    case AHIDB_Record:
      debugmsg(1010);
      return FALSE;
    case AHIDB_FullDuplex:
      debugmsg(1011);
      return TRUE;
    case AHIDB_Realtime:
      debugmsg(1012);
      return TRUE;
    case AHIDB_MaxChannels:
      debugmsg(1013);
      return 2;
    case AHIDB_MaxPlaySamples:
      debugmsg(1014);
      return 3840;
    case AHIDB_MaxRecordSamples:
      debugmsg(1015);
      return 0;
    case AHIDB_MinMonitorVolume:
      debugmsg(1016);
      return 0x0;
    case AHIDB_MaxMonitorVolume:
      debugmsg(1017);
      return 0x10000;
    case AHIDB_MinInputGain:
      debugmsg(1018);
      return 0x0;
    case AHIDB_MaxInputGain:
      debugmsg(1019);
      return 0x0;
    case AHIDB_MinOutputVolume:
      debugmsg(1020);
      return 0x0;
    case AHIDB_MaxOutputVolume:
      debugmsg(1021);
      return 0x10000;
    case AHIDB_Inputs:
      debugmsg(1022);
      return 0;
    case AHIDB_Input:
      debugmsg(1023);
      return 0;
    case AHIDB_Outputs:
      debugmsg(1024);
      return 2;
    case AHIDB_Output:
      debugmsg(1025);
      switch (arg) {
      case 0: return (int32_t) "OUT 1";
      case 1: return (int32_t) "OUT 2";
      }
    default:
      debugmsg(1099);
      return def;
    }
}

static int32_t __attribute__((used)) intAHIsub_HardwareControl(uint32_t attr asm("d0"), uint32_t arg asm("d1"), struct AHIAudioCtrlDrv *AudioCtrl asm("a2"))
{
  return 0;

  debugmsg(11);
  int32_t rc = TRUE;
  struct z9ax *ahi_data = (struct z9ax*)AudioCtrl->ahiac_DriverData;

  switch (attr) {
  case AHIC_MonitorVolume:
    debugmsg(1101);
    ahi_data->monitor_volume = arg;
    break;
  case AHIC_MonitorVolume_Query:
    debugmsg(1102);
    rc = ahi_data->monitor_volume;
    break;
  case AHIC_InputGain:
    debugmsg(1103);
    ahi_data->input_gain = arg;
    break;
  case AHIC_InputGain_Query:
    debugmsg(1104);
    rc = ahi_data->input_gain;
    break;
  case AHIC_OutputVolume:
    debugmsg(1105);
    ahi_data->output_volume = arg;
    break;
  case AHIC_OutputVolume_Query:
    debugmsg(1106);
    rc = ahi_data->output_volume;
    break;
  case AHIC_Input:
    debugmsg(1107);
    break;
  case AHIC_Input_Query:
    debugmsg(1108);
    break;
  case AHIC_Output:
    debugmsg(1109);
    rc = TRUE;
    break;
  case AHIC_Output_Query:
    debugmsg(1110);
    rc = 0;
    break;
  default:
    debugmsg(1199);
    rc = FALSE;
    break;
  }

  return rc;
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

static uint32_t __attribute__((used)) intAHIsub_Enable(struct AHIAudioCtrlDrv *AudioCtrl asm("a2"))
{
  //debugmsg(0x8000);
  struct z9ax *ahi_data = (struct z9ax*)AudioCtrl->ahiac_DriverData;

  if (ahi_data->disable_cnt > 0) {
    ahi_data->disable_cnt--;
  }

  if (!ahi_data->disable_cnt) {
    suspend_playback = 0;
  }

  return AHIE_OK;
}

static uint32_t __attribute__((used)) intAHIsub_Disable(struct AHIAudioCtrlDrv *AudioCtrl asm("a2"))
{
  //debugmsg(0x9000);
  struct z9ax *ahi_data = (struct z9ax*)AudioCtrl->ahiac_DriverData;

  suspend_playback = 1;
  ahi_data->disable_cnt++;

  return AHIE_OK;
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
