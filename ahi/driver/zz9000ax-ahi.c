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

#include "zz9000_ax.h"
#include "zzcfg_query.h"
#include "zz9000ax-ahi.h"
#include "zz9000_aperture.h"

#include "zz9k/library_vectors.h"
#include "zz9k/request.h"
#include <zz9k/audio.h>
#include <proto/zz9k.h>

// Comment out to enable debug output:
#define kprintf(...)

#define STR(s) #s
#define XSTR(s) STR(s)

#define DEVICE_NAME "zz9000ax.audio"
#define DEVICE_DATE "(17.08.2026)"
#define DEVICE_VERSION 4
#define DEVICE_REVISION 26
#define DEVICE_ID_STRING "ZZ9000AX " XSTR(DEVICE_VERSION) "." XSTR(DEVICE_REVISION) " " DEVICE_DATE
#define DEVICE_PRIORITY 0

#define REAL_HARDWARE 1

// ZZ_AX_AUDIO_BUFSZ is the hardware-side ring size (8 periods). The CPU-side
// bounce buffer is only ever filled one period at a time — see BOUNCE_BUFSZ.
#define BOUNCE_BUFSZ ZZ_AX_BOUNCE_BUFSZ
// AHI sample frames = 16-bit stereo => 4 bytes per frame. This is what
// ahiac_BuffSamples counts and what AHIDB_MaxPlaySamples must advertise;
// keep this in sync with the mixer byte math (bytes = BuffSamples << 2)
// and with the bounce-buffer capacity check in WorkerProcess().
#define BOUNCE_MAX_FRAMES ZZ_AX_BOUNCE_MAX_FRAMES
// Lease-mode staging headroom: periods mixed ahead of firmware's
// consumed cursor. 4 = 80 ms of runway; the compositor consumes one
// period per 20-ms tick, so the timer keeps this topped up.
#define LEASE_RUNWAY_PERIODS 2

// AmigaOS scheduling is strictly preemptive priority with no aging; any
// task at or above this priority stuck in a CPU loop will starve the mixer
// and produce audible stutter. Keep the mixer well above user-level tasks
// and slightly below critical system servers (timer.device = 127).
#define WORKER_PRIORITY 110

struct ExecBase     *SysBase;
struct Library      *UtilityBase;
struct Library      *AHIsubBase  = NULL;
struct DosLibrary   *DOSBase     = NULL;
struct z9ax_base    *Z9AXBase;
// zz9k.library base for the proto inline calls; opened per AllocAudio
// (AHI's low-level API is exclusive, so at most one owner exists) and
// closed again on FreeAudio. NULL means "no control plane" -- legacy
// playback. Cleared in init() for the same BSS-reuse hygiene as the
// library bases above.
struct Library      *ZZ9KBase    = NULL;
// Whether the firmware advertised ZZ9K_CAP_AUDIO_CONTROL at allocate:
// gates the release-time neutral trim submit and the legacy LPF stamp
// (absent capability = old firmware, pre-scene behavior). Mirrors the
// ZZ9KBase lifetime: set at allocate, cleared at release.
static int audio_control_capped = 0;
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
  *((volatile uint16_t*)(base+ZZ_REG_AUDIO_PARAM)) = param;
  *((volatile uint16_t*)(base+ZZ_REG_AUDIO_VAL))   = val;
  *((volatile uint16_t*)(base+ZZ_REG_AUDIO_PARAM)) = 0;
}

static BOOL recording_supported(uint32_t hw_addr)
{
  if (!hw_addr) return FALSE;

  return (read_reg(hw_addr, ZZ_REG_AUDIO_CONFIG) & 1) &&
         (read_reg(hw_addr, ZZ_REG_AUDIO_RX_STATUS) &
          ZZ_AX_AUDIO_RX_STATUS_CAPABLE);
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
  Z9AXBase->owner = NULL;

  // Same reasoning for the library-base globals: the fail: label below
  // calls CloseLibrary on any non-NULL base, so leftover pointers from a
  // previous failed init must not leak in.
  DOSBase = NULL;
  UtilityBase = NULL;
  ExpansionBase = NULL;
  IntuitionBase = NULL;
  ZZ9KBase = NULL;
  audio_control_capped = 0;

  if (!(DOSBase = (struct DosLibrary *)OpenLibrary((STRPTR)"dos.library",0)))
    goto fail;

  if (!(UtilityBase = (struct Library *)OpenLibrary((STRPTR)"utility.library",0)))
    goto fail;

  if (!(ExpansionBase = (struct ExpansionBase *)OpenLibrary((STRPTR)"expansion.library",0)))
    goto fail;

  // TW: Zorro2/3 detection during early init phase.
  // Find Z2 or Z3 model of MNT ZZ9000
  if ((cd = (struct ConfigDev*)FindConfigDev(NULL, ZZ9000_MNT_MANUFACTURER,
                                             ZZ9000_PRODUCT_Z3))) {
    // ZORRO 3
    Z9AXBase->zorro_version = 3;
    Z9AXBase->hw_addr = (uint32_t)cd->cd_BoardAddr;
    Z9AXBase->hw_size = (uint32_t)cd->cd_BoardSize;
  }
  else if ((cd = (struct ConfigDev*)FindConfigDev(NULL, ZZ9000_MNT_MANUFACTURER,
                                                  ZZ9000_PRODUCT_Z2))) {
    // ZORRO 2
    Z9AXBase->zorro_version = 2;
    Z9AXBase->hw_addr = (uint32_t)cd->cd_BoardAddr;
    Z9AXBase->hw_size = (uint32_t)cd->cd_BoardSize;
  } else {
    // Not detected — Z9AXBase already zeroed above.
    goto fail;
  }

  BPTR fh;
  UWORD cfg_present = 0;
  if ((fh=Open((CONST_STRPTR)ZZ_AX_INT2_ENV, MODE_OLDFILE))) {
    kprintf((CONST_STRPTR)"ZZ9000AX: Using INT2 mode (ENV).\n");
    Close(fh);
    Z9AXBase->flags |= ZZ_AX_DEVF_INT2MODE;
  } else if (zzcfg_query(Z9AXBase->hw_addr, ZZ_CFG_KEY_INT2, &cfg_present) &&
             cfg_present) {
    // `int2 = on` in ZZ9000.CFG (firmware 2.3+)
    kprintf((CONST_STRPTR)"ZZ9000AX: Using INT2 mode (ZZ9000.CFG).\n");
    Z9AXBase->flags |= ZZ_AX_DEVF_INT2MODE;
  } else {
    kprintf((CONST_STRPTR)"ZZ9000AX: Using INT6 mode (default).\n");
  }

  // The mix-levels env override is gone (R11): Paula/AX balance intent
  // now flows through the firmware control plane's operator baseline.
  // Probe for the stale variable's existence only -- never parse a
  // value -- and tell operators still carrying the old early-R1 remedy
  // where the replacement lives. KPrintF (not the locally disabled
  // kprintf) so the one-liner is always visible on the debug channel.
  if ((fh = Open((CONST_STRPTR)"ENV:ZZ9K_MIX_LEVELS", MODE_OLDFILE))) {
    Close(fh);
    KPrintF((CONST_STRPTR)"ZZ9000AX: ENV:ZZ9K_MIX_LEVELS is ignored; "
            "balance needs matched firmware (scene baseline).\n");
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

static void process_recording(struct z9ax *ahi_data,
                              struct AHIAudioCtrlDrv *AudioCtrl)
{
#ifdef REAL_HARDWARE
  uint16_t status = read_reg(ahi_data->hw_addr, ZZ_REG_AUDIO_RX_STATUS);
  uint16_t sequence;
  uint16_t available;
  uint8_t newest_period;
  uint16_t index;
  uint32_t bytes;

  if (!(status & ZZ_AX_AUDIO_RX_STATUS_CAPABLE)) return;

  sequence = zz_ax_audio_rx_status_sequence(status);
  available = zz_ax_audio_rx_sequence_distance(sequence,
                                                ahi_data->record_sequence);
  if (!available) return;
  if (available > ZZ_AX_AUDIO_RX_RESIDENT_PERIODS)
    available = ZZ_AX_AUDIO_RX_RESIDENT_PERIODS;

  if (AudioCtrl->ahiac_BuffSamples > BOUNCE_MAX_FRAMES) {
    ahi_data->record_sequence = sequence;
    return;
  }

  newest_period = zz_ax_audio_rx_status_period(status);
  bytes = AudioCtrl->ahiac_BuffSamples << 2;

  for (index = 0; index < available; index++) {
    uint8_t period = (uint8_t)((newest_period + ZZ_AX_AUDIO_PERIODS -
                                available + 1U + index) %
                               ZZ_AX_AUDIO_PERIODS);

    CopyMem((void *)(ahi_data->audio_rx_hw_buf_addr +
                     period * ZZ_AX_BYTES_PER_PERIOD),
            (void *)ahi_data->record_buf_addr, bytes);

    ahi_data->record_message.ahirm_Type = AHIST_S16S;
    ahi_data->record_message.ahirm_Buffer =
        (void *)ahi_data->record_buf_addr;
    ahi_data->record_message.ahirm_Length = AudioCtrl->ahiac_BuffSamples;

    if (AudioCtrl->ahiac_SamplerFunc) {
      CallHookPkt(AudioCtrl->ahiac_SamplerFunc, AudioCtrl,
                  &ahi_data->record_message);
    }

    if (ahi_data->record_stop) break;
  }

  ahi_data->record_sequence = sequence;
#else
  (void)ahi_data;
  (void)AudioCtrl;
#endif
}

static BOOL playback_period_ready(struct z9ax *ahi_data,
                                  uint32_t *period_offset)
{
  if (ahi_data->play_stop) return FALSE;

#ifdef REAL_HARDWARE
  /* Capture-capable firmware publishes a TX sequence so a capture-only
   * assertion of the shared audio interrupt cannot advance playback. */
  if (ahi_data->record_capable) {
    uint16_t status = read_reg(ahi_data->hw_addr,
                               ZZ_REG_AUDIO_TX_STATUS);
    uint16_t sequence =
        (ahi_data->tx_status_capable &&
         (status & ZZ_AX_AUDIO_TX_STATUS_CAPABLE)) ?
            zz_ax_audio_tx_status_sequence(status) : status;

    if (sequence == ahi_data->play_sequence) return FALSE;
    ahi_data->play_sequence = sequence;
    if (ahi_data->tx_status_capable &&
        (status & ZZ_AX_AUDIO_TX_STATUS_CAPABLE)) {
      /*
       * New firmware publishes the period MM2S most recently completed.
       * Refill that slot instead of assuming DMA began at ring offset zero:
       * the old startup guess could overwrite the active period, emit part
       * of the first sample immediately, then play it again after wrap.
       */
      *period_offset =
          (uint32_t)zz_ax_audio_tx_status_period(status) *
          ZZ_AX_BYTES_PER_PERIOD;
    }
  }
#endif

  return TRUE;
}

/* AHI's mixer writes m68k-native big-endian S16; the lease contract
 * is S16LE (the legacy register path let firmware do this swap -- the
 * SWAB register / audio_swab). The pump owns the conversion here: one
 * in-place word swap per staged period, before the bytes go to the
 * granted ring. */
static void fabric_swap_period_le(void *buffer, uint32_t bytes)
{
  uint16_t *words = (uint16_t *)buffer;
  uint32_t i;

  for (i = 0U; i < bytes / 2U; i++)
    words[i] = (uint16_t)((words[i] >> 8) | (words[i] << 8));
}



/* One lease-pacing pass (worker context, timer wake): adopt firmware
 * credits, stage whole source-rate periods while credited space and
 * the play direction allow (PlayerFunc/MixerFunc per period -- the
 * same cadence the legacy path drives from the card interrupt), then
 * publish the producer line: cursor after PCM, heartbeat on every
 * publication (R6/R11). The worker is the sole producer-line writer;
 * Stop(PLAY) only sets the session's PAUSED flag, which this publish
 * carries (cursor progress suppressed, heartbeat alive, R12). A
 * REVOKED credit snapshot (heartbeat expiry, cursor fault, foreign
 * generation) ends staging for this lease; firmware has already
 * freed the slot, and the stale release at FreeAudio is a no-op. */
static void fabric_lease_pump(struct z9ax *ahi_data,
                              struct AHIAudioCtrlDrv *AudioCtrl)
{
  ZZ9KAudioRingSession *session = &ahi_data->lease_session;

  if (!ahi_data->lease_held || !session->mapped)
    return;
  if (zz9k_audio_ring_take_credits(session, 4U) ==
      ZZ9K_AUDIO_RING_CREDIT_REVOKED) {
    KPrintF((CONST_STRPTR)"ZZ9000AX: fabric lease REVOKED; staging "
            "stopped (heartbeat/cursor/generation).\n");
    ahi_data->lease_held = 0;
    memset(session, 0, sizeof(*session));
    return;
  }
  if (AudioCtrl->ahiac_BuffSamples > BOUNCE_MAX_FRAMES)
    return;  /* the legacy defence-in-depth bound still applies */

  /* Whole-period staging (fixes the partial-fill equilibrium): the
   * grant's period is source_rate/50*4; ahi.device may run a
   * BuffSamples whose mix is smaller than that (player-requested
   * buffer sizes), and staging mix-sized chunks never lines up with
   * the lease period -- the fill silence-pads the gaps and padded
   * periods retire no credit, a self-sustaining partial fill
   * (measured 64% at 44.1 kHz). Mixer output accumulates in a
   * dedicated buffer and only whole lease periods enter the ring,
   * so every staged chunk is one tagged period and credits always
   * close. One mix per wake (PlayerFunc bursts starve decoders);
   * outstanding tops at LEASE_RUNWAY_PERIODS; the lease stays
   * PAUSED until two periods are staged (inaudible prefill). */
  {
    uint32_t lease_period =
        (session->grant.source_rate / 50U) * 4U;

    if (lease_period != 0U && lease_period <= BOUNCE_BUFSZ &&
        ahi_data->lease_accum != NULL &&
        !ahi_data->play_stop &&
        session->write_cursor - session->consumed_cursor <
            (uint64_t)LEASE_RUNWAY_PERIODS * lease_period) {
      CallHookPkt(AudioCtrl->ahiac_PlayerFunc, AudioCtrl, NULL);
      if (!(*AudioCtrl->ahiac_PreTimer)()) {
        uint32_t mix_bytes = AudioCtrl->ahiac_BuffSamples << 2;
        uint32_t room = BOUNCE_BUFSZ - ahi_data->lease_accum_fill;

        if (mix_bytes > room)
          mix_bytes = room;
        if (mix_bytes != 0U) {
          CallHookPkt(AudioCtrl->ahiac_MixerFunc, AudioCtrl,
                      (void *)(uintptr_t)ahi_data->audio_buf_addr);
          fabric_swap_period_le(
              (void *)(uintptr_t)ahi_data->audio_buf_addr,
              mix_bytes);
          memcpy(ahi_data->lease_accum +
                     ahi_data->lease_accum_fill,
                 (const void *)(uintptr_t)
                     ahi_data->audio_buf_addr,
                 mix_bytes);
          ahi_data->lease_accum_fill += mix_bytes;
        }
        (*AudioCtrl->ahiac_PostTimer)();
        while (ahi_data->lease_accum_fill >= lease_period &&
               zz9k_audio_ring_free_bytes(session) >=
                   lease_period) {
          if (zz9k_audio_ring_write(session, ahi_data->lease_accum,
                  lease_period) != lease_period)
            break;
          ahi_data->lease_accum_fill -= lease_period;
          memmove(ahi_data->lease_accum,
              ahi_data->lease_accum + lease_period,
              ahi_data->lease_accum_fill);
        }
      }
    }
    if ((session->flags & ZZ9K_AUDIO_RING_PRODUCER_FLAG_PAUSED) &&
        !ahi_data->play_stop &&
        session->write_cursor - session->consumed_cursor >=
            2ULL * lease_period) {
      session->flags &= ~ZZ9K_AUDIO_RING_PRODUCER_FLAG_PAUSED;
    }
  }
  if (!ahi_data->lease_traced) {
    ahi_data->lease_traced = 1;
    KPrintF((CONST_STRPTR)"ZZ9000AX: lease pump first pass: staged "
            "w=%lu consumed=%lu flags=%lx play_stop=%u.\n",
            (unsigned long)session->write_cursor,
            (unsigned long)session->consumed_cursor,
            (unsigned long)session->flags,
            (unsigned int)ahi_data->play_stop);
  }
  zz9k_audio_ring_publish(session);
}

/* (Re)arm the 10-ms UNIT_MICROHZ lease timer. */
static void fabric_timer_post(struct z9ax *ahi_data)
{
  ahi_data->lease_timer_req->tr_node.io_Command = TR_ADDREQUEST;
  ahi_data->lease_timer_req->tr_time.tv_secs = 0;
  ahi_data->lease_timer_req->tr_time.tv_micro = 10000;
  SendIO((struct IORequest *)ahi_data->lease_timer_req);
}

void WorkerProcess() {
  struct Process* proc = (struct Process *) FindTask(NULL);
  struct z9ax* ahi_data = proc->pr_Task.tc_UserData;
  struct AHIAudioCtrlDrv* AudioCtrl = ahi_data->audioctrl;

#ifndef REAL_HARDWARE
  uint8_t* glob_buf = AllocVec(ZZ_AX_BYTES_PER_PERIOD * 2, MEMF_ANY);
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

  Signal(ahi_data->t_mainproc, 1L << ahi_data->mainproc_signal);

  /* Fabric lease mode paces itself: a 10-ms UNIT_MICROHZ timer wakes
   * the worker to adopt credits, stage source-rate periods, and
   * refresh the lease heartbeat; the card interrupt keeps serving
   * the record direction exactly as before. Failure to create the
   * timer is fatal for lease mode only -- fall back is impossible
   * mid-session (the firmware owns the output), so the lease simply
   * never becomes active and Start() fails on its own merits. */
  uint32_t lease_timer_sig = 0;
  if (ahi_data->fabric_mode) {
    ahi_data->lease_timer_port = CreateMsgPort();
    if (ahi_data->lease_timer_port) {
      ahi_data->lease_timer_req = (struct timerequest *)CreateIORequest(
          (APTR)ahi_data->lease_timer_port, sizeof(struct timerequest));
      if (ahi_data->lease_timer_req) {
        if (OpenDevice((STRPTR)"timer.device", UNIT_MICROHZ,
                       (struct IORequest *)ahi_data->lease_timer_req,
                       0) == 0) {
          lease_timer_sig = 1L << ahi_data->lease_timer_port->mp_SigBit;
          fabric_timer_post(ahi_data);
        }
      }
    }
  }

  for(;;) {
    signals = Wait(SIGBREAKF_CTRL_C | (1L<<ahi_data->enable_signal) |
                   lease_timer_sig);
    if (signals & SIGBREAKF_CTRL_C) break;

    if (lease_timer_sig && (signals & lease_timer_sig)) {
      /* Drain the replied timer request and re-arm before pacing. */
      while (GetMsg(ahi_data->lease_timer_port))
        ;
      fabric_timer_post(ahi_data);
      fabric_lease_pump(ahi_data, AudioCtrl);
    }

    if (ahi_data->fabric_mode) {
      /* Record-only path: the card interrupt still drives capture. */
      if (!ahi_data->record_stop) process_recording(ahi_data, AudioCtrl);
      continue;
    }

    // A pending enable_signal may have been latched by the ISR between
    // Stop()/teardown updating the direction flags and this wake-up.
    if (ahi_data->play_stop && ahi_data->record_stop) continue;


    uint32_t period_offset = ahi_data->buf_offset;

    if (playback_period_ready(ahi_data, &period_offset)) {
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
                  (long)AudioCtrl->ahiac_BuffSamples,
                  (long)BOUNCE_MAX_FRAMES);
          (*AudioCtrl->ahiac_PostTimer)();
          if (!ahi_data->record_stop)
            process_recording(ahi_data, AudioCtrl);
          continue;
        }
#ifdef REAL_HARDWARE
        CallHookPkt(AudioCtrl->ahiac_MixerFunc, AudioCtrl,
                    (void*)ahi_data->audio_buf_addr);
#else
        CallHookPkt(AudioCtrl->ahiac_MixerFunc, AudioCtrl, glob_buf);
        uint32_t* xbuf = (uint32_t*)glob_buf;
        kprintf((uint8_t*)"%lx %lx %lx %lx\n", xbuf[0], xbuf[1],
                xbuf[2], xbuf[3]);
#endif
        uint32_t bytes = AudioCtrl->ahiac_BuffSamples << 2;

        int overrun = 0;
#ifdef REAL_HARDWARE
        write_reg(ahi_data->hw_addr, ZZ_REG_AUDIO_SCALE,
                  AudioCtrl->ahiac_BuffSamples);

        // def. the faster way
        CopyMem((void*)ahi_data->audio_buf_addr,
                (void*)(ahi_data->audio_hw_buf_addr + period_offset),
                bytes);
        // byteswap, resample and play buffer
        write_reg(ahi_data->hw_addr, ZZ_REG_AUDIO_SWAB,
                  period_offset>>8);
        overrun = read_reg(ahi_data->hw_addr, ZZ_REG_AUDIO_SWAB);
#endif

        if (overrun == 1) {
          ahi_data->buf_offset = 0;
        } else {
          ahi_data->buf_offset =
              period_offset + ZZ_AX_BYTES_PER_PERIOD;
        }

        if (ahi_data->buf_offset >= ZZ_AX_AUDIO_BUFSZ) {
          ahi_data->buf_offset = 0;
        }

        (*AudioCtrl->ahiac_PostTimer)();
      }
    }

    if (!ahi_data->record_stop) process_recording(ahi_data, AudioCtrl);
  }


  /* Lease timer teardown: abort a pending request before freeing the
   * port/request pair (a never-opened device leaves io_Device NULL). */
  if (ahi_data->lease_timer_req) {
    if (ahi_data->lease_timer_req->tr_node.io_Device) {
      AbortIO((struct IORequest *)ahi_data->lease_timer_req);
      WaitIO((struct IORequest *)ahi_data->lease_timer_req);
      CloseDevice((struct IORequest *)ahi_data->lease_timer_req);
    }
    DeleteIORequest((struct IORequest *)ahi_data->lease_timer_req);
    ahi_data->lease_timer_req = NULL;
  }
  if (ahi_data->lease_timer_port) {
    DeleteMsgPort(ahi_data->lease_timer_port);
    ahi_data->lease_timer_port = NULL;
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
  USHORT status = *(USHORT*)(data->hw_addr+ZZ_REG_CONFIG);

  // audio interrupt signal set?
  if (status & 2) {
    // ack/clear audio interrupt
    *(USHORT*)(data->hw_addr+ZZ_REG_CONFIG) = 8|32;

    if(data->disable_cnt) return;
    if(data->play_stop && data->record_stop) return;
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
  irq->is_Node.ln_Name = ZZ_AX_IRQ_NAME_AHI;
  irq->is_Data = ahi_data;
  irq->is_Code = (void*)dev_isr;
}

// Install the interrupt server. MUST be called with Forbid() already active
// so the caller can combine both ownership checks, the AHI owner publication,
// and AddIntServer into one atomic claim step.
static void install_irq_server_locked(struct z9ax* ahi_data) {
  struct Interrupt* irq = &ahi_data->irq;
#ifdef REAL_HARDWARE
  if (ahi_data->flags & ZZ_AX_DEVF_INT2MODE) {
    AddIntServer(INTB_PORTS, irq);
  } else {
    AddIntServer(INTB_EXTER, irq);
  }
#else
  AddIntServer(INTB_VERTB, irq); // for debugging
#endif
  ahi_data->irq_installed = 1;
}

static uint16_t active_hw_interrupts(const struct z9ax* ahi_data) {
  uint16_t mask = 0;

  /* Fabric lease mode never arms the legacy play path: the fabric
   * compositor owns the TX ring, and firmware would strip the bit
   * anyway. The record bit still uses the card interrupt. */
  if (!ahi_data->fabric_mode && !ahi_data->play_stop)
    mask |= ZZ_AX_AUDIO_CONFIG_PLAY;
  if (ahi_data->record_capable && !ahi_data->record_stop)
    mask |= ZZ_AX_AUDIO_CONFIG_RECORD;

  return mask;
}

static void update_hw_interrupts(struct z9ax* ahi_data) {
#ifdef REAL_HARDWARE
  write_reg(ahi_data->hw_addr, ZZ_REG_AUDIO_CONFIG,
            active_hw_interrupts(ahi_data));
#else
  (void)ahi_data;
#endif
}

static void disable_hw_interrupt(struct z9ax* ahi_data) {
#ifdef REAL_HARDWARE
  write_reg(ahi_data->hw_addr, ZZ_REG_AUDIO_CONFIG, 0);
#else
  (void)ahi_data;
#endif
}

static void zero_hw_audio_ring(struct z9ax* ahi_data) {
#ifdef REAL_HARDWARE
  volatile uint8_t *hw_buf = (volatile uint8_t *)ahi_data->audio_hw_buf_addr;
  uint32_t i;
  for (i = 0; i < ZZ_AX_AUDIO_BUFSZ; i++) hw_buf[i] = 0;
#else
  (void)ahi_data;
#endif
}

void destroy_interrupt(struct z9ax* ahi_data) {
  struct Interrupt* irq = &ahi_data->irq;

  if (!ahi_data->irq_installed) return;

#ifdef REAL_HARDWARE
  // disable HW interrupt
  disable_hw_interrupt(ahi_data);
#endif

  Forbid();
#ifdef REAL_HARDWARE
  if (ahi_data->flags & ZZ_AX_DEVF_INT2MODE) {
    RemIntServer(INTB_PORTS, irq);
  } else {
    RemIntServer(INTB_EXTER, irq);
  }
#else
  RemIntServer(INTB_VERTB, irq);
#endif
  ahi_data->irq_installed = 0;
  if (Z9AXBase && Z9AXBase->owner == ahi_data)
    Z9AXBase->owner = NULL;
  Permit();
}

// Check whether MHI has its ISR installed on our shared interrupt level.
// MUST be called with Forbid() already active so that the caller can combine
// the check with AddIntServer() into a single atomic claim step. The
// intuition IntVects[] server list can be mutated by AddIntServer/
// RemIntServer from any task, so walking it unprotected would be unsafe.
static BOOL mhi_present_locked(void) {
  struct List *IrqList;
  if(Z9AXBase->flags & ZZ_AX_DEVF_INT2MODE) {
    IrqList = (struct List *)SysBase->IntVects[INTB_PORTS].iv_Data;
  }
  else {
    IrqList = (struct List *)SysBase->IntVects[INTB_EXTER].iv_Data;
  }
  return FindName(IrqList, (CONST_STRPTR)ZZ_AX_IRQ_NAME_MHI) ? TRUE : FALSE;
}

// Firmware-authoritative control plane (R4/R16): submit this owner's
// neutral source trim through the ZZ9K_OP_AUDIO_TRIM_SUBMIT mailbox
// opcode over zz9k.library. The neutral balance word is the pinned
// keep-baseline release -- "no trim from this owner": the firmware
// answers with the operator baseline pair and does not restage the
// mixer; it owns every master-chain write and the reply's
// applied/bound words are the authority, so the driver never mirrors
// them into DSP registers. Requires ZZ9KBase to be live and must run
// outside Forbid() -- ZZ9KCall blocks on the mailbox completion.
// Failure is non-fatal: playback continues exactly as before, only
// the trim release is lost (e.g. transient firmware error).
static void submit_source_trim(void)
{
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  ZZ9KAudioTrimSubmitPayload *payload;
  int status;

  zz9k_request_init(&request, ZZ9K_OP_AUDIO_TRIM_SUBMIT);
  request.entry.payload_len = sizeof(ZZ9KAudioTrimSubmitPayload);
  payload = (ZZ9KAudioTrimSubmitPayload *)request.entry.payload.inline_data;
  zz9k_put_be32(payload->balance, ZZ9K_AUDIO_BALANCE_NEUTRAL);
  zz9k_put_be32(payload->flags, 0);

  status = ZZ9KCall(&request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
  if (status == ZZ9K_STATUS_OK) {
    // The reply payload is byte storage; copy it out before reading
    // typed fields (mirrors the SDK reply-extraction convention).
    ZZ9KAudioTrimResultPayload result;
    memcpy(&result, reply.payload.inline_data, sizeof(result));
    if (zz9k_get_be32(result.flags) & ZZ9K_AUDIO_TRIM_RESULT_BOUNDED) {
      KPrintF((CONST_STRPTR)"ZZ9000AX: neutral trim bounded by scene policy.\n");
    }
  }
}

/* ---- Audio-fabric lease mode (AHI migration) ----
 *
 * When the card advertises ZZ9K_CAP_AUDIO_FABRIC and the audio
 * service's ZZ9K_SERVICE_FLAG_AUDIO_FABRIC_RATE, playback runs as a
 * direct-ring lease producer: the worker mixes at ahiac_MixFreq into
 * the bounce buffer, stages source-rate periods into the granted
 * board ring, and the firmware compositor converts per-slot with the
 * qualified kernel and mixes the lease alongside the pump (MHI /
 * ZZPlay). The legacy register path (SWAB + TX ring + play bit) stays
 * byte-identical for non-fabric stacks; the Amiga-side AHI/MHI
 * interrupt-server exclusion is only skipped while lease mode runs,
 * where firmware's ownership state remains the authority.
 *
 * The seqlock publisher discipline (single writer, cursor after PCM,
 * heartbeat on every publication) is the SDK client contract; these
 * helpers implement it with the SDK's inline session helpers, so this
 * file also provides the two platform cache hooks those helpers call
 * (same CacheClearE/CACRF_ClearD discipline as the SDK host layer:
 * producer writes must reach the card before the cursor publication;
 * the firmware-owned line is never written here, so clearing its
 * clean lines is an invalidate without a stale writeback). */
void zz9k_audio_ring_cache_flush(const volatile void *address,
                                 uint32_t length)
{
  if (address && length != 0U)
    CacheClearE((APTR)(uintptr_t)address, (ULONG)length, CACRF_ClearD);
}

void zz9k_audio_ring_cache_invalidate(const volatile void *address,
                                      uint32_t length)
{
  if (address && length != 0U)
    CacheClearE((APTR)(uintptr_t)address, (ULONG)length, CACRF_ClearD);
}

/* Probe the fabric plane: global capability plus the audio service's
 * FABRIC_RATE flag (rate-bearing leases; firmware that advertises the
 * fabric without the rate flag only grants 48-kHz bypass leases and
 * is treated as legacy here so AHI's mix-rate table keeps working).
 * Runs after ZZ9KBase is open; blocks on mailbox completions, so it
 * must never run under Forbid(). */
static int fabric_rate_capped(void)
{
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  ZZ9KCaps caps;
  ZZ9KQueryServicePayload *query;
  ZZ9KServiceInfoPayload info;

  if (ZZ9KQueryCaps(&caps) != ZZ9K_STATUS_OK ||
      !(caps.capability_bits & ZZ9K_CAP_AUDIO_FABRIC))
    return 0;

  zz9k_request_init(&request, ZZ9K_OP_QUERY_SERVICE);
  request.entry.payload_len = sizeof(*query);
  query = (ZZ9KQueryServicePayload *)request.entry.payload.inline_data;
  zz9k_put_be32(query->service_id, ZZ9K_SERVICE_AUDIO);
  if (ZZ9KCall(&request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS) !=
      ZZ9K_STATUS_OK)
    return 0;
  memcpy(&info, reply.payload.inline_data, sizeof(info));
  return (zz9k_get_be32(info.flags) &
          ZZ9K_SERVICE_FLAG_AUDIO_FABRIC_RATE) != 0;
}

/* Acquire one lease under the caller's mix rate. Tries the Zorro III
 * direct-ring slots in order and takes the first grant; a refusal
 * (occupied slots, conversion budget, bus policy) is a clean decline
 * and the caller fails the Start honestly instead of silently
 * falling back -- the firmware strips the legacy play bit while the
 * fabric owns the output, so a silent legacy fallback would be a
 * silent no-output. Grant validation mirrors the SDK host layer's
 * board-window bounds check; deep Zorro II region cross-checks stay
 * an SDK-client responsibility, and firmware only ever grants the
 * reserved direct regions. Requires ZZ9KBase and runs outside
 * Forbid(). Returns 1 with session mapped, 0 on any refusal. */
static int fabric_lease_acquire(struct z9ax *ahi_data, uint32_t mix_freq)
{
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  ZZ9KAudioRingAcquireResultPayload result;
  ZZ9KAudioRingSession *session = &ahi_data->lease_session;
  ZZ9KAudioRingAcquirePayload *payload;
  uint32_t slot;
  int status;

  for (slot = 1U; slot <= ZZ9K_AUDIO_RING_SLOT_MAX; slot++) {
    zz9k_request_init(&request, ZZ9K_OP_AUDIO_RING_ACQUIRE);
    request.entry.payload_len = sizeof(*payload);
    payload =
        (ZZ9KAudioRingAcquirePayload *)request.entry.payload.inline_data;
    zz9k_put_be32(payload->slot, slot);
    zz9k_put_be32(payload->identity, ZZ9K_AUDIO_METER_IDENTITY_AHI);
    zz9k_put_be32(payload->gain, 128U);
    zz9k_put_be32(payload->flags,
                  ZZ9K_AUDIO_RING_ACQUIRE_FLAG_SOURCE_RATE);
    zz9k_put_be32(payload->source_rate_hz, mix_freq);

    status = ZZ9KCall(&request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
    if (status != ZZ9K_STATUS_OK) {
      KPrintF((CONST_STRPTR)"ZZ9000AX: lease acquire slot %lu refused: "
              "status %ld.\n", (unsigned long)slot, (long)status);
      continue;
    }
    memcpy(&result, reply.payload.inline_data, sizeof(result));

    memset(session, 0, sizeof(*session));
    session->grant.slot = zz9k_get_be32(result.slot);
    session->grant.generation = zz9k_get_be32(result.generation);
    session->grant.ring_offset = zz9k_get_be32(result.ring_offset);
    session->grant.ring_capacity = zz9k_get_be32(result.ring_capacity);
    session->grant.control_offset =
        zz9k_get_be32(result.control_offset);
    session->grant.period_bytes = zz9k_get_be32(result.period_bytes);
    session->grant.period_us = zz9k_get_be32(result.period_us);
    session->grant.sample_contract =
        zz9k_get_be32(result.sample_contract);
    session->grant.gain_applied = zz9k_get_be32(result.gain_applied);
    session->grant.slot_count = zz9k_get_be32(result.slot_count);
    session->grant.flags = zz9k_get_be32(result.flags);
    session->grant.source_rate = zz9k_get_be32(result.source_rate);

    /* Board-window bounds + contract sanity (R2/R3): both granted
     * ranges must fit the board window, the contract must be the
     * requested source-rate lease, and the echoed rate must be the
     * mix frequency asked for. */
    if (session->grant.sample_contract !=
            ZZ9K_AUDIO_RING_CONTRACT_SOURCE_RATE_STEREO_S16LE ||
        session->grant.source_rate != mix_freq ||
        !zz9k_audio_ring_grant_valid(&session->grant) ||
        session->grant.ring_offset +
                session->grant.ring_capacity >
            Z9AXBase->hw_size ||
        session->grant.control_offset +
                ZZ9K_AUDIO_RING_CONTROL_SIZE >
            Z9AXBase->hw_size) {
      /* Unusable grant: surrender it so the slot does not sit BUSY
       * until heartbeat revocation (the SDK session layer's rule). */
      zz9k_request_init(&request, ZZ9K_OP_AUDIO_RING_RELEASE);
      request.entry.payload_len =
          sizeof(ZZ9KAudioRingReleasePayload);
      {
        ZZ9KAudioRingReleasePayload *rel =
            (ZZ9KAudioRingReleasePayload *)
                request.entry.payload.inline_data;
        zz9k_put_be32(rel->slot, session->grant.slot);
        zz9k_put_be32(rel->generation, session->grant.generation);
        zz9k_put_be32(rel->flags, 0U);
      }
      (void)ZZ9KCall(&request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
      memset(session, 0, sizeof(*session));
      continue;
    }

    session->ring = (volatile uint8_t *)(uintptr_t)
        (Z9AXBase->hw_addr + session->grant.ring_offset);
    session->producer_line =
        (volatile ZZ9KAudioRingProducerLine *)(void *)(uintptr_t)
        (Z9AXBase->hw_addr + session->grant.control_offset);
    session->firmware_line =
        (volatile ZZ9KAudioRingFirmwareLine *)(void *)(uintptr_t)
        (Z9AXBase->hw_addr + session->grant.control_offset +
         ZZ9K_AUDIO_RING_CONTROL_LINE_SIZE);
    session->write_cursor = 0U;
    session->consumed_cursor = 0U;
    session->heartbeat = 1U;
    session->flags = ZZ9K_AUDIO_RING_PRODUCER_FLAG_PAUSED;
    session->mapped = 1U;
    /* First publication: paused, cursor 0, fresh heartbeat. The lease
     * is live from acquisition even before PCM is staged (R11). */
    zz9k_audio_ring_publish(session);
    ahi_data->lease_traced = 0;
    ahi_data->lease_held = 1;
    return 1;
  }
  return 0;
}

/* Idempotent surrender under the grant's own generation (a stale
 * generation release is a no-op by contract). Outside Forbid(). */
static void fabric_lease_release(struct z9ax *ahi_data)
{
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  uint32_t slot;
  uint32_t generation;

  if (!ahi_data->lease_held)
    return;
  slot = ahi_data->lease_session.grant.slot;
  generation = ahi_data->lease_session.grant.generation;
  ahi_data->lease_held = 0;
  memset(&ahi_data->lease_session, 0,
         sizeof(ahi_data->lease_session));

  zz9k_request_init(&request, ZZ9K_OP_AUDIO_RING_RELEASE);
  request.entry.payload_len = sizeof(ZZ9KAudioRingReleasePayload);
  {
    ZZ9KAudioRingReleasePayload *rel =
        (ZZ9KAudioRingReleasePayload *)
            request.entry.payload.inline_data;
    zz9k_put_be32(rel->slot, slot);
    zz9k_put_be32(rel->generation, generation);
    zz9k_put_be32(rel->flags, 0U);
  }
  (void)ZZ9KCall(&request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);

  KPrintF((CONST_STRPTR)"ZZ9000AX: fabric lease released (slot %lu "
          "gen %lu).\n", (unsigned long)slot, (unsigned long)generation);
}

static uint32_t __attribute__((used)) intAHIsub_AllocAudio(struct TagItem *tagList asm("a1"), struct AHIAudioCtrlDrv *AudioCtrl asm("a2")) {
  // TW: Just take the values from where init() has already stored them.
  uint32_t hw_addr = Z9AXBase->hw_addr;
  int zorro = Z9AXBase->zorro_version;
  if(!hw_addr) return AHISF_ERROR; // TW: Only AHISF_xxx return codes are allowed here.
  if(!zorro) return AHISF_ERROR; // TW: Only AHISF_xxx return codes are allowed here.

  // ZZ_REG_AUDIO_CONFIG bit 0 is the "AX present" strap; mask explicitly so
  // other status bits can't ever make this look like detection succeeded.
  uint16_t audio_config = read_reg(hw_addr, ZZ_REG_AUDIO_CONFIG);
  int ax_present = audio_config & 1;
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

  BOOL record_capable = recording_supported(hw_addr);

  // One 20 ms period at the selected mix rate, in sample frames.
  AudioCtrl->ahiac_BuffSamples = AudioCtrl->ahiac_MixFreq/50;
  if (AudioCtrl->ahiac_BuffSamples > BOUNCE_MAX_FRAMES)
    return AHISF_ERROR;

  struct z9ax *ahi_data = AllocVec(sizeof(struct z9ax), MEMF_PUBLIC | MEMF_FAST | MEMF_CLEAR);
  // allocate bounce buffer, as letting AHI write directly to hardware is slower than CopyMem
  void* audio_buf = AllocVec(BOUNCE_BUFSZ, MEMF_PUBLIC | MEMF_FAST | MEMF_CLEAR);
  void* record_buf = record_capable ?
      AllocVec(BOUNCE_BUFSZ, MEMF_PUBLIC | MEMF_FAST | MEMF_CLEAR) : NULL;
  void* lease_accum =
      AllocVec(BOUNCE_BUFSZ, MEMF_PUBLIC | MEMF_FAST | MEMF_CLEAR);

  if (!ahi_data || !audio_buf || !lease_accum ||
      (record_capable && !record_buf)) {
    if (record_buf) FreeVec(record_buf);
    if (lease_accum) FreeVec(lease_accum);
    if (audio_buf) FreeVec(audio_buf);
    if (ahi_data)  FreeVec(ahi_data);
    return AHISF_ERROR; // TW: Only AHISF_xxx return codes are allowed here.
  }

  // TW: Upon allocation playback is initially stopped.
  ahi_data->play_stop = 1;
  ahi_data->record_stop = 1;
  ahi_data->record_capable = record_capable;
  ahi_data->tx_status_capable =
      (audio_config & ZZ_AX_AUDIO_CONFIG_TX_STATUS_CAPABLE) != 0;
  ahi_data->flags = Z9AXBase->flags;
  ahi_data->audio_buf_addr = (uint32_t)audio_buf;
  ahi_data->lease_accum = (uint8_t *)lease_accum;
  ahi_data->lease_accum_fill = 0U;
  ahi_data->record_buf_addr = (uint32_t)record_buf;
  ahi_data->hw_addr = hw_addr;
  ahi_data->audioctrl = AudioCtrl;
  ahi_data->ahi_base = AHIsubBase;
  ahi_data->worker_signal = -1;
  ahi_data->enable_signal = -1;
  ahi_data->mainproc_signal = -1;
  ahi_data->zorro_version = zorro;
  ahi_data->t_mainproc = FindTask(NULL);
  ahi_data->buf_offset = 0;
  /* The audio ring owns the final 64 KiB.  On negotiated Z2 cards, verify
   * that ownership against the same descriptor P96 consumes; mixed old/new
   * pairs retain the exact legacy top-of-window rule. */
  uint32_t offset_tx = Z9AXBase->hw_size - 0x20000;
  if (zorro == 2) {
    struct ZZApertureLayout layout;
    uint16_t fw_caps = read_reg(hw_addr, ZZ_REG_FW_CAPABILITIES);
    uint32_t descriptor = zz9000_read_reg32(
        hw_addr, ZZ_REG_Z2_APERTURE_INFO_HI);
    enum ZZApertureNegotiation status = zz_z2_aperture_negotiate(
        descriptor, Z9AXBase->hw_size, fw_caps, &layout);

    if (status == ZZ_APERTURE_INVALID ||
        (status == ZZ_APERTURE_VALID &&
         layout.audio.size != ZZ_Z2_AUDIO_SIZE)) {
      if (record_buf) FreeVec(record_buf);
      FreeVec(audio_buf);
      FreeVec(ahi_data);
      return AHISF_ERROR;
    }
    if (status == ZZ_APERTURE_VALID)
      offset_tx = zz_aperture_memory_offset(layout.audio.base);
  }
  uint32_t offset_rx = offset_tx + ZZ_AX_RX_BUFFER_DELTA;
  ahi_data->audio_hw_buf_addr = hw_addr + 0x10000 + offset_tx;
  ahi_data->audio_rx_hw_buf_addr = hw_addr + 0x10000 + offset_rx;

  AudioCtrl->ahiac_DriverData = ahi_data;

  // Control-plane client (R4/R16): when zz9k.library is present AND the
  // running firmware advertises ZZ9K_CAP_AUDIO_CONTROL, submit this
  // owner's neutral source trim -- the reserved keep-baseline word,
  // "no trim from this owner": the firmware answers with the operator
  // baseline pair and does not restage the mixer. The reply is
  // authoritative; the driver never mirrors it into DSP registers. The
  // capability query and the trim submission block on the mailbox
  // completion, so both run before the ownership claim, never under
  // Forbid. The same pass decides fabric lease mode: the card must
  // advertise the audio fabric AND the audio service's rate flag, so
  // AHI's mix-rate table keeps working through card-side conversion.
  ZZ9KBase = OpenLibrary((STRPTR)"zz9k.library", 0);
  audio_control_capped = 0;
  ahi_data->fabric_mode = 0;
  if (ZZ9KBase) {
    ZZ9KCaps caps;
    if (ZZ9KQueryCaps(&caps) == ZZ9K_STATUS_OK &&
        (caps.capability_bits & ZZ9K_CAP_AUDIO_CONTROL)) {
      audio_control_capped = 1;
      submit_source_trim();
    }
    ahi_data->fabric_mode =
        (uint8_t)fabric_rate_capped();
  }
  // Atomic ownership claim: reject both another low-level AHI allocation
  // and -- on non-fabric stacks -- MHI, before touching shared hardware.
  // AHI's low-level API is exclusive; full duplex is AHISF_PLAY|AHISF_RECORD
  // on this one AudioCtrl, not two independent AudioCtrls. Publishing owner
  // and installing the ISR under the same Forbid closes both AHI/AHI and
  // AHI/MHI TOCTOU windows. In fabric lease mode the AHI/MHI software
  // exclusion is deliberately skipped: AHI plays through a bounded lease
  // beside MHI's pump, and firmware's ownership state stays the authority
  // (a legacy session still blocks the pump bind fail-closed). The HW-side
  // interrupt stays OFF until the worker is up; Start() publishes the
  // requested direction mask.
  prepare_irq_struct(ahi_data);
  Forbid();
  if (Z9AXBase->owner ||
      (!ahi_data->fabric_mode && mhi_present_locked())) {
    Permit();
    kprintf((CONST_STRPTR)"Can't allocate! Audio hardware already owned.\n");
    /* The control-plane binding opened above the claim must be
     * released exactly like the fail: label -- the neutral trim
     * release plus the library close -- or a rejected allocation
     * leaks an open zz9k.library and a registered owner trim. */
    if (ZZ9KBase) {
      if (audio_control_capped) submit_source_trim();
      CloseLibrary(ZZ9KBase);
      ZZ9KBase = NULL;
    }
    audio_control_capped = 0;
    if (record_buf) FreeVec(record_buf);
    FreeVec(lease_accum);
    FreeVec(audio_buf);
    FreeVec(ahi_data);
    AudioCtrl->ahiac_DriverData = NULL;
    return AHISF_ERROR;
  }
  Z9AXBase->owner = ahi_data;
  install_irq_server_locked(ahi_data);
  // Explicitly silence the FPGA DAC before we touch any audio state.
  // Rationale: destroy_interrupt() writes this same 0 on FreeAudio, so
  // every AllocAudio after the first one starts with the DAC already off
  // and setup runs cleanly. The very first AllocAudio after a cold boot,
  // however, sees whatever power-on default the FPGA left in this
  // register (observed: DAC enabled at power-on on some revisions),
  // which means the DAC has been consuming random contents from
  // audio_hw_buf_addr since boot and keeps doing so throughout setup.
  // Mirroring FreeAudio's disable here makes first-open and reopen take
  // identical paths through setup, eliminating the "garbage burst on
  // first app launch" symptom. We must only do this once we own the
  // card (post install_irq_server_locked) so we can't stomp an
  // in-progress MHI session.
  write_reg(hw_addr, ZZ_REG_AUDIO_CONFIG, 0);
  Permit();

  Forbid();
  /* Fabric lease mode never repoints the legacy TX ring: the firmware
   * compositor owns it, and an AP_TX_BUF_OFFS write would request a
   * formatter re-init under the fabric's feet (the record-start wart
   * from the two-client investigation). Record keeps its RX buffer
   * parameters on both paths. */
  if (!ahi_data->fabric_mode) {
    write_audio_param(hw_addr, 0, offset_tx >> 16);
    write_audio_param(hw_addr, 1, offset_tx & 0xffff);
  }
  /* Record buffer params arm the firmware's deferred audio_init_i2s
   * (a full TX-formatter reset+restart) at EVERY allocation -- on the
   * running fabric that restart storms the formatter's period
   * interrupt (the skip-forward defect) and is the handoff-29 record
   * wart with teeth. In lease mode the RX params move to the first
   * Start(RECORD); a playback-only session never restarts the
   * formatter at all. Legacy stacks keep the qualified behavior. */
  if (record_capable && !ahi_data->fabric_mode) {
    write_audio_param(hw_addr, ZZ_AX_AP_RX_BUF_OFFS_HI, offset_rx >> 16);
    write_audio_param(hw_addr, ZZ_AX_AP_RX_BUF_OFFS_LO, offset_rx & 0xffff);
  }
  Permit();

  // Old firmware (no zz9k.library, or the capability not advertised):
  // no scene module owns the master chain, so keep the legacy
  // anti-alias stamp at allocate -- the low-pass cutoff at half the
  // mix rate, capped just under the filter's rough spot near 24 kHz.
  // On control-plane firmware the LPF is scene-owned and this stamp is
  // skipped; LPF tracking on mixed sets needs matched firmware.
  if (!audio_control_capped) {
    int lpf_freq = AudioCtrl->ahiac_MixFreq / 2;
    if (lpf_freq > 23900) lpf_freq = 23900;
    write_audio_param(hw_addr, ZZ_AX_AP_DSP_SET_LOWPASS, lpf_freq);
  }

  // Zero the hardware audio ring buffer before we enable playback -- the
  // legacy path only: a lease's grant is pre-zeroed by firmware (R5), and
  // the legacy ring is not ours to touch in fabric mode. The FPGA DAC
  // starts consuming from audio_hw_buf_addr as soon as the HW audio
  // interrupt is armed, and whatever garbage was left there by a previous
  // MHI session, a previous AHI session, or power-on junk will be played
  // as a short burst before the worker writes the first mixed period.
  // ZZ_AX_AUDIO_BUFSZ is the full ring size (8 periods); zeroing all of it
  // means the DAC plays silence until real data lands.
  if (!ahi_data->fabric_mode)
    zero_hw_audio_ring(ahi_data);

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

  // Worker is up and the period size is initialized. Start() will reset
  // the ring and enable the requested hardware directions.

  // none of that weird timing
  return AHISF_KNOWSTEREO | AHISF_MIXING |
         (record_capable ? AHISF_CANRECORD : 0); // | AHISF_TIMING;

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
  // Release the control-plane binding opened at allocate. The release
  // is explicit: when the capability was seen, submit the reserved
  // neutral balance word -- the pinned keep-baseline release, "no trim
  // from this owner"; the firmware answers with the operator baseline
  // pair and does not restage the mixer. The submission blocks on the
  // mailbox completion and CloseLibrary can expunge, so both stay
  // outside any Forbid window.
  if (ZZ9KBase) {
    if (audio_control_capped) submit_source_trim();
    CloseLibrary(ZZ9KBase);
    ZZ9KBase = NULL;
  }
  audio_control_capped = 0;
  if (ahi_data->mainproc_signal != -1) {
    FreeSignal(ahi_data->mainproc_signal);
    ahi_data->mainproc_signal = -1;
  }
  if (ahi_data->audio_buf_addr) {
    FreeVec((void*)ahi_data->audio_buf_addr);
    ahi_data->audio_buf_addr = 0;
  }
  if (ahi_data->lease_accum) {
    FreeVec(ahi_data->lease_accum);
    ahi_data->lease_accum = NULL;
  }
  if (ahi_data->record_buf_addr) {
    FreeVec((void*)ahi_data->record_buf_addr);
    ahi_data->record_buf_addr = 0;
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
  ahi_data->record_stop = 1;

  // Stop the worker while our named ISR still advertises ownership to MHI.
  // Both directions are stopped and the hardware interrupt is disabled, so a
  // late shared-level invocation can only acknowledge and return. Removing
  // the ISR before the worker exits would open a hand-off window in which MHI
  // could claim the hardware while this instance was still tearing down.
  disable_hw_interrupt(ahi_data);
  if (ahi_data->worker_process) {
    Signal((struct Task *)ahi_data->worker_process, SIGBREAKF_CTRL_C);
    // Worker clears worker_process and signals mainproc_signal on exit.
    if (ahi_data->mainproc_signal != -1) {
      Wait(1L << ahi_data->mainproc_signal);
    }
    ahi_data->worker_process = NULL;
  }

  // Surrender the fabric lease while zz9k.library is still open: the
  // worker (the sole producer-line writer) is gone, so the lease must
  // not linger on heartbeat alone. Blocking mailbox call, outside any
  // Forbid window; idempotent and stale-generation safe.
  if (ahi_data->fabric_mode)
    fabric_lease_release(ahi_data);

  // Release the control-plane binding opened at allocate. The release
  // is explicit: when the capability was seen, submit the reserved
  // neutral balance word -- the pinned keep-baseline release, "no trim
  // from this owner"; the firmware answers with the operator baseline
  // pair and does not restage the mixer. The submission blocks on the
  // mailbox completion and CloseLibrary can expunge, so both stay
  // outside any Forbid window.
  if (ZZ9KBase) {
    if (audio_control_capped) submit_source_trim();
    CloseLibrary(ZZ9KBase);
    ZZ9KBase = NULL;
  }
  audio_control_capped = 0;
  destroy_interrupt(ahi_data);

  if (ahi_data->mainproc_signal != -1) {
    FreeSignal(ahi_data->mainproc_signal);
    ahi_data->mainproc_signal = -1;
  }

  if (ahi_data->audio_buf_addr) {
    FreeVec((void*)ahi_data->audio_buf_addr);
    ahi_data->audio_buf_addr = 0;
  }
  if (ahi_data->lease_accum) {
    FreeVec(ahi_data->lease_accum);
    ahi_data->lease_accum = NULL;
  }
  if (ahi_data->record_buf_addr) {
    FreeVec((void*)ahi_data->record_buf_addr);
    ahi_data->record_buf_addr = 0;
  }

  FreeVec(AudioCtrl->ahiac_DriverData);
  AudioCtrl->ahiac_DriverData = NULL;
}

// TW: Prepared Stop() and Start() to store status in a flag in z9ax.
static void __attribute__((used)) intAHIsub_Stop(uint32_t Flags asm("d0"), struct AHIAudioCtrlDrv *AudioCtrl asm("a2")) {
  struct z9ax *ahi_data = AudioCtrl->ahiac_DriverData;
  BOOL stop_play;

  if (!ahi_data) return;

  stop_play = (Flags & AHISF_PLAY) != 0;
  if (Flags & (AHISF_PLAY | AHISF_RECORD)) {
    Forbid();
    if (Flags & AHISF_PLAY) {
      ahi_data->play_stop = 1;
      ahi_data->buf_offset = 0;
    }
    if (Flags & AHISF_RECORD) ahi_data->record_stop = 1;
    update_hw_interrupts(ahi_data);
    Permit();
  }

  if (stop_play) {
    if (ahi_data->fabric_mode) {
      /* Lease mode: the worker publishes the PAUSED producer flag on
       * its next pacing pass (it is the sole producer-line writer);
       * cursor progress is suppressed without an underrun and the
       * heartbeat stays live (R12). The lease is released at
       * FreeAudio, not here, so Start() resumes without a new
       * grant. */
      ahi_data->lease_session.flags |=
          ZZ9K_AUDIO_RING_PRODUCER_FLAG_PAUSED;
    } else {
      // Clear the 30 KB Zorro-side ring outside Forbid(); AHI
      // serializes Start/Stop calls for this driver instance, and
      // playback is off.
      zero_hw_audio_ring(ahi_data);
    }
  }
}

static uint32_t __attribute__((used)) intAHIsub_Start(uint32_t flags asm("d0"), struct AHIAudioCtrlDrv *AudioCtrl asm("a2")) {
  struct z9ax *ahi_data = AudioCtrl->ahiac_DriverData;
  uint16_t play_sequence = 0;
  uint32_t result = AHIE_OK;
  if (!ahi_data) return AHIE_OK;

  if ((flags & AHISF_PLAY) && ahi_data->fabric_mode) {
    /* Lease mode: acquire at the first play Start (or after a
     * mid-session revocation zeroed lease_held), clear PAUSED, and
     * let the credit-paced worker take it from here. A refused
     * acquire fails the Start honestly: the fabric owns the output,
     * so a silent legacy fallback would be silent no-output. */
    if (!ahi_data->lease_held) {
      if (!fabric_lease_acquire(ahi_data,
                                AudioCtrl->ahiac_MixFreq)) {
        KPrintF((CONST_STRPTR)"ZZ9000AX: fabric lease refused at "
                "%lu Hz; Start(PLAY) fails.\n",
                (unsigned long)AudioCtrl->ahiac_MixFreq);
        result = AHIE_UNKNOWN;
      }
    }
    if (result == AHIE_OK) {
      Forbid();
      ahi_data->buf_offset = 0;
      /* PAUSED stays as acquired: the worker clears it once two
       * periods are staged (primed-ring prefill). Stop(PLAY) sets
       * it again, so a resume re-primes the same way. */
      ahi_data->play_stop = 0;
      update_hw_interrupts(ahi_data);
      Permit();
    }
  }
  else if (flags & AHISF_PLAY) {
    Forbid();
    ahi_data->buf_offset = 0;
    ahi_data->play_stop = 1;
    update_hw_interrupts(ahi_data);
    Permit();

    // Clear the 30 KB Zorro-side ring outside Forbid(); play_stop remains
    // set so the worker drops any stale signal instead of racing this
    // silence pass.
    zero_hw_audio_ring(ahi_data);

    if (ahi_data->record_capable)
    {
      uint16_t status = read_reg(ahi_data->hw_addr,
                                 ZZ_REG_AUDIO_TX_STATUS);
      play_sequence =
          (ahi_data->tx_status_capable &&
           (status & ZZ_AX_AUDIO_TX_STATUS_CAPABLE)) ?
              zz_ax_audio_tx_status_sequence(status) : status;
    }

    Forbid();
    ahi_data->buf_offset = 0;
    ahi_data->play_sequence = play_sequence;
    ahi_data->play_stop = 0;
    update_hw_interrupts(ahi_data);
    Permit();
  }

  if ((flags & AHISF_RECORD) && ahi_data->record_capable) {
    uint16_t status;

    if (ahi_data->fabric_mode) {
      /* Deferred from AllocAudio (see there): the RX buffer params
       * land only when recording is actually requested. They arm the
       * firmware's deferred formatter reinit -- an accepted glitch
       * when starting capture, never a playback-session side
       * effect. */
      uint32_t offset_rx = ahi_data->audio_rx_hw_buf_addr -
                           (ahi_data->hw_addr + 0x10000);
      write_audio_param(ahi_data->hw_addr, ZZ_AX_AP_RX_BUF_OFFS_HI,
                        offset_rx >> 16);
      write_audio_param(ahi_data->hw_addr, ZZ_AX_AP_RX_BUF_OFFS_LO,
                        offset_rx & 0xffff);
    }
    write_reg(ahi_data->hw_addr, ZZ_REG_AUDIO_SCALE,
              AudioCtrl->ahiac_BuffSamples);
    status = read_reg(ahi_data->hw_addr, ZZ_REG_AUDIO_RX_STATUS);

    Forbid();
    ahi_data->record_sequence = zz_ax_audio_rx_status_sequence(status);
    ahi_data->record_stop = 0;
    update_hw_interrupts(ahi_data);
    Permit();
  }

  // AHIE_OK when every requested direction started; a refused lease
  // reports AHIE_UNKNOWN for playback.
  return result;
}

static int32_t __attribute__((used)) intAHIsub_GetAttr(uint32_t attr_ asm("d0"), int32_t arg_ asm("d1"), int32_t def_ asm("d2"), struct TagItem *tagList asm("a1"), struct AHIAudioCtrlDrv *AudioCtrl asm("a2")) {
  uint32_t attr = attr_;
  int32_t arg = arg_, def = def_;
  BOOL can_record = Z9AXBase && recording_supported(Z9AXBase->hw_addr);

  // TW: We can't rely on AudioCtrl->ahiac_DriverData being valid during this function call!

  switch(attr)
    {
    case AHIDB_Bits:
      return 16;
    case AHIDB_Frequencies:
      return ZZ_NUM_FREQS;
    case AHIDB_Frequency:
      if (arg < 0 || arg >= ZZ_NUM_FREQS)
        return def;
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
      return can_record;
    case AHIDB_FullDuplex:
      return can_record;
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
      return can_record ? BOUNCE_MAX_FRAMES : 0;
    case AHIDB_MinMonitorVolume:
      return 0x0;
    case AHIDB_MaxMonitorVolume:
      return 0x0;
    case AHIDB_MinInputGain:
      return can_record ? 0x10000 : 0;
    case AHIDB_MaxInputGain:
      return can_record ? 0x10000 : 0;
    case AHIDB_MinOutputVolume:
      return 0x0;
    case AHIDB_MaxOutputVolume:
      return 0x0;
    case AHIDB_Inputs:
      return can_record ? 1 : 0;
    case AHIDB_Input:
      return (can_record && arg == 0) ? (int32_t) "RCA In" : def;
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
  struct z9ax *ahi_data = AudioCtrl->ahiac_DriverData;

  if (!ahi_data) return FALSE;

  switch (attr) {
    case AHIC_MixFreq_Query:
      return AudioCtrl->ahiac_MixFreq;
    case AHIC_InputGain:
      ahi_data->input_gain = 0x10000;
      return ahi_data->record_capable ? TRUE : FALSE;
    case AHIC_InputGain_Query:
      return ahi_data->record_capable ? 0x10000 : 0;
    case AHIC_Input:
      if (!ahi_data->record_capable || arg != 0) return FALSE;
      return TRUE;
    case AHIC_Input_Query:
      return 0;
    case AHIC_MonitorVolume_Query:
    case AHIC_OutputVolume_Query:
    case AHIC_Output_Query:
      return 0;
    default:
      return FALSE;
  }
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
