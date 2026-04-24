/*
 * MNT ZZ9000AX Amiga AHI Driver
 *
 * Copyright (C) 2022-2026, MNT Research GmbH, Lucie L. Hartmann <lucie@mntre.com>
 *                          https://mntre.com
 *
 * Based on code by _Bnu (thanks a ton!) and AHI example drivers.
 * Modified by Thomas Wenzel (TW)
 * Hardened by Dimitris Panokostas <midwan@gmail.com> (2026)
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
#include <exec/interrupts.h>
#include <hardware/intbits.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/utility.h>
#include <proto/expansion.h>

#include <proto/ahi_sub.h>
#include <clib/ahi_sub_protos.h>
#include <clib/debug_protos.h>

#include <math.h>
#include <string.h>
#include <stdint.h>

#include "zz9000ax-ahi.h"

// Comment out to enable debug output:
#define kprintf(...)

#define STR(s) #s
#define XSTR(s) STR(s)

#define DEVICE_NAME "zz9000ax.audio"
#define DEVICE_DATE "(24.04.2026)"
#define DEVICE_VERSION 4
#define DEVICE_REVISION 22
#define DEVICE_ID_STRING "ZZ9000AX " XSTR(DEVICE_VERSION) "." XSTR(DEVICE_REVISION) " " DEVICE_DATE
#define DEVICE_PRIORITY 0

#define REAL_HARDWARE 1

#define ZZ_BYTES_PER_PERIOD 3840
// AUDIO_BUFSZ is the hardware-side ring size (8 periods). The CPU-side
// bounce buffer is only ever filled one period at a time — see BOUNCE_BUFSZ.
#define AUDIO_BUFSZ (ZZ_BYTES_PER_PERIOD*8) // TODO: query from hardware
#define BOUNCE_BUFSZ ZZ_BYTES_PER_PERIOD
// AHI sample frames = 16-bit stereo => 4 bytes per frame. This is what
// ahiac_BuffSamples counts and what AHIDB_MaxPlaySamples must advertise;
// keep this in sync with the mixer byte math (bytes = BuffSamples << 2)
// and with the bounce-buffer capacity check in WorkerProcess().
#define BOUNCE_MAX_FRAMES (BOUNCE_BUFSZ / 4)
// AmigaOS scheduling is strictly preemptive priority with no aging; any
// task at or above this priority stuck in a CPU loop will starve the mixer
// and produce audible stutter. Keep the mixer well above user-level tasks
// and slightly below critical system servers (timer.device = 127).
#define WORKER_PRIORITY 110

#define REG_ZZ_CONFIG       0x04
#define REG_ZZ_AUDIO_SWAB   0x70
#define REG_ZZ_AUDIO_SCALE  0x74
#define REG_ZZ_AUDIO_PARAM  0x76
#define REG_ZZ_AUDIO_VAL    0x78
#define REG_ZZ_AUDIO_CONFIG 0xF4

struct ExecBase     *SysBase;
struct Library      *UtilityBase;
struct Library      *AHIsubBase  = NULL;
struct DosLibrary   *DOSBase     = NULL;
struct z9ax_base    *Z9AXBase;
//struct GfxBase      *GraphicsBase = NULL;

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

// TW: register access routines for cleaner code.
static inline void write_reg(uint32_t base, uint16_t reg, uint16_t val)
{
  *((volatile uint16_t*)(base+reg)) = val;
}

static inline uint16_t read_reg(uint32_t base, uint16_t reg)
{
  return *((volatile uint16_t*)(base+reg));
}

static inline void write_audio_param(uint32_t base, uint16_t param, uint16_t val)
{
  *((volatile uint16_t*)(base+REG_ZZ_AUDIO_PARAM)) = param;
  *((volatile uint16_t*)(base+REG_ZZ_AUDIO_VAL))   = val;
  *((volatile uint16_t*)(base+REG_ZZ_AUDIO_PARAM)) = 0;
}

const char device_name[] = DEVICE_NAME;
const char device_id_string[] = DEVICE_ID_STRING;

#define ZZ_NUM_FREQS 6

const uint16_t freqs[ZZ_NUM_FREQS] = {
  8000,
  12000,
  24000,
  32000,
  44100,
  48000,
};

// NOTE: Non-const globals above are written exactly once during init() and
// treated as read-only thereafter. Do not introduce other mutable globals:
// a Resident/AutoInit device's BSS is not reliably zeroed by all loaders,
// and stale state will corrupt AHI code/data across OpenDevice cycles.

#define debugmsg(v) while(0) {};

static uint32_t __attribute__((used)) init(BPTR seg_list asm("a0"), struct Library *dev asm("d0"))
{
  struct ConfigDev* cd = NULL;

  SysBase = *(struct ExecBase **)4L;
  Z9AXBase = (struct z9ax_base*)dev;

  // BSS/driver-base may be reused across AutoInit reloads; start clean
  // so AllocAudio's hw_addr/zorro_version gates can't be fooled by stale
  // state left over from a previous failed init.
  Z9AXBase->zorro_version = 0;
  Z9AXBase->hw_addr = 0;
  Z9AXBase->hw_size = 0;
  Z9AXBase->flags = 0;

  // Same reasoning for the library-base globals: the fail: label below
  // calls CloseLibrary on any non-NULL base, so leftover pointers from a
  // previous failed init must not leak in.
  DOSBase = NULL;
  UtilityBase = NULL;
  ExpansionBase = NULL;
  IntuitionBase = NULL;

  if (!(DOSBase = (struct DosLibrary *)OpenLibrary((STRPTR)"dos.library",0)))
    goto fail;

  if (!(UtilityBase = (struct Library *)OpenLibrary((STRPTR)"utility.library",0)))
    goto fail;

  if (!(ExpansionBase = (struct ExpansionBase *)OpenLibrary((STRPTR)"expansion.library",0)))
    goto fail;

  // TW: Zorro2/3 detection during early init phase.
  // Find Z2 or Z3 model of MNT ZZ9000
  if ((cd = (struct ConfigDev*)FindConfigDev(NULL,0x6d6e,0x4))) {
    // ZORRO 3
    Z9AXBase->zorro_version = 3;
    Z9AXBase->hw_addr = (uint32_t)cd->cd_BoardAddr;
    Z9AXBase->hw_size = (uint32_t)cd->cd_BoardSize;
  }
  else if ((cd = (struct ConfigDev*)FindConfigDev(NULL,0x6d6e,0x3))) {
    // ZORRO 2
    Z9AXBase->zorro_version = 2;
    Z9AXBase->hw_addr = (uint32_t)cd->cd_BoardAddr;
    Z9AXBase->hw_size = (uint32_t)cd->cd_BoardSize;
  } else {
    // Not detected — Z9AXBase already zeroed above.
    goto fail;
  }

  BPTR fh;
  if ((fh=Open((CONST_STRPTR)"ENV:ZZ9K_INT2",MODE_OLDFILE))) {
    kprintf((CONST_STRPTR)"ZZ9000AX: Using INT2 mode.\n");
    Close(fh);
    Z9AXBase->flags |= DEVF_INT2MODE;
  } else {
    kprintf((CONST_STRPTR)"ZZ9000AX: Using INT6 mode (default).\n");
  }

  return (uint32_t)dev;

fail:
  if (ExpansionBase) { CloseLibrary((struct Library *)ExpansionBase); ExpansionBase = NULL; }
  if (UtilityBase)   { CloseLibrary((struct Library *)UtilityBase);   UtilityBase   = NULL; }
  if (DOSBase)       { CloseLibrary((struct Library *)DOSBase);       DOSBase       = NULL; }
  return 0;
}

static uint8_t* __attribute__((used)) expunge(struct Library *libbase asm("a6"))
{
  if(DOSBase)       { CloseLibrary((struct Library *)DOSBase); DOSBase = NULL; }
  if(UtilityBase)   { CloseLibrary((struct Library *)UtilityBase); UtilityBase = NULL; }
  if(ExpansionBase) { CloseLibrary((struct Library *)ExpansionBase); ExpansionBase = NULL; }

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

  // OpenLibrary/OpenDevice callers are not required to Forbid; guard the
  // increment explicitly so concurrent opens can't race the counter.
  Forbid();
  dev->lib_OpenCnt++;
  Permit();
}

static uint8_t* __attribute__((used)) close(struct Library *dev asm("a6"), struct IORequest *iotd asm("a1"))
{
  // Mirror the Forbid() guard used in open() so the open-count stays consistent
  // if two tasks close the device concurrently.
  Forbid();
  if (dev->lib_OpenCnt > 0) dev->lib_OpenCnt--;
  Permit();
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

void WorkerProcess() {
  struct Process* proc = (struct Process *) FindTask(NULL);
  struct z9ax* ahi_data = proc->pr_Task.tc_UserData;
  struct AHIAudioCtrlDrv* AudioCtrl = ahi_data->audioctrl;

#ifndef REAL_HARDWARE
  uint8_t* glob_buf = AllocVec(ZZ_BYTES_PER_PERIOD*2, MEMF_ANY);
#endif

  ahi_data->worker_signal = AllocSignal(-1);
  ahi_data->enable_signal = AllocSignal(-1);

  // If either signal failed, bail out without entering the mix loop.
  if (ahi_data->worker_signal == -1 || ahi_data->enable_signal == -1) {
    if (ahi_data->worker_signal != -1) { FreeSignal(ahi_data->worker_signal); ahi_data->worker_signal = -1; }
    if (ahi_data->enable_signal != -1) { FreeSignal(ahi_data->enable_signal); ahi_data->enable_signal = -1; }
#ifndef REAL_HARDWARE
    if (glob_buf) FreeVec(glob_buf);
#endif
    // Clear worker_process so AllocAudio can detect the failure after the handshake.
    ahi_data->worker_process = NULL;
    Signal((struct Task *)ahi_data->t_mainproc, 1L << ahi_data->mainproc_signal);
    return;
  }

  uint32_t signals = 0;
  uint32_t buf_offset = 0;

  Signal(ahi_data->t_mainproc, 1L << ahi_data->mainproc_signal);

  for(;;) {
    signals = Wait(SIGBREAKF_CTRL_C | (1L<<ahi_data->enable_signal));
    if (signals & SIGBREAKF_CTRL_C) break;

    // A pending enable_signal may have been latched by the ISR between
    // Stop()/teardown setting play_stop and this wake-up. Drop those
    // cycles on the floor — the hardware is already (or about to be)
    // disabled and the AHI layer may be mid-teardown.
    if (ahi_data->play_stop) continue;

    CallHookPkt(AudioCtrl->ahiac_PlayerFunc, AudioCtrl, NULL);

    if (!(*AudioCtrl->ahiac_PreTimer)()) {
      // Defence in depth: the mixer writes ahiac_BuffSamples*4 bytes into
      // our bounce buffer. We set BuffSamples to MixFreq/50 in AllocAudio,
      // which is bounded by our advertised max mix rate (48 kHz => 960
      // frames => 3840 bytes = BOUNCE_BUFSZ). If anything ever drifts —
      // AHI layer override, higher mix rate added to freqs[], buffer
      // shrunk — catch it here instead of smashing memory.
      if (AudioCtrl->ahiac_BuffSamples > BOUNCE_MAX_FRAMES) {
        kprintf((CONST_STRPTR)"ZZ9000AX: BuffSamples %ld exceeds bounce cap %ld; skipping\n",
                (long)AudioCtrl->ahiac_BuffSamples, (long)BOUNCE_MAX_FRAMES);
        (*AudioCtrl->ahiac_PostTimer)();
        continue;
      }
#ifdef REAL_HARDWARE
      CallHookPkt(AudioCtrl->ahiac_MixerFunc, AudioCtrl, (void*)ahi_data->audio_buf_addr);
#else
      CallHookPkt(AudioCtrl->ahiac_MixerFunc, AudioCtrl, glob_buf);
      uint32_t* xbuf = (uint32_t*)glob_buf;
      kprintf((uint8_t*)"%lx %lx %lx %lx\n", xbuf[0], xbuf[1], xbuf[2], xbuf[3]);
#endif
      uint32_t bytes = AudioCtrl->ahiac_BuffSamples << 2; //(AudioCtrl->ahiac_Flags & AHIACF_STEREO ? 2 : 1);

      int overrun = 0;
#ifdef REAL_HARDWARE
      write_reg(ahi_data->hw_addr, REG_ZZ_AUDIO_SCALE, AudioCtrl->ahiac_BuffSamples);

      // def. the faster way
      CopyMem((void*)ahi_data->audio_buf_addr, (void*)ahi_data->audio_hw_buf_addr + buf_offset, bytes);
      // byteswap, resample and play buffer
      write_reg(ahi_data->hw_addr, REG_ZZ_AUDIO_SWAB, buf_offset>>8);
      overrun = read_reg(ahi_data->hw_addr, REG_ZZ_AUDIO_SWAB);
#endif

      if (overrun == 1) {
        //memset((void*)ahi_data->audio_buf_addr, 0, AUDIO_BUFSZ);
        buf_offset = 0;
      } else {
        buf_offset += ZZ_BYTES_PER_PERIOD;
      }

      if (buf_offset>=AUDIO_BUFSZ) {
        buf_offset = 0;
      }

      (*AudioCtrl->ahiac_PostTimer)();
    }
  }

  Forbid();
  if (ahi_data->enable_signal != -1) { FreeSignal(ahi_data->enable_signal); ahi_data->enable_signal = -1; }
  if (ahi_data->worker_signal != -1) { FreeSignal(ahi_data->worker_signal); ahi_data->worker_signal = -1; }

#ifndef REAL_HARDWARE
  if (glob_buf) FreeVec(glob_buf);
#endif

  ahi_data->worker_process = NULL;
  Signal((struct Task *)ahi_data->t_mainproc, 1L << ahi_data->mainproc_signal);

  // Multitaking will resume at exit
}

// TW: C interrupt service routine called by ASM wrapper.
void cdev_isr(struct z9ax* data asm("a1")) {
  USHORT status = *(USHORT*)(data->hw_addr+REG_ZZ_CONFIG);

  // audio interrupt signal set?
  if (status & 2) {
    // ack/clear audio interrupt
    *(USHORT*)(data->hw_addr+REG_ZZ_CONFIG) = 8|32;

    if(data->disable_cnt) return;
    if(data->play_stop) return;
    if(data->worker_process) {
      Signal((struct Task*)data->worker_process, 1L<<data->enable_signal);
    }
  }
}

// TW: dev_isr is now an external asm wrapper.
extern uint32_t dev_isr(struct z9ax* data asm("a1"));

// Fill in the Interrupt server node so it's ready to be added to the
// int-server list. Kept separate from the actual AddIntServer call so the
// install can be performed atomically under the AllocAudio ownership Forbid().
static void prepare_irq_struct(struct z9ax* ahi_data) {
  struct Interrupt* irq = &ahi_data->irq;

  irq->is_Node.ln_Type = NT_INTERRUPT;
  irq->is_Node.ln_Pri = 126; // High priority: this ISR must react quickly.
  irq->is_Node.ln_Name = "ZZ9000AX";
  irq->is_Data = ahi_data;
  irq->is_Code = (void*)dev_isr;
}

// Install the interrupt server. MUST be called with Forbid() already active
// so the caller can combine the MHI-presence check and the AddIntServer into
// one atomic "claim" step (otherwise MHI could slip in between).
static void install_irq_server_locked(struct z9ax* ahi_data) {
  struct Interrupt* irq = &ahi_data->irq;
#ifdef REAL_HARDWARE
  if (ahi_data->flags & DEVF_INT2MODE) {
    AddIntServer(INTB_PORTS, irq);
  } else {
    AddIntServer(INTB_EXTER, irq);
  }
#else
  AddIntServer(INTB_VERTB, irq); // for debugging
#endif
  ahi_data->irq_installed = 1;
}

// Flip the hardware-side audio interrupt on. Called only once the worker is
// up and ready to handle the resulting signals.
static void enable_hw_interrupt(struct z9ax* ahi_data) {
#ifdef REAL_HARDWARE
  USHORT hw_config = read_reg(ahi_data->hw_addr, REG_ZZ_AUDIO_CONFIG);
  hw_config |= 1;
  write_reg(ahi_data->hw_addr, REG_ZZ_AUDIO_CONFIG, hw_config);
#else
  (void)ahi_data;
#endif
}

void destroy_interrupt(struct z9ax* ahi_data) {
  struct Interrupt* irq = &ahi_data->irq;

  if (!ahi_data->irq_installed) return;

#ifdef REAL_HARDWARE
  // disable HW interrupt
  write_reg(ahi_data->hw_addr, REG_ZZ_AUDIO_CONFIG, 0);

  Forbid();
  if (ahi_data->flags & DEVF_INT2MODE) {
    RemIntServer(INTB_PORTS, irq);
  } else {
    RemIntServer(INTB_EXTER, irq);
  }
  Permit();
#else
  Forbid();
  RemIntServer(INTB_VERTB, irq);
  Permit();
#endif
  ahi_data->irq_installed = 0;
}

// Check whether MHI has its ISR installed on our shared interrupt level.
// MUST be called with Forbid() already active so that the caller can combine
// the check with AddIntServer() into a single atomic claim step. The
// intuition IntVects[] server list can be mutated by AddIntServer/
// RemIntServer from any task, so walking it unprotected would be unsafe.
static BOOL mhi_present_locked(void) {
  struct List *IrqList;
  if(Z9AXBase->flags & DEVF_INT2MODE) {
    IrqList = (struct List *)SysBase->IntVects[INTB_PORTS].iv_Data;
  }
  else {
    IrqList = (struct List *)SysBase->IntVects[INTB_EXTER].iv_Data;
  }
  return FindName(IrqList, (CONST_STRPTR)"mhizz9000") ? TRUE : FALSE;
}

// Read an optional user override for AP_DSP_SET_VOLUMES from
// ENV:ZZ9K_MIX_LEVELS. The file (created e.g. with
// `setenv ZZ9K_MIX_LEVELS C040`) contains a 1-4 digit hex string packing
// ZZ9000AX/AHI output level in the high byte and Paula pass-through in
// the low byte. "0x" prefix, leading whitespace and trailing newlines
// are tolerated. Returns `default_value` on any failure so a stale or
// mangled file can never brick audio.
static uint16_t read_mix_levels_env(uint16_t default_value)
{
  BPTR fh;
  UBYTE buf[16];
  LONG len;
  uint16_t out = 0;
  int digits = 0;
  int i;

  if (!DOSBase) return default_value;
  fh = Open((CONST_STRPTR)"ENV:ZZ9K_MIX_LEVELS", MODE_OLDFILE);
  if (!fh) return default_value;
  len = Read(fh, buf, sizeof(buf) - 1);
  Close(fh);
  if (len <= 0) return default_value;
  buf[len] = 0;

  for (i = 0; i < len && digits < 4; i++) {
    UBYTE c = buf[i];
    int d;
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      if (digits > 0) break;
      continue;
    }
    if (digits == 0 && c == '0' && (i + 1 < len) &&
        (buf[i + 1] == 'x' || buf[i + 1] == 'X')) {
      i++;
      continue;
    }
    if (c >= '0' && c <= '9') d = c - '0';
    else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
    else break;
    out = (uint16_t)((out << 4) | d);
    digits++;
  }
  return (digits > 0) ? out : default_value;
}

static uint32_t __attribute__((used)) intAHIsub_AllocAudio(struct TagItem *tagList asm("a1"), struct AHIAudioCtrlDrv *AudioCtrl asm("a2")) {
  // TW: Just take the values from where init() has already stored them.
  uint32_t hw_addr = Z9AXBase->hw_addr;
  int zorro = Z9AXBase->zorro_version;
  if(!hw_addr) return AHISF_ERROR; // TW: Only AHISF_xxx return codes are allowed here.
  if(!zorro) return AHISF_ERROR; // TW: Only AHISF_xxx return codes are allowed here.

  // REG_ZZ_AUDIO_CONFIG bit 0 is the "AX present" strap; mask explicitly so
  // other status bits can't ever make this look like detection succeeded.
  int ax_present = read_reg(hw_addr, REG_ZZ_AUDIO_CONFIG) & 1;
  if (!ax_present) {
    const char *alert = "\x00\x14\x14ZZ9000AX not detected. AHI driver will exit.\x00\x00";
    if (!IntuitionBase) {
      IntuitionBase = (struct IntuitionBase*)OpenLibrary((STRPTR)"intuition.library",37);
      if (IntuitionBase) {
        DisplayAlert(RECOVERY_ALERT, (APTR)alert, 52);
        CloseLibrary((struct Library *)IntuitionBase);
        IntuitionBase = NULL;
      }
    }
    return AHISF_ERROR; // TW: Only AHISF_xxx return codes are allowed here.
  }

  struct z9ax *ahi_data = AllocVec(sizeof(struct z9ax), MEMF_PUBLIC | MEMF_FAST | MEMF_CLEAR);
  // allocate bounce buffer, as letting AHI write directly to hardware is slower than CopyMem
  void* audio_buf = AllocVec(BOUNCE_BUFSZ, MEMF_PUBLIC | MEMF_FAST | MEMF_CLEAR);

  if (!ahi_data || !audio_buf) {
    if (audio_buf) FreeVec(audio_buf);
    if (ahi_data)  FreeVec(ahi_data);
    return AHISF_ERROR; // TW: Only AHISF_xxx return codes are allowed here.
  }

  // TW: Upon allocation playback is initially stopped.
  ahi_data->play_stop = 1;
  ahi_data->flags = Z9AXBase->flags;
  ahi_data->audio_buf_addr = (uint32_t)audio_buf;
  ahi_data->hw_addr = hw_addr;
  ahi_data->audioctrl = AudioCtrl;
  ahi_data->ahi_base = AHIsubBase;
  ahi_data->worker_signal = -1;
  ahi_data->enable_signal = -1;
  ahi_data->mainproc_signal = -1;
  ahi_data->zorro_version = zorro;
  ahi_data->t_mainproc = FindTask(NULL);
  // FIXME: see also zz_template_addr in RTG driver
  uint32_t offset_tx = Z9AXBase->hw_size - 0x20000;
  ahi_data->audio_hw_buf_addr = hw_addr + 0x10000 + offset_tx;

  AudioCtrl->ahiac_DriverData = ahi_data;

  // Atomic ownership claim: check MHI isn't already on the shared interrupt
  // level AND install our own ISR node in a single Forbid() window. Doing
  // the two steps under one Forbid closes the TOCTOU that would otherwise
  // let AHI and MHI both "win" the claim (we saw it; we took it; nobody
  // could slip in between). The HW-side interrupt stays OFF until the
  // worker is up — see enable_hw_interrupt() near the end.
  prepare_irq_struct(ahi_data);
  Forbid();
  if (mhi_present_locked()) {
    Permit();
    kprintf((CONST_STRPTR)"Can't allocate! Hardware already used by MHI.\n");
    FreeVec(audio_buf);
    FreeVec(ahi_data);
    AudioCtrl->ahiac_DriverData = NULL;
    return AHISF_ERROR;
  }
  install_irq_server_locked(ahi_data);
  Permit();

  int lpf_freq = AudioCtrl->ahiac_MixFreq / 2;

  // filter has issues near 24KHz
  if (lpf_freq > 23900) lpf_freq = 23900;

  BPTR f;
  if ((f = Open((APTR)"ENV:ZZ9000AX-NOLPF", MODE_OLDFILE))) {
    Close(f);
    // turn off auto low pass filter
    lpf_freq = 23900;
  }

  Forbid();
  // set tx buffer address
  write_audio_param(hw_addr, 0, offset_tx>>16);
  write_audio_param(hw_addr, 1, offset_tx&0xffff);

  // set LPF freq to half of sampling freq
  write_audio_param(hw_addr, 9, lpf_freq);

  // Balanced Paula-vs-ZZ9000AX output mixer default (param 10,
  // AP_DSP_SET_VOLUMES). High byte = ZZ9000AX/AHI level, low byte = Paula
  // pass-through level, each 0x00-0xFF. Summing both above ~0x100 starts
  // saturating the DAC (per MNT forum thread 1011).
  //
  // Default 0xC040 compensates for early ZZ9000AX revisions that carry
  // an opamp on U4 which over-amplifies the Paula pass-through and makes
  // raw Paula dominate over AHI playback: we boost AHI to 0xC0 (~1.5x)
  // and cut Paula to 0x40 (~0.5x) to pull them toward parity. Users with
  // the fixed-hardware revision (U4 opamp desoldered by MNT) can override
  // via `setenv ZZ9K_MIX_LEVELS 8080` (or any 4-digit hex value) to get a
  // symmetric mix back.
  write_audio_param(hw_addr, 10, read_mix_levels_env(0xC040));
  Permit();

  // Zero the hardware audio ring buffer before we enable playback. The
  // FPGA DAC starts consuming from audio_hw_buf_addr as soon as the HW
  // audio interrupt is armed, and whatever garbage was left there by a
  // previous MHI session, a previous AHI session, or power-on junk will
  // be played as a short burst before the worker writes the first mixed
  // period. AUDIO_BUFSZ is the full ring size (8 periods); zeroing all
  // of it means the DAC plays silence until real data lands.
  {
    volatile uint8_t *hw_buf = (volatile uint8_t *)ahi_data->audio_hw_buf_addr;
    uint32_t i;
    for (i = 0; i < AUDIO_BUFSZ; i++) hw_buf[i] = 0;
  }

  ahi_data->mainproc_signal = AllocSignal(-1);
  if (ahi_data->mainproc_signal == -1) {
    kprintf((CONST_STRPTR)"ZZ9000AX: AllocSignal failed\n");
    goto fail;
  }

  Forbid();
  ahi_data->worker_process = CreateNewProcTags(NP_Entry,    (uint32_t)&WorkerProcess,
                                               NP_Name,     (uint32_t)device_name,
                                               NP_Priority, WORKER_PRIORITY,
                                               TAG_DONE);
  if (ahi_data->worker_process) {
    ahi_data->worker_process->pr_Task.tc_UserData = ahi_data;
  }
  Permit();

  if (!ahi_data->worker_process) {
    kprintf((CONST_STRPTR)"ZZ9000AX: CreateNewProcTags failed\n");
    goto fail;
  }

  // Wait for worker to finish its early init (signal allocation, etc.)
  Wait(1L << ahi_data->mainproc_signal);

  // Worker may have failed to allocate its signals; it clears itself in that case.
  if (!ahi_data->worker_process) {
    kprintf((CONST_STRPTR)"ZZ9000AX: worker failed to init\n");
    goto fail;
  }

  // Worker is up; safe to let the hardware start firing audio interrupts.
  enable_hw_interrupt(ahi_data);

  // yields 960 samples (3840 bytes) for 48000Hz
  AudioCtrl->ahiac_BuffSamples = AudioCtrl->ahiac_MixFreq/50;

  // none of that weird timing
  return AHISF_KNOWSTEREO | AHISF_MIXING; // | AHISF_TIMING;

fail:
  // Invariant at this label: the worker has NOT been fully brought up.
  // Either mainproc_signal allocation failed, CreateNewProcTags failed,
  // or the worker signalled back with worker_process cleared (signal alloc
  // failed). We must never reach fail: with a live worker, otherwise it
  // would be orphaned.
  // The interrupt server was already installed as part of the atomic claim
  // earlier, so we must release it here before freeing ahi_data; otherwise
  // RemIntServer would walk a freed node next time something probes.
  destroy_interrupt(ahi_data);
  if (ahi_data->mainproc_signal != -1) {
    FreeSignal(ahi_data->mainproc_signal);
    ahi_data->mainproc_signal = -1;
  }
  if (ahi_data->audio_buf_addr) {
    FreeVec((void*)ahi_data->audio_buf_addr);
    ahi_data->audio_buf_addr = 0;
  }
  FreeVec(ahi_data);
  AudioCtrl->ahiac_DriverData = NULL;
  return AHISF_ERROR;
}

static void __attribute__((used)) intAHIsub_FreeAudio(struct AHIAudioCtrlDrv *AudioCtrl asm("a2")) {
  if (!AudioCtrl->ahiac_DriverData) return;

  struct z9ax *ahi_data = AudioCtrl->ahiac_DriverData;

  // Make sure the worker's mix loop won't try to touch hardware after we tear down.
  ahi_data->play_stop = 1;

  // Remove ISR first so we know nothing will signal the worker concurrently.
  destroy_interrupt(ahi_data);

  if (ahi_data->worker_process) {
    Signal((struct Task *)ahi_data->worker_process, SIGBREAKF_CTRL_C);
    // Worker clears worker_process and signals mainproc_signal on exit.
    if (ahi_data->mainproc_signal != -1) {
      Wait(1L << ahi_data->mainproc_signal);
    }
    ahi_data->worker_process = NULL;
  }

  if (ahi_data->mainproc_signal != -1) {
    FreeSignal(ahi_data->mainproc_signal);
    ahi_data->mainproc_signal = -1;
  }

  if (ahi_data->audio_buf_addr) {
    FreeVec((void*)ahi_data->audio_buf_addr);
    ahi_data->audio_buf_addr = 0;
  }

  FreeVec(AudioCtrl->ahiac_DriverData);
  AudioCtrl->ahiac_DriverData = NULL;
}

// TW: Prepared Stop() and Start() to store status in a flag in z9ax.
static void __attribute__((used)) intAHIsub_Stop(uint32_t Flags asm("d0"), struct AHIAudioCtrlDrv *AudioCtrl asm("a2")) {
  struct z9ax *ahi_data = AudioCtrl->ahiac_DriverData;
  if (!ahi_data) return;
  if (Flags & AHISF_PLAY) {
    ahi_data->play_stop = 1;
  }
}

static uint32_t __attribute__((used)) intAHIsub_Start(uint32_t flags asm("d0"), struct AHIAudioCtrlDrv *AudioCtrl asm("a2")) {
  struct z9ax *ahi_data = AudioCtrl->ahiac_DriverData;
  if (!ahi_data) return AHIE_OK;
  if (flags & AHISF_PLAY) {
    ahi_data->play_stop = 0;
  }
  // Returns AHIE_OK if successful.
  return AHIE_OK;
}

static int32_t __attribute__((used)) intAHIsub_GetAttr(uint32_t attr_ asm("d0"), int32_t arg_ asm("d1"), int32_t def_ asm("d2"), struct TagItem *tagList asm("a1"), struct AHIAudioCtrlDrv *AudioCtrl asm("a2")) {
  uint32_t attr = attr_;
  int32_t arg = arg_, def = def_;

  // TW: We can't rely on AudioCtrl->ahiac_DriverData being valid during this function call!

  switch(attr)
    {
    case AHIDB_Bits:
      return 16;
    case AHIDB_Frequencies:
      return ZZ_NUM_FREQS;
    case AHIDB_Frequency:
      return freqs[arg];
    case AHIDB_Index:
      for (int i = 0; i < ZZ_NUM_FREQS; i++) {
        if (freqs[i] >= arg)
          return i;
      }
      return ZZ_NUM_FREQS-1;
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
      return 1;
    case AHIDB_MaxPlaySamples:
      // AHI contract: this is sample frames, NOT bytes. At the highest mix
      // rate we advertise (48 kHz) the driver sets ahiac_BuffSamples to
      // MixFreq/50 = 960 frames, which is exactly BOUNCE_MAX_FRAMES.
      return BOUNCE_MAX_FRAMES;
    case AHIDB_MaxRecordSamples:
      return 0;
    case AHIDB_MinMonitorVolume:
      return 0x0;
    case AHIDB_MaxMonitorVolume:
      return 0x0;
    case AHIDB_MinInputGain:
      return 0x0;
    case AHIDB_MaxInputGain:
      return 0x0;
    case AHIDB_MinOutputVolume:
      return 0x0;
    case AHIDB_MaxOutputVolume:
      return 0x0;
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

// TW: C routines called by ASM wrappers which preserve all registers.
void __attribute__((used)) cintAHIsub_Enable(struct AHIAudioCtrlDrv *AudioCtrl asm("a2"))
{
  struct z9ax *ahi_data = AudioCtrl->ahiac_DriverData;
  if (!ahi_data) return;
  if (ahi_data->disable_cnt > 0) {
    ahi_data->disable_cnt--;
  }
}

void __attribute__((used)) cintAHIsub_Disable(struct AHIAudioCtrlDrv *AudioCtrl asm("a2"))
{
  struct z9ax *ahi_data = AudioCtrl->ahiac_DriverData;
  if (!ahi_data) return;
  ahi_data->disable_cnt++;
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

extern void __attribute__((used)) intAHIsub_Enable(struct AHIAudioCtrlDrv *AudioCtrl asm("a2"));
extern void __attribute__((used)) intAHIsub_Disable(struct AHIAudioCtrlDrv *AudioCtrl asm("a2"));

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
  sizeof(struct z9ax_base), // TW: This is the size of z9ax_base, not the size of the driver data.
  (uint32_t)function_table,
  0,
  (uint32_t)init,
};
