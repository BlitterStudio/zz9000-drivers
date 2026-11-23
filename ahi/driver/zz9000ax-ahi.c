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
#include <proto/timer.h>

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
#define DEVICE_DATE "(22.11.2026)"
#define DEVICE_VERSION 4
#define DEVICE_REVISION 33
#define DEVICE_ID_STRING "ZZ9000AX " XSTR(DEVICE_VERSION) "." XSTR(DEVICE_REVISION) " " DEVICE_DATE
#define DEVICE_PRIORITY 0

#define REAL_HARDWARE 1

// ZZ_AX_AUDIO_BUFSZ is the hardware-side ring size (8 periods). Each
// MixerFunc call writes at most one transport period into the bounce buffer.
#define BOUNCE_BUFSZ ZZ_AX_BOUNCE_BUFSZ
// AHI sample frames = 16-bit stereo => 4 bytes per frame. This is what
// ahiac_BuffSamples counts and what AHIDB_MaxPlaySamples must advertise;
// keep this in sync with the mixer byte math (bytes = BuffSamples << 2)
// and with the bounce-buffer capacity check in WorkerProcess().
#define BOUNCE_MAX_FRAMES ZZ_AX_BOUNCE_MAX_FRAMES
// AHI software-mixing channel ceiling advertised via AHIDB_MaxChannels.
// This is an AHISF_MIXING subdriver: ahi.device mixes every allocated
// channel on the m68k (linear interpolation, 32-bit accumulation) and the
// card only ever receives the final stereo S16 stream, so this number is
// a host CPU budget, not a hardware limit -- the ZZ9000AX transport is
// channel-count-invariant. Cost accrues per playing channel at the
// selected mix rate (silent channels take AHI's cheap AddSilence path),
// and users pick the actual count per mode in AHI Prefs. 8 keeps
// 44.1 kHz mixing deliverable on 68040-class hosts and matches the
// comparable m68k mixing driver baseline (WinUAE's AHI driver: 8);
// recommend 2-4 on 68030. Bumping the ceiling is this define alone.
#define ZZ_AX_AHI_MAX_CHANNELS 8
// Lease-mode startup headroom: two periods = 40 ms. Absolute deadline
// pacing keeps production locked to the compositor after playback begins.
#define LEASE_RUNWAY_PERIODS 2U
// A revoked active lease is retried by the worker at most once per
// elapsed second; Stop(PLAY) cancels recovery by setting play_stop.
#define LEASE_RETRY_SECONDS 1

static int fabric_lease_acquire(struct z9ax *ahi_data, uint32_t mix_freq);
static void fabric_lease_release(struct z9ax *ahi_data);

// AmigaOS scheduling is strictly preemptive priority with no aging; any
// task at or above this priority stuck in a CPU loop will starve the mixer
// and produce audible stutter. Keep the mixer well above user-level tasks
// and slightly below critical system servers (timer.device = 127).
#define WORKER_PRIORITY 110

// Every library base this startup-less driver uses is defined here. A base
// left to libstubs/libnix would pull an auto-open entry that only libnix
// startup code opens (tools/check-link-map.sh rejects that).
struct ExecBase     *SysBase;
struct Library      *UtilityBase = NULL;
struct ExpansionBase *ExpansionBase = NULL;
struct IntuitionBase *IntuitionBase = NULL;
struct Library      *AHIsubBase  = NULL;
struct DosLibrary   *DOSBase     = NULL;
struct Device       *TimerBase    = NULL;
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
  /* The selector/value/reset triplet is shared with MHI. Forbid pairs
   * with MHI's setAudioParam guard so coexistence cannot interleave
   * task-context transactions and redirect either value write. */
  Forbid();
  *((volatile uint16_t*)(base+ZZ_REG_AUDIO_PARAM)) = param;
  *((volatile uint16_t*)(base+ZZ_REG_AUDIO_VAL))   = val;
  *((volatile uint16_t*)(base+ZZ_REG_AUDIO_PARAM)) = 0;
  Permit();
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
  Z9AXBase->allocating = NULL;

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

  uint32_t record_frames = AudioCtrl->ahiac_MixFreq / 50U;
  if (record_frames > BOUNCE_MAX_FRAMES) {
    ahi_data->record_sequence = sequence;
    return;
  }

  newest_period = zz_ax_audio_rx_status_period(status);
  bytes = record_frames << 2;

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
    ahi_data->record_message.ahirm_Length = record_frames;

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

/* AHI's mixer writes m68k-native big-endian S16. New firmware accepts
 * that order on the lease (flag bit 1, contract 4) and swaps before
 * the FIR. Older firmware rejects the flag; the driver then swaps. */
#define ZZ_AX_ACQUIRE_FLAG_SOURCE_S16BE (1U << 1)
#define ZZ_AX_CONTRACT_SOURCE_RATE_STEREO_S16BE 4U
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

static void fabric_lease_flush_accum(struct z9ax *ahi_data,
                                     ZZ9KAudioRingSession *session,
                                     uint32_t lease_period)
{
  while (ahi_data->lease_accum_fill >= lease_period &&
         zz9k_audio_ring_free_bytes(session) >= lease_period) {
    if (zz9k_audio_ring_write(session, ahi_data->lease_accum,
                              lease_period) != lease_period)
      break;
    ahi_data->lease_accum_fill -= lease_period;
    memmove(ahi_data->lease_accum,
            ahi_data->lease_accum + lease_period,
            ahi_data->lease_accum_fill);
  }
}

/* Start one player interval, reading the live 16.16 Hz after the hook so a
 * tempo change made there applies to the samples it just configured.
 * Division remainder is carried exactly at an unchanged frequency. On a
 * frequency change, rescale that fraction rather than resetting phase.
 * Only integer/libgcc arithmetic is used by this startup-less device. */
static void software_player_tick(struct z9ax *ahi_data,
                                  struct AHIAudioCtrlDrv *AudioCtrl)
{
  uint32_t numerator = AudioCtrl->ahiac_MixFreq << 16;
  uint32_t frequency;
  uint64_t frames;

  CallHookPkt(AudioCtrl->ahiac_PlayerFunc, AudioCtrl, NULL);
  frequency = (uint32_t)AudioCtrl->ahiac_PlayerFreq;
  if (frequency == 0U) frequency = 50U << 16;
  /* One source frame is the smallest representable interval. Normal AHI
   * clients promise MinPlayerFreq/MaxPlayerFreq within this range. */
  if (frequency > numerator) frequency = numerator;
  if (ahi_data->player_freq && frequency != ahi_data->player_freq)
    ahi_data->player_remainder = (uint32_t)(
        (uint64_t)ahi_data->player_remainder * frequency /
        ahi_data->player_freq);
  ahi_data->player_freq = frequency;
  frames = (uint64_t)numerator + ahi_data->player_remainder;
  ahi_data->player_frames_left = (uint32_t)(frames / frequency);
  ahi_data->player_remainder = (uint32_t)(frames % frequency);
}

/* Render one fixed transport period, splitting at player boundaries. Slow
 * ticks span several wakes; fast ticks subdivide a wake. Always mix into the
 * start of the full bounce buffer: ahi.device clears its allocation-sized
 * output even when BuffSamples is smaller. The caller owns ONE timer pair
 * for this wake, not one per back-to-back chunk (which falsely overloads
 * AHI's CPU limiter). A skipped wake still advances every player tick. */
static void software_mix_period(struct z9ax *ahi_data,
                                 struct AHIAudioCtrlDrv *AudioCtrl,
                                 uint8_t *destination, BOOL mix, BOOL swap)
{
  uint32_t period_frames = AudioCtrl->ahiac_MixFreq / 50U;
  uint32_t remaining = period_frames;
  uint32_t offset = 0U;
  void *bounce = (void *)(uintptr_t)ahi_data->audio_buf_addr;

  while (remaining) {
    uint32_t chunk;
    if (!ahi_data->player_frames_left)
      software_player_tick(ahi_data, AudioCtrl);
    chunk = ahi_data->player_frames_left;
    if (chunk > remaining) chunk = remaining;
    AudioCtrl->ahiac_BuffSamples = chunk;
    if (mix) {
      CallHookPkt(AudioCtrl->ahiac_MixerFunc, AudioCtrl, bounce);
      if (destination) {
        if (swap) fabric_swap_period_le(bounce, chunk << 2);
        if (destination + offset != bounce)
          CopyMem(bounce, destination + offset, chunk << 2);
      }
    }
    ahi_data->player_frames_left -= chunk;
    remaining -= chunk;
    offset += chunk << 2;
  }
  AudioCtrl->ahiac_BuffSamples = period_frames;
}



/* One lease-pacing pass (worker context, timer wake): recover an active
 * revoked lease with a rate-bounded mailbox acquire, adopt firmware
 * credits, stage whole source-rate transport periods, with player ticks
 * and mixer chunks scheduled independently inside each period, then
 * publish the producer line: cursor after PCM, heartbeat on every
 * publication (R6/R11). Disable suppresses AHI callbacks but still
 * publishes the heartbeat. Stop(PLAY) prevents further production and
 * surrenders the lease, discarding both staged and ring-resident PCM. */
static void fabric_lease_pump(struct z9ax *ahi_data,
                              struct AHIAudioCtrlDrv *AudioCtrl)
{
  ZZ9KAudioRingSession *session = &ahi_data->lease_session;
  uint32_t recovery_generation;
  int acquired;
  int stale;
  struct timeval now;

  if (ahi_data->lease_retry_deadline.tv_secs != 0 ||
      ahi_data->lease_retry_deadline.tv_micro != 0) {
    GetSysTime(&now);
    if (now.tv_secs < ahi_data->lease_retry_deadline.tv_secs ||
        (now.tv_secs == ahi_data->lease_retry_deadline.tv_secs &&
         now.tv_micro < ahi_data->lease_retry_deadline.tv_micro))
      return;
    ahi_data->lease_retry_deadline.tv_secs = 0;
    ahi_data->lease_retry_deadline.tv_micro = 0;
  }
  ahi_data->lease_acquire_uncertain = 0U;
  if (ahi_data->lease_release_pending) {
    fabric_lease_release(ahi_data);
    return;
  }
  if (!ahi_data->lease_held || !session->mapped) {
    if (ahi_data->play_stop || ahi_data->lease_held)
      return;
    Forbid();
    if (ahi_data->lease_acquire_in_progress) {
      Permit();
      return;
    }
    ahi_data->lease_acquire_in_progress = 1U;
    recovery_generation = ahi_data->play_transport_generation;
    Permit();
    acquired = fabric_lease_acquire(ahi_data,
                                    AudioCtrl->ahiac_MixFreq);
    Forbid();
    stale = ahi_data->play_transport_generation !=
                recovery_generation ||
            ahi_data->play_stop;
    Permit();
    if (!acquired) {
      Forbid();
      ahi_data->lease_acquire_in_progress = 0U;
      Permit();
      GetSysTime(&ahi_data->lease_retry_deadline);
      ahi_data->lease_retry_deadline.tv_secs += LEASE_RETRY_SECONDS;
      return;
    }
    /* Stop, or a Stop/Start pair, may complete while the mailbox call
     * blocks. Keep acquisition serialized until any stale grant is
     * surrendered; only the epoch that began recovery may retain it. */
    if (stale) {
      fabric_lease_release(ahi_data);
      Forbid();
      ahi_data->lease_acquire_in_progress = 0U;
      Permit();
      return;
    }
    Forbid();
    ahi_data->lease_acquire_in_progress = 0U;
    Permit();
    ahi_data->lease_accum_fill = 0U;
    KPrintF((CONST_STRPTR)"ZZ9000AX: fabric lease recovered at %lu Hz.\n",
            (unsigned long)AudioCtrl->ahiac_MixFreq);
  }
  if (zz9k_audio_ring_take_credits(session, 4U) ==
      ZZ9K_AUDIO_RING_CREDIT_REVOKED) {
    KPrintF((CONST_STRPTR)"ZZ9000AX: fabric lease REVOKED; recovery "
            "scheduled (heartbeat/cursor/generation).\n");
    ahi_data->lease_held = 0;
    ahi_data->lease_source_be = 0;
    ahi_data->lease_retry_deadline.tv_secs = 0;
    ahi_data->lease_retry_deadline.tv_micro = 0;
    ahi_data->lease_accum_fill = 0U;
    memset(session, 0, sizeof(*session));
    return;
  }
  if (AudioCtrl->ahiac_BuffSamples > BOUNCE_MAX_FRAMES)
    return;  /* the legacy defence-in-depth bound still applies */

  /* PreTimer gates one transport wake. Every PreTimer is paired with
   * PostTimer even when it skips mixing. The output still stages whole
   * grant periods under backpressure and stays PAUSED until the configured
   * runway is staged (inaudible prefill). */
  {
    uint32_t lease_period =
        (session->grant.source_rate / 50U) * 4U;

    if (lease_period != 0U && lease_period <= BOUNCE_BUFSZ &&
        ahi_data->lease_accum != NULL && !ahi_data->play_stop &&
        ahi_data->disable_cnt == 0U &&
        AudioCtrl->ahiac_PreTimer && AudioCtrl->ahiac_MixerFunc) {
      /* Drain credited whole periods before mixing so newly available
       * ring space prevents an avoidable drop. */
      fabric_lease_flush_accum(ahi_data, session, lease_period);
      if (!ahi_data->player_frames_left)
        software_player_tick(ahi_data, AudioCtrl);
      {
        BOOL mix = !(*AudioCtrl->ahiac_PreTimer)();
        uint32_t mix_bytes = (AudioCtrl->ahiac_MixFreq / 50U) << 2;
        uint32_t room = BOUNCE_BUFSZ - ahi_data->lease_accum_fill;
        BOOL stage = mix_bytes == lease_period && mix_bytes <= room;
        /* Backpressure discards the complete new transport period, never
         * a truncated prefix. AHI still mixes to advance channel state. */
        software_mix_period(ahi_data, AudioCtrl,
            stage ? ahi_data->lease_accum + ahi_data->lease_accum_fill : NULL,
            mix, !ahi_data->lease_source_be);
        if (mix && stage) ahi_data->lease_accum_fill += mix_bytes;
      }
      (*AudioCtrl->ahiac_PostTimer)();
      fabric_lease_flush_accum(ahi_data, session, lease_period);
      if ((session->flags & ZZ9K_AUDIO_RING_PRODUCER_FLAG_PAUSED) &&
          session->write_cursor - session->consumed_cursor >=
              (uint64_t)LEASE_RUNWAY_PERIODS * lease_period) {
        session->flags &= ~ZZ9K_AUDIO_RING_PRODUCER_FLAG_PAUSED;
      }
    }
  }
  zz9k_audio_ring_publish(session);
}

/* Keep a mix clock anchored to an absolute deadline. Re-arming a
 * relative request after each delivery accumulates dispatch latency;
 * production then falls below the consumer's fixed rate. A late deadline
 * becomes an immediate request, preserving every elapsed period without
 * running the steady-state clock fast. The lease worker paces 20 ms periods;
 * the voice worker polls at a few milliseconds. */
static void timer_post_micro(struct z9ax *ahi_data, uint32_t micro)
{
  struct timeval now;
  struct timeval delay;

  ahi_data->lease_timer_deadline.tv_micro += micro;
  if (ahi_data->lease_timer_deadline.tv_micro >= 1000000) {
    ahi_data->lease_timer_deadline.tv_secs++;
    ahi_data->lease_timer_deadline.tv_micro -= 1000000;
  }
  GetSysTime(&now);
  if (ahi_data->lease_timer_deadline.tv_secs > now.tv_secs ||
      (ahi_data->lease_timer_deadline.tv_secs == now.tv_secs &&
       ahi_data->lease_timer_deadline.tv_micro > now.tv_micro)) {
    delay.tv_secs =
        ahi_data->lease_timer_deadline.tv_secs - now.tv_secs;
    if (ahi_data->lease_timer_deadline.tv_micro < now.tv_micro) {
      delay.tv_secs--;
      delay.tv_micro = 1000000 +
          ahi_data->lease_timer_deadline.tv_micro - now.tv_micro;
    } else {
      delay.tv_micro =
          ahi_data->lease_timer_deadline.tv_micro - now.tv_micro;
    }
  } else {
    delay.tv_secs = 0;
    delay.tv_micro = 0;
  }
  ahi_data->lease_timer_req->tr_node.io_Command = TR_ADDREQUEST;
  ahi_data->lease_timer_req->tr_time = delay;
  SendIO((struct IORequest *)ahi_data->lease_timer_req);
}

static void fabric_timer_post(struct z9ax *ahi_data)
{
  timer_post_micro(ahi_data, 20000U);
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

  /* Fabric lease mode paces itself: a UNIT_MICROHZ timer wakes the
   * worker to adopt credits, stage source-rate periods, and refresh
   * the lease heartbeat; the card interrupt keeps serving the record
   * direction exactly as before. The pacing timer is part of worker
   * initialization -- without it nothing ever calls
   * fabric_lease_pump, so a session that reported success would
   * produce no audio and lose its lease heartbeat. Bring the port,
   * request, and device up (and post the first request) before the
   * success handshake below; any failure aborts init so AllocAudio
   * takes its existing failure path instead of starting a silent
   * session. */
  uint32_t lease_timer_sig = 0;
  if (ahi_data->fabric_mode) {
    ahi_data->lease_timer_port = CreateMsgPort();
    if (ahi_data->lease_timer_port) {
      ahi_data->lease_timer_req = (struct timerequest *)CreateIORequest(
          (APTR)ahi_data->lease_timer_port, sizeof(struct timerequest));
    }
    if (ahi_data->lease_timer_req &&
        OpenDevice((STRPTR)"timer.device", UNIT_MICROHZ,
                   (struct IORequest *)ahi_data->lease_timer_req, 0) == 0) {
      lease_timer_sig = 1L << ahi_data->lease_timer_port->mp_SigBit;
      TimerBase = ahi_data->lease_timer_req->tr_node.io_Device;
      GetSysTime(&ahi_data->lease_timer_deadline);
      fabric_timer_post(ahi_data);
    } else {
      /* Clean up whatever the failed setup left behind: nothing may
       * outlive this worker, and AllocAudio's failure path owns the
       * rest of the teardown. */
      if (ahi_data->lease_timer_req) {
        DeleteIORequest((struct IORequest *)ahi_data->lease_timer_req);
        ahi_data->lease_timer_req = NULL;
      }
      if (ahi_data->lease_timer_port) {
        DeleteMsgPort(ahi_data->lease_timer_port);
        ahi_data->lease_timer_port = NULL;
      }
    }
  }

  // Fabric mode without a live pacing timer is fatal: bail out before
  // the success handshake so AllocAudio fails instead of starting a
  // session that could never produce audio.
  if (ahi_data->fabric_mode && !lease_timer_sig) {
    FreeSignal(ahi_data->enable_signal); ahi_data->enable_signal = -1;
    FreeSignal(ahi_data->worker_signal); ahi_data->worker_signal = -1;
#ifndef REAL_HARDWARE
    if (glob_buf) FreeVec(glob_buf);
#endif
    // Clear worker_process so AllocAudio can detect the failure after the handshake.
    ahi_data->worker_process = NULL;
    Signal((struct Task *)ahi_data->t_mainproc, 1L << ahi_data->mainproc_signal);
    return;
  }

  Signal(ahi_data->t_mainproc, 1L << ahi_data->mainproc_signal);

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
      if (!ahi_data->record_stop && ahi_data->disable_cnt == 0U)
        process_recording(ahi_data, AudioCtrl);
      continue;
    }

    // A pending enable_signal may have been latched by the ISR between
    // Stop()/teardown updating the direction flags and this wake-up.
    if (ahi_data->play_stop && ahi_data->record_stop) continue;


    uint32_t period_offset = ahi_data->buf_offset;

    if (playback_period_ready(ahi_data, &period_offset)) {
      if (!ahi_data->player_frames_left)
        software_player_tick(ahi_data, AudioCtrl);

      BOOL mix = !(*AudioCtrl->ahiac_PreTimer)();
      {
        // Defence in depth: no MixerFunc call may exceed our bounce buffer.
        if (AudioCtrl->ahiac_BuffSamples > BOUNCE_MAX_FRAMES) {
          kprintf((CONST_STRPTR)"ZZ9000AX: BuffSamples %ld exceeds bounce cap %ld; skipping\n",
                  (long)AudioCtrl->ahiac_BuffSamples,
                  (long)BOUNCE_MAX_FRAMES);
          (*AudioCtrl->ahiac_PostTimer)();
          if (!ahi_data->record_stop)
            process_recording(ahi_data, AudioCtrl);
          continue;
        }
        uint32_t period_frames = AudioCtrl->ahiac_MixFreq / 50U;
        // A whole-tick period (including PlayerFreq 50) needs no assembly
        // copy. Split periods reuse the otherwise idle lease accumulator.
        uint8_t *pcm = ahi_data->player_frames_left >= period_frames ?
            (uint8_t *)(uintptr_t)ahi_data->audio_buf_addr :
            ahi_data->lease_accum;
        software_mix_period(ahi_data, AudioCtrl, pcm, mix, FALSE);
        uint32_t bytes = period_frames << 2;

        if (mix) {
          int overrun = 0;
#ifdef REAL_HARDWARE
          // SCALE describes the source rate for a fixed 20 ms hardware
          // period, never the length of an individual player tick.
          write_reg(ahi_data->hw_addr, ZZ_REG_AUDIO_SCALE, period_frames);
          CopyMem(pcm,
                  (void*)(ahi_data->audio_hw_buf_addr + period_offset),
                  bytes);
          // byteswap, resample and play the complete transport period
          write_reg(ahi_data->hw_addr, ZZ_REG_AUDIO_SWAB,
                    period_offset >> 8);
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
        }
      }
      /* AHI v4 requires PostTimer even when PreTimer skips the mix. */
      (*AudioCtrl->ahiac_PostTimer)();
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
      TimerBase = NULL;
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

// TW: dev_isr is the hardware wrapper; dev_token_isr is an inert
// ownership sentinel for mixed-version driver installations.
extern uint32_t dev_isr(struct z9ax* data asm("a1"));
extern uint32_t dev_token_isr(void);

// Fill in the Interrupt server node so it's ready to be added to the
// int-server list. Kept separate from the actual AddIntServer call so the
// install can be performed atomically under the AllocAudio ownership Forbid().
static void prepare_irq_struct(struct z9ax* ahi_data) {
  struct Interrupt* irq = &ahi_data->irq;
  struct Interrupt* fabric = &ahi_data->irq_fabric_token;

  /* Keep the real node legacy-visible so pre-fabric MHI binaries
   * continue to reject AHI. New MHI checks the qualified sentinel
   * first and may coexist only when it is also fabric-qualified. */
  irq->is_Node.ln_Type = NT_INTERRUPT;
  irq->is_Node.ln_Pri = 126; // High priority: this ISR must react quickly.
  irq->is_Node.ln_Name = ZZ_AX_IRQ_NAME_AHI;
  irq->is_Data = ahi_data;
  irq->is_Code = (void*)dev_isr;

  fabric->is_Node.ln_Type = NT_INTERRUPT;
  fabric->is_Node.ln_Pri = 125;
  fabric->is_Node.ln_Name = ZZ_AX_IRQ_NAME_AHI_FABRIC;
  fabric->is_Data = ahi_data;
  fabric->is_Code = (void*)dev_token_isr;
}

// Install the interrupt server. MUST be called with Forbid() already active
// so the caller can combine both ownership checks, the AHI owner publication,
// and AddIntServer into one atomic claim step.
static void install_irq_server_locked(struct z9ax* ahi_data) {
  struct Interrupt* irq = &ahi_data->irq;
  struct Interrupt* fabric = &ahi_data->irq_fabric_token;
#ifdef REAL_HARDWARE
  if (ahi_data->flags & ZZ_AX_DEVF_INT2MODE) {
    AddIntServer(INTB_PORTS, irq);
    if (ahi_data->fabric_mode || ahi_data->voice)
      AddIntServer(INTB_PORTS, fabric);
  } else {
    AddIntServer(INTB_EXTER, irq);
    if (ahi_data->fabric_mode || ahi_data->voice)
      AddIntServer(INTB_EXTER, fabric);
  }
#else
  AddIntServer(INTB_VERTB, irq); // for debugging
  if (ahi_data->fabric_mode || ahi_data->voice)
    AddIntServer(INTB_VERTB, fabric);
#endif
  ahi_data->irq_installed = 1;
  /* The fabric token marks an owner that MHI may run beside: a lease, or a
   * voice session, which the fabric mixes the same way. */
  ahi_data->fabric_token_installed =
      (uint8_t)((ahi_data->fabric_mode || ahi_data->voice) ? 1 : 0);
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
  struct Interrupt* fabric = &ahi_data->irq_fabric_token;

  if (!ahi_data->irq_installed) return;

#ifdef REAL_HARDWARE
  // disable HW interrupt
  disable_hw_interrupt(ahi_data);
#endif

  Forbid();
#ifdef REAL_HARDWARE
  if (ahi_data->flags & ZZ_AX_DEVF_INT2MODE) {
    if (ahi_data->fabric_token_installed)
      RemIntServer(INTB_PORTS, fabric);
    RemIntServer(INTB_PORTS, irq);
  } else {
    if (ahi_data->fabric_token_installed)
      RemIntServer(INTB_EXTER, fabric);
    RemIntServer(INTB_EXTER, irq);
  }
#else
  if (ahi_data->fabric_token_installed)
    RemIntServer(INTB_VERTB, fabric);
  RemIntServer(INTB_VERTB, irq);
#endif
  ahi_data->fabric_token_installed = 0;
  ahi_data->irq_installed = 0;
  if (Z9AXBase && Z9AXBase->owner == ahi_data)
    Z9AXBase->owner = NULL;
  Permit();
}

// Identify the mode of an active MHI owner from its installed IRQ nodes.
// Qualified MHI publishes a fabric sentinel in addition to the legacy node
// retained for old AHI binaries; check fabric first. Fallback and old MHI
// publish only the legacy node. MUST run under Forbid().
#define Z9AX_MHI_MODE_NONE   0U
#define Z9AX_MHI_MODE_LEGACY 1U
#define Z9AX_MHI_MODE_FABRIC 2U
static uint8_t mhi_mode_locked(void) {
  struct List *IrqList;
  if(Z9AXBase->flags & ZZ_AX_DEVF_INT2MODE) {
    IrqList = (struct List *)SysBase->IntVects[INTB_PORTS].iv_Data;
  }
  else {
    IrqList = (struct List *)SysBase->IntVects[INTB_EXTER].iv_Data;
  }
  if (FindName(IrqList, (CONST_STRPTR)ZZ_AX_IRQ_NAME_MHI_FABRIC))
    return Z9AX_MHI_MODE_FABRIC;
  if (FindName(IrqList, (CONST_STRPTR)ZZ_AX_IRQ_NAME_MHI))
    return Z9AX_MHI_MODE_LEGACY;
  return Z9AX_MHI_MODE_NONE;
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

static int fabric_grant_range_valid(const struct z9ax *ahi_data,
                                    uint32_t offset, uint32_t size)
{
  if (ahi_data->zorro_version != 2U)
    return 1;
  if (ahi_data->z2_direct_ring_size !=
          ZZ_Z2_DIRECT_RING_RESERVE_SIZE ||
      size > ahi_data->z2_direct_ring_size ||
      offset < ahi_data->z2_direct_ring_base)
    return 0;
  return offset - ahi_data->z2_direct_ring_base <=
      ahi_data->z2_direct_ring_size - size;
}

/* Acquire one lease under the caller's mix rate. Tries the Zorro III
 * direct-ring slots in order and takes the first grant; a refusal
 * (occupied slots, conversion budget, bus policy) is a clean decline
 * and the caller fails the Start honestly instead of silently
 * falling back -- the firmware strips the legacy play bit while the
 * fabric owns the output, so a silent legacy fallback would be a
 * silent no-output. Every grant must fit the board window; Zorro II
 * additionally requires a negotiated generation-2 layout and keeps
 * both granted ranges inside its reserved direct-ring carve-out.
 * Requires ZZ9KBase and runs outside Forbid(). Returns 1 with session
 * mapped, 0 on any refusal. */
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
                  ZZ9K_AUDIO_RING_ACQUIRE_FLAG_SOURCE_RATE |
                  ZZ_AX_ACQUIRE_FLAG_SOURCE_S16BE);
    zz9k_put_be32(payload->source_rate_hz, mix_freq);

    status = ZZ9KCall(&request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
    if (status == ZZ9K_STATUS_BAD_REQUEST) {
      zz9k_put_be32(payload->flags,
                    ZZ9K_AUDIO_RING_ACQUIRE_FLAG_SOURCE_RATE);
      status = ZZ9KCall(&request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
    }
    if (status != ZZ9K_STATUS_OK) {
      KPrintF((CONST_STRPTR)"ZZ9000AX: lease acquire slot %lu refused: "
              "status %ld.\n", (unsigned long)slot, (long)status);
      if (status == ZZ9K_STATUS_TIMEOUT) {
        /* Applied-but-unacknowledged: do not probe another slot or let
         * Start retry until the heartbeatless grant can be revoked. */
        ahi_data->lease_acquire_uncertain = 1U;
        GetSysTime(&ahi_data->lease_retry_deadline);
        ahi_data->lease_retry_deadline.tv_secs += LEASE_RETRY_SECONDS;
        return 0;
      }
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

    /* The pinned SDK validator knows contracts 1 and 2 only. Contract 4
     * is the same source-rate geometry with m68k sample order. Remember
     * that, then validate it as contract 2. Rejecting it here releases
     * the grant and Start produces no audio. */
    ahi_data->lease_source_be =
        session->grant.sample_contract ==
        ZZ_AX_CONTRACT_SOURCE_RATE_STEREO_S16BE;
    if (ahi_data->lease_source_be)
      session->grant.sample_contract =
          ZZ9K_AUDIO_RING_CONTRACT_SOURCE_RATE_STEREO_S16LE;
    /* Board-window bounds + contract sanity (R2/R3): both granted
     * ranges must fit the board window, the contract must be the
     * requested source-rate lease, and the echoed rate must be the
     * mix frequency asked for. */
    if (session->grant.sample_contract !=
            ZZ9K_AUDIO_RING_CONTRACT_SOURCE_RATE_STEREO_S16LE ||
        session->grant.source_rate != mix_freq ||
        !zz9k_audio_ring_grant_valid(&session->grant) ||
        !fabric_grant_range_valid(ahi_data,
                                  session->grant.ring_offset,
                                  session->grant.ring_capacity) ||
        !fabric_grant_range_valid(ahi_data,
                                  session->grant.control_offset,
                                  ZZ9K_AUDIO_RING_CONTROL_SIZE) ||
        session->grant.ring_offset > Z9AXBase->hw_size ||
        session->grant.ring_capacity >
            Z9AXBase->hw_size - session->grant.ring_offset ||
        session->grant.control_offset > Z9AXBase->hw_size ||
        ZZ9K_AUDIO_RING_CONTROL_SIZE >
            Z9AXBase->hw_size - session->grant.control_offset) {
      /* Unusable grant: transfer its identity into the same retained,
       * retryable release path used by Stop. Do not probe another slot
       * while a failed cleanup may still own this one. */
      ahi_data->lease_held = 1U;
      fabric_lease_release(ahi_data);
      if (ahi_data->lease_release_pending)
        return 0;
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
    ahi_data->lease_retry_deadline.tv_secs = 0;
    ahi_data->lease_retry_deadline.tv_micro = 0;
    ahi_data->lease_held = 1;
    return 1;
  }
  return 0;
}

/* Idempotent surrender under the grant's own generation (a stale
 * generation release is a no-op by contract). Outside Forbid().
 * A failed mailbox completion retains the identity and blocks any
 * subsequent acquire until a timer-paced retry is confirmed. */
static void fabric_lease_release(struct z9ax *ahi_data)
{
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  int status;
  Forbid();
  if (ahi_data->lease_release_in_progress) {
    Permit();
    return;
  }
  ahi_data->lease_release_in_progress = 1U;
  Permit();

  if (ahi_data->lease_held) {
    ahi_data->lease_release_slot =
        ahi_data->lease_session.grant.slot;
    ahi_data->lease_release_generation =
        ahi_data->lease_session.grant.generation;
    ahi_data->lease_release_pending = 1U;
    ahi_data->lease_held = 0;
    ahi_data->lease_source_be = 0;
    memset(&ahi_data->lease_session, 0,
           sizeof(ahi_data->lease_session));
  }
  if (!ahi_data->lease_release_pending) {
    Forbid();
    ahi_data->lease_release_in_progress = 0U;
    Permit();
    return;
  }

  zz9k_request_init(&request, ZZ9K_OP_AUDIO_RING_RELEASE);
  request.entry.payload_len = sizeof(ZZ9KAudioRingReleasePayload);
  {
    ZZ9KAudioRingReleasePayload *rel =
        (ZZ9KAudioRingReleasePayload *)
            request.entry.payload.inline_data;
    zz9k_put_be32(rel->slot, ahi_data->lease_release_slot);
    zz9k_put_be32(rel->generation,
                  ahi_data->lease_release_generation);
    zz9k_put_be32(rel->flags, 0U);
  }
  status = ZZ9KCall(&request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
  if (status != ZZ9K_STATUS_OK) {
    GetSysTime(&ahi_data->lease_retry_deadline);
    ahi_data->lease_retry_deadline.tv_secs += LEASE_RETRY_SECONDS;
    KPrintF((CONST_STRPTR)"ZZ9000AX: fabric lease release deferred "
            "(slot %lu gen %lu, status %ld).\n",
            (unsigned long)ahi_data->lease_release_slot,
            (unsigned long)ahi_data->lease_release_generation,
            (long)status);
    Forbid();
    ahi_data->lease_release_in_progress = 0U;
    Permit();
    return;
  }

  KPrintF((CONST_STRPTR)"ZZ9000AX: fabric lease released (slot %lu "
          "gen %lu).\n",
          (unsigned long)ahi_data->lease_release_slot,
          (unsigned long)ahi_data->lease_release_generation);
  ahi_data->lease_release_pending = 0U;
  ahi_data->lease_release_slot = 0U;
  ahi_data->lease_release_generation = 0U;
  ahi_data->lease_retry_deadline.tv_secs = 0;
  ahi_data->lease_retry_deadline.tv_micro = 0;
  Forbid();
  ahi_data->lease_release_in_progress = 0U;
  Permit();
}

/* ---- Hardware-mixing modes (voice engine) ----
 *
 * AudioModes $CAFE0002 (nearest sample) and $CAFE0003 (linear interpolation)
 * carry the private tags AHIDB_ZZVoice and AHIDB_ZZVoiceLinear, whose data
 * is the mode id. ahi.device hands a mode's tag list to AllocAudio and
 * GetAttr, so the driver reads the mode from it. In these modes AllocAudio
 * returns no AHISF_* flag: ahi.device mixes nothing and forwards every
 * AHI_SetSound, AHI_SetFreq and AHI_SetVol to the driver. Each AHI channel
 * becomes a voice of the card's voice engine through voice_model.c.
 *
 * One worker process owns the model, the command ring and the session. The
 * AHI calls append to the model's FIFO from any context, under Disable().
 * Start and Stop hand the worker a command and wait for its answer.
 * LoadSound copies into the card bank through the session's staging buffer.
 * Nothing falls back to software mixing: when the engine cannot be used,
 * AllocAudio fails and the debug channel says why.
 */

/* AHIDB_UserBase is in the AHI developer includes, not in every NDK. */
#ifndef AHIDB_UserBase
#define AHIDB_UserBase (AHI_TagBase + 500)
#endif
#define AHIDB_ZZVoice       (AHIDB_UserBase + 0)
#define AHIDB_ZZVoiceLinear (AHIDB_UserBase + 1)

#define Z9AX_VOICE_ID_NEAREST 0xCAFE0002UL
#define Z9AX_VOICE_ID_LINEAR  0xCAFE0003UL

#define Z9AX_VOICE_REASON_LIBRARY   1U
#define Z9AX_VOICE_REASON_REVISION  2U
#define Z9AX_VOICE_REASON_ZORRO2    3U
#define Z9AX_VOICE_REASON_NO_ENGINE 4U
#define Z9AX_VOICE_REASON_OPEN      5U
#define Z9AX_VOICE_REASON_MEMORY    6U
#define Z9AX_VOICE_REASON_VIEW      7U

/* The sample bank on the card. ENV:ZZ9K_AHI_BANK (KiB) overrides the
 * request; the card may grant less (see voice_open_session). */
#define Z9AX_VOICE_BANK_DEFAULT (16U * 1024U * 1024U)
#define Z9AX_VOICE_BANK_MIN     (1U * 1024U * 1024U)
/* Zorro III host buffers: a 1024-record ring and a 64 KiB staging buffer
 * make a multi-megabyte sample set load in few round trips. Zorro II asks
 * for the library defaults, which fit the 16 KiB host window. */
#define Z9AX_VOICE_RING_BYTES \
  (ZZ9K_VOICE_RING_HEADER_BYTES + 1024U * ZZ9K_VOICE_RECORD_BYTES)
#define Z9AX_VOICE_STAGING_BYTES (64U * 1024U)
/* The worker wakes this often: the heartbeat, the command ring and the
 * tick scheduler all run from it. */
#define Z9AX_VOICE_POLL_MICRO 4000U
#define Z9AX_VOICE_TICKS_PER_POLL 4U
/* The engine renders in 20 ms periods of 960 frames. */
#define Z9AX_VOICE_PERIOD_FRAMES (ZZ9K_VOICE_OUTPUT_RATE / 50U)

/* The session calls go through these names so voice_worker_test.c can
 * replace them with a fake session (it defines ZZ_AX_VOICE_TEST). */
#ifdef ZZ_AX_VOICE_TEST
int zz_ax_voice_open(const ZZ9KVoiceOpenDesc *desc, ZZ9KVoiceSession *session);
int zz_ax_voice_close(ZZ9KVoiceSession *session);
int zz_ax_voice_reset(ZZ9KVoiceSession *session);
uint32_t zz_ax_voice_write(ZZ9KVoiceSession *session,
                           const ZZ9KVoiceRecord *records, uint32_t count);
int zz_ax_voice_get_status(ZZ9KVoiceSession *session, ZZ9KVoiceStatus *status);
#else
#define zz_ax_voice_open       ZZ9KVoiceOpen
#define zz_ax_voice_close      ZZ9KVoiceClose
#define zz_ax_voice_reset      ZZ9KVoiceReset
#define zz_ax_voice_write      ZZ9KVoiceWrite
#define zz_ax_voice_get_status ZZ9KVoiceGetStatus
#endif
static void voice_timer_end(struct z9ax *ahi_data);

/* The mode the tag list names: Z9AX_VOICE_NEAREST, Z9AX_VOICE_LINEAR, or
 * Z9AX_VOICE_NONE for a software mode (or no tag list). */
static uint8_t voice_mode_of(const struct TagItem *tags)
{
  const struct TagItem *tag;
  uint32_t id = 0U;
  uint8_t mode = Z9AX_VOICE_NONE;

  for (tag = tags; tag != NULL && tag->ti_Tag != TAG_DONE; tag++) {
    if (tag->ti_Tag == AHIDB_ZZVoice || tag->ti_Tag == AHIDB_ZZVoiceLinear) {
      id = tag->ti_Data;
      mode = (uint8_t)(tag->ti_Tag == AHIDB_ZZVoice ? Z9AX_VOICE_NEAREST
                                                    : Z9AX_VOICE_LINEAR);
    }
  }
  if ((mode == Z9AX_VOICE_NEAREST && id != Z9AX_VOICE_ID_NEAREST) ||
      (mode == Z9AX_VOICE_LINEAR && id != Z9AX_VOICE_ID_LINEAR)) {
    mode = Z9AX_VOICE_NONE;
  }
  return mode;
}

static const char *voice_reason_text(uint8_t reason)
{
  switch (reason) {
    case Z9AX_VOICE_REASON_LIBRARY:
      return "zz9k.library is not available";
    case Z9AX_VOICE_REASON_REVISION:
      return "zz9k.library is older than revision 42";
    case Z9AX_VOICE_REASON_ZORRO2:
      return "the firmware does not advertise the voice engine on Zorro II";
    case Z9AX_VOICE_REASON_NO_ENGINE:
      return "the firmware does not advertise the voice engine";
    case Z9AX_VOICE_REASON_OPEN:
      return "the voice engine refused the session (engine busy or card reset)";
    case Z9AX_VOICE_REASON_MEMORY:
      return "the card has no memory for the sample bank";
    case Z9AX_VOICE_REASON_VIEW:
      return "the voice engine refused the sample views";
    default:
      return "unknown";
  }
}

/* One line per refusal on the debug channel, with what the user can do. */
static void voice_refused(uint8_t reason, uint32_t status)
{
  KPrintF((CONST_STRPTR)"ZZ9000AX: hardware mixing refused: %s",
          (CONST_STRPTR)voice_reason_text(reason));
  if (status != 0U)
    KPrintF((CONST_STRPTR)" (status %ld)", (long)status);
  KPrintF((CONST_STRPTR)". The hardware mixing modes need matched firmware"
          " and zz9k.library 42 or newer."
          " Streaming clients (Play16, ZZPlay AHI output, datatypes)"
          " use a 16 bit Stereo mode.\n");
}

/* Flags of the audio service, or 0 when the query fails. */
static uint32_t voice_service_flags(void)
{
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  ZZ9KQueryServicePayload *query;
  ZZ9KServiceInfoPayload info;

  zz9k_request_init(&request, ZZ9K_OP_QUERY_SERVICE);
  request.entry.payload_len = sizeof(*query);
  query = (ZZ9KQueryServicePayload *)request.entry.payload.inline_data;
  zz9k_put_be32(query->service_id, ZZ9K_SERVICE_AUDIO);
  if (ZZ9KCall(&request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS) !=
      ZZ9K_STATUS_OK)
    return 0U;
  memcpy(&info, reply.payload.inline_data, sizeof(info));
  return zz9k_get_be32(info.flags);
}

/* Whether the firmware and zz9k.library can run the engine. Sets *stereo_ok
 * when the firmware advertises stereo voice samples. Returns a
 * Z9AX_VOICE_REASON_* or 0. Needs ZZ9KBase; blocks on the mailbox. */
static uint8_t voice_probe(struct z9ax_voice *v)
{
  uint32_t flags;

  if (ZZ9KBase->lib_Revision < ZZ9K_LIBRARY_MIN_REVISION_AUDIO_VOICE)
    return Z9AX_VOICE_REASON_REVISION;
  flags = voice_service_flags();
  if ((flags & ZZ9K_SERVICE_FLAG_AUDIO_VOICE) == 0U)
    return Z9AX_VOICE_REASON_NO_ENGINE;
  v->stereo_ok = (flags & ZZ9K_SERVICE_FLAG_AUDIO_VOICE_STEREO) != 0U;
  return 0U;
}

/* ENV:name as a decimal number, or def when it is absent or not a number. */
static uint32_t voice_env_number(const char *name, uint32_t def)
{
  char text[16];
  LONG length;
  LONG i;
  uint32_t value = 0U;

  length = GetVar((CONST_STRPTR)name, text, (LONG)sizeof(text),
                  GVF_GLOBAL_ONLY);
  if (length <= 0)
    return def;
  for (i = 0; i < length; i++) {
    if (text[i] < '0' || text[i] > '9' || value > 0x19999999U)
      return def;
    value = value * 10U + (uint32_t)(text[i] - '0');
  }
  return value;
}

/* Model hooks. PlayerFunc and SoundFunc are the client's hooks, called on
 * the worker. */
static void voice_hook_player(void *user)
{
  struct z9ax *ahi_data = (struct z9ax *)user;
  struct AHIAudioCtrlDrv *audio = ahi_data->audioctrl;

  if (audio->ahiac_PlayerFunc)
    CallHookPkt(audio->ahiac_PlayerFunc, audio, NULL);
}

static void voice_hook_sound(void *user, uint16_t channel)
{
  struct z9ax *ahi_data = (struct z9ax *)user;
  struct AHIAudioCtrlDrv *audio = ahi_data->audioctrl;
  struct AHISoundMessage message;

  message.ahism_Channel = channel;
  if (audio->ahiac_SoundFunc)
    CallHookPkt(audio->ahiac_SoundFunc, audio, &message);
}

static void voice_hook_lock(void *user)
{
  (void)user;
  Disable();
}

static void voice_hook_unlock(void *user)
{
  (void)user;
  Enable();
}

/* AHIsub_Disable protects client data from the hooks: nothing is taken
 * from the FIFO while it is nested, so an AHI_PlayA sequence stays whole. */
static int voice_hook_blocked(void *user)
{
  return ((struct z9ax *)user)->disable_cnt != 0U;
}

static uint32_t voice_hook_player_freq(void *user)
{
  return ((struct z9ax *)user)->audioctrl->ahiac_PlayerFreq;
}

static void voice_timer_post(struct z9ax *ahi_data)
{
  timer_post_micro(ahi_data, Z9AX_VOICE_POLL_MICRO);
}

/* The worker's pacing timer (timer.device, UNIT_MICROHZ), on the request
 * and port the lease worker would use. Brings them up and posts the first
 * request. Returns 0 when timer.device is not available. */
static int voice_timer_setup(struct z9ax *ahi_data)
{
  ahi_data->lease_timer_port = CreateMsgPort();
  if (ahi_data->lease_timer_port) {
    ahi_data->lease_timer_req = (struct timerequest *)CreateIORequest(
        (APTR)ahi_data->lease_timer_port, sizeof(struct timerequest));
  }
  if (ahi_data->lease_timer_req &&
      OpenDevice((STRPTR)"timer.device", UNIT_MICROHZ,
                 (struct IORequest *)ahi_data->lease_timer_req, 0) == 0) {
    ahi_data->lease_timer_signal =
        (int8_t)ahi_data->lease_timer_port->mp_SigBit;
    TimerBase = ahi_data->lease_timer_req->tr_node.io_Device;
    GetSysTime(&ahi_data->lease_timer_deadline);
    voice_timer_post(ahi_data);
    return 1;
  }
  voice_timer_end(ahi_data);
  return 0;
}

/* Defines engine sample `view` over the whole bank (DEFINE only), so that
 * a sound is a view plus a byte offset. Needs the session open; blocks on
 * the mailbox. */
static int voice_define_view(struct z9ax_voice *v, uint32_t view)
{
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  int status;

  status = zz9k_request_audio_voice_upload(
      &request, v->session.grant.session, ZZ9K_VOICE_UPLOAD_FLAG_DEFINE, 0U,
      0U, 0U, view, 0U, v->session.grant.bank_bytes, view);
  if (status != ZZ9K_STATUS_OK)
    return status;
  memset(&reply, 0, sizeof(reply));
  return ZZ9KCall(&request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
}

/* Opens the voice session and starts the model. Returns 1, or 0 with
 * v->reason and v->open_status set. A NO_MEMORY answer halves the bank down
 * to its floor, then drops the large host buffers for the defaults. */
static int voice_open_session(struct z9ax *ahi_data)
{
  struct z9ax_voice *v = ahi_data->voice;
  ZZ9KVoiceOpenDesc desc;
  VmConfig cfg;
  uint32_t bank = v->bank_request;
  uint32_t ring_bytes = v->zorro == 2U ? 0U : Z9AX_VOICE_RING_BYTES;
  uint32_t staging_bytes = v->zorro == 2U ? 0U : Z9AX_VOICE_STAGING_BYTES;
  uint32_t attempts;
  uint32_t view;
  int status = ZZ9K_STATUS_OK;

  for (attempts = 0U; attempts < 16U; attempts++) {
    memset(&desc, 0, sizeof(desc));
    desc.voices = v->channels;
    desc.bank_bytes = bank;
    desc.lead_frames = v->lead_request;
    desc.flags = v->linear ? ZZ9K_VOICE_OPEN_FLAG_LINEAR : 0U;
    desc.ring_bytes = ring_bytes;
    desc.staging_bytes = staging_bytes;
    status = zz_ax_voice_open(&desc, &v->session);
    if (status != ZZ9K_STATUS_NO_MEMORY)
      break;
    if (bank > Z9AX_VOICE_BANK_MIN) {
      bank >>= 1;
      if (bank < Z9AX_VOICE_BANK_MIN)
        bank = Z9AX_VOICE_BANK_MIN;
    } else if (ring_bytes != 0U || staging_bytes != 0U) {
      bank = v->bank_request;
      ring_bytes = 0U;
      staging_bytes = 0U;
    } else {
      break;
    }
  }
  v->open_status = (uint32_t)status;
  if (status != ZZ9K_STATUS_OK) {
    v->reason = (uint8_t)(status == ZZ9K_STATUS_NO_MEMORY
                              ? Z9AX_VOICE_REASON_MEMORY
                              : Z9AX_VOICE_REASON_OPEN);
    return 0;
  }
  v->session_open = 1U;
  if (v->session.grant.voices < v->channels ||
      v->session.grant.bank_bytes < 4U) {
    v->reason = Z9AX_VOICE_REASON_OPEN;
    v->open_status = ZZ9K_STATUS_INTERNAL_ERROR;
    goto fail;
  }
  v->lead = v->session.grant.lead_frames;
  for (view = 0U; view < VM_VIEWS; view++) {
    if ((view == VM_VIEW_S8S || view == VM_VIEW_S16S) && !v->stereo_ok)
      continue;
    status = voice_define_view(v, view);
    if (status != ZZ9K_STATUS_OK) {
      v->reason = Z9AX_VOICE_REASON_VIEW;
      v->open_status = (uint32_t)status;
      goto fail;
    }
  }
  memset(&cfg, 0, sizeof(cfg));
  cfg.channels = v->channels;
  cfg.sounds = v->sounds;
  cfg.bank_bytes = v->session.grant.bank_bytes & ~3U;
  cfg.stereo_views = v->stereo_ok;
  cfg.hooks.player = voice_hook_player;
  cfg.hooks.sound = voice_hook_sound;
  cfg.hooks.lock = voice_hook_lock;
  cfg.hooks.unlock = voice_hook_unlock;
  cfg.hooks.blocked = voice_hook_blocked;
  cfg.hooks.player_freq = voice_hook_player_freq;
  cfg.hooks.user = ahi_data;
  vm_init(&v->model, &cfg);
  return 1;

fail:
  zz_ax_voice_close(&v->session);
  v->session_open = 0U;
  return 0;
}

/* Stops the pacing timer and frees its request and port. */
static void voice_timer_end(struct z9ax *ahi_data)
{
  if (ahi_data->lease_timer_req) {
    if (ahi_data->lease_timer_req->tr_node.io_Device) {
      AbortIO((struct IORequest *)ahi_data->lease_timer_req);
      WaitIO((struct IORequest *)ahi_data->lease_timer_req);
      CloseDevice((struct IORequest *)ahi_data->lease_timer_req);
      TimerBase = NULL;
    }
    DeleteIORequest((struct IORequest *)ahi_data->lease_timer_req);
    ahi_data->lease_timer_req = NULL;
  }
  if (ahi_data->lease_timer_port) {
    DeleteMsgPort(ahi_data->lease_timer_port);
    ahi_data->lease_timer_port = NULL;
  }
  ahi_data->lease_timer_signal = -1;
}

/* Reads the status block: the heartbeat, the clock and the counters. A
 * status that reads closed means the card ended the session (lost heartbeat
 * or reset); playback stops and says so. Returns 0 on a failed read. */
static int voice_read_status(struct z9ax *ahi_data)
{
  struct z9ax_voice *v = ahi_data->voice;
  ZZ9KVoiceStatus status;

  if (zz_ax_voice_get_status(&v->session, &status) != ZZ9K_STATUS_OK) {
    v->status_failures++;
    return 0;
  }
  v->status = status;
  if (status.state == ZZ9K_VOICE_STATE_CLOSED && !v->session_lost) {
    v->session_lost = 1U;
    v->playing = 0U;
    KPrintF((CONST_STRPTR)"ZZ9000AX: the card closed the voice session"
            " (reset or lost heartbeat); sound stops.\n");
  }
  return 1;
}

/* Hands the model's records to the ring. Only the worker calls this. What
 * the ring could not take stays for the next pass. */
static void voice_write_records(struct z9ax *ahi_data)
{
  struct z9ax_voice *v = ahi_data->voice;
  const ZZ9KVoiceRecord *records;
  uint32_t count;
  uint32_t written;

  for (;;) {
    records = vm_records(&v->model, &count);
    if (count == 0U)
      return;
    written = zz_ax_voice_write(&v->session, records, count);
    if (written < count)
      v->ring_full++;
    if (written == 0U)
      return;
    vm_records_taken(&v->model, written);
  }
}

/* AHI_UnloadSound only sets a bit: it may not call the mailbox, because it
 * must not break a Forbid(). The worker frees the sounds here; a sound a
 * voice is playing stops at the model time and its bank range waits for the
 * engine horizon (voice_model.c). */
static void voice_run_unloads(struct z9ax *ahi_data)
{
  struct z9ax_voice *v = ahi_data->voice;
  uint32_t word;

  for (word = 0U; word < VM_MAX_SOUNDS / 32U; word++) {
    uint32_t mask;

    Forbid();
    mask = v->unload_pending[word];
    v->unload_pending[word] = 0U;
    Permit();
    while (mask != 0U) {
      uint32_t bit = 0U;

      while ((mask & (1UL << bit)) == 0U)
        bit++;
      mask &= ~(1UL << bit);
      Forbid();
      vm_sound_remove(&v->model, word * 32U + bit);
      Permit();
    }
  }
}

/* AHIET_CHANNELINFO: the offset of every channel at the model time, which
 * is up to one lead ahead of the sound, as PlayerFunc is. */
static void voice_channel_info(struct z9ax *ahi_data)
{
  struct z9ax_voice *v = ahi_data->voice;
  struct AHIEffChannelInfo *info;
  struct Hook *hook;

  Forbid();
  info = v->channelinfo;
  Permit();
  if (!info || info->ahieci_Channels != v->channels)
    return;
  hook = info->ahieci_Func;
  if (!hook)
    return;
  vm_channel_offsets(&v->model, info->ahieci_Offset, v->channels);
  CallHookPkt(hook, ahi_data->audioctrl, info);
}

/* Where a new timeline starts: the granted lead ahead of the playing frame,
 * and never closer to the engine horizon than one period, so the first
 * records cannot be late when the firmware advances between the status
 * read and the write. The first record of the timeline, the MASTER gain,
 * starts the engine clock when the session is new. */
static uint32_t voice_anchor(const struct z9ax_voice *v)
{
  uint32_t stamp = v->status.playing_frame + v->lead;

  if ((int32_t)(stamp - v->status.horizon) < (int32_t)Z9AX_VOICE_PERIOD_FRAMES)
    stamp = v->status.horizon + Z9AX_VOICE_PERIOD_FRAMES;
  return stamp;
}

/* Start and Stop, as the worker runs them. RESET drops every queued record
 * before the model is moved, so nothing stamped for the old timeline plays. */
static void voice_handle_cmd(struct z9ax *ahi_data)
{
  struct z9ax_voice *v = ahi_data->voice;
  uint32_t cmd = v->cmd;
  uint32_t result = AHIE_OK;

  if (cmd == Z9AX_VOICE_CMD_NONE)
    return;
  v->cmd = Z9AX_VOICE_CMD_NONE;
  if (!v->session_open || v->session_lost || !voice_read_status(ahi_data) ||
      v->status.state == ZZ9K_VOICE_STATE_CLOSED) {
    result = AHIE_UNKNOWN;
  } else if (cmd == Z9AX_VOICE_CMD_START) {
    zz_ax_voice_reset(&v->session);
    vm_stop(&v->model);
    vm_start(&v->model, voice_anchor(v));
    if (v->start_frame == 0U)
      v->start_frame = v->model.now;
    v->playing = 1U;
    voice_write_records(ahi_data);
  } else {
    zz_ax_voice_reset(&v->session);
    vm_stop(&v->model);
    v->playing = 0U;
  }
  v->cmd_result = result;
  if (v->cmd_task && v->cmd_signal != -1)
    Signal(v->cmd_task, 1UL << v->cmd_signal);
}

/* One COPY upload through the staging buffer: at most one staging buffer of
 * whole longwords from the front of the data. Only the worker calls this.
 * *done is the number of bytes the mailbox took. */
static int voice_copy_step(struct z9ax_voice *v, uint32_t bank_offset,
                           const uint8_t *data, uint32_t length,
                           uint32_t *done)
{
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  uint32_t chunk_max = v->session.staging_bytes & ~3U;
  uint32_t chunk = length < chunk_max ? length : chunk_max;
  int status;

  *done = 0U;
  if (chunk == 0U)
    return ZZ9K_STATUS_BAD_REQUEST;
  memcpy((void *)(uintptr_t)v->session.staging, data, chunk);
  zz9k_audio_ring_cache_flush(v->session.staging, chunk);
  status = zz9k_request_audio_voice_upload(
      &request, v->session.grant.session, ZZ9K_VOICE_UPLOAD_FLAG_COPY, 0U,
      bank_offset, chunk, 0U, 0U, 0U, 0U);
  if (status != ZZ9K_STATUS_OK)
    return status;
  memset(&reply, 0, sizeof(reply));
  status = ZZ9KCall(&request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
  if (status == ZZ9K_STATUS_OK)
    *done = chunk;
  return status;
}

/* Ends the waiting LoadSound's request with `status` and wakes its task. */
static void voice_upload_done(struct z9ax_voice *v, int status)
{
  struct Task *task = v->up_task;
  int8_t signal = v->up_signal;

  v->up_status = (uint32_t)status;
  v->up_pending = 0U;
  Signal(task, 1UL << signal);
}

/* The copy of a LoadSound, one staging buffer per worker pass: the status
 * read of every pass keeps the heartbeat (each upload call counts as
 * liveness too) and the model ticks go on, so even a sample of megabytes
 * neither lets the session lapse nor stalls playing voices. A request that
 * is not finished wakes the worker again at once through its own signal. */
static void voice_handle_upload(struct z9ax *ahi_data)
{
  struct z9ax_voice *v = ahi_data->voice;
  uint32_t done = 0U;
  int status;

  if (!v->up_pending)
    return;
  if (!v->session_open || v->session_lost) {
    voice_upload_done(v, ZZ9K_STATUS_BAD_HANDLE);
    return;
  }
  status = voice_copy_step(v, v->up_offset, v->up_data, v->up_length, &done);
  v->up_data += done;
  v->up_offset += done;
  v->up_length -= done;
  if (status == ZZ9K_STATUS_OK && v->up_length != 0U) {
    Signal(FindTask(NULL), 1UL << ahi_data->enable_signal);
    return;
  }
  voice_upload_done(v, status);
}

/* One worker pass: the command, the heartbeat and the clock, the bank, then
 * as many ticks as are due. */
static void voice_service(struct z9ax *ahi_data)
{
  struct z9ax_voice *v = ahi_data->voice;
  uint32_t budget;

  voice_handle_cmd(ahi_data);
  voice_handle_upload(ahi_data);
  if (!v->session_open)
    return;
  if (!voice_read_status(ahi_data))
    return;
  Forbid();
  vm_bank_poll(&v->model, v->status.horizon);
  Permit();
  voice_run_unloads(ahi_data);
  if (!v->playing || v->session_lost)
    return;
  /* A worker that could not run for longer than the lead is behind the
   * sound and its records would be late. Anchor the model to the card again
   * instead of replaying a backlog. */
  if ((int32_t)(v->status.playing_frame - v->model.now) > 0) {
    zz_ax_voice_reset(&v->session);
    vm_stop(&v->model);
    vm_start(&v->model, voice_anchor(v));
    v->resyncs++;
  }
  /* While the client holds AHIsub_Disable its hooks wait, as the software
   * modes' interrupt does: no PlayerFunc, no SoundFunc and no channel info.
   * The status read above keeps the session alive, and records already
   * made still go out. */
  if (ahi_data->disable_cnt == 0U)
    vm_run_async(&v->model);
  for (budget = Z9AX_VOICE_TICKS_PER_POLL; budget != 0U; budget--) {
    if (ahi_data->disable_cnt != 0U)
      break;
    /* The ring is slow: let it drain before the record buffer fills. */
    if (v->model.out_n > VM_OUT_RECORDS / 2U)
      break;
    if (!vm_tick_due(&v->model, v->status.playing_frame, 2U * v->lead))
      break;
    vm_tick(&v->model);
  }
  voice_write_records(ahi_data);
  if (ahi_data->disable_cnt == 0U)
    voice_channel_info(ahi_data);
}

static char *voice_text_number(char *p, uint32_t value)
{
  char digits[10];
  uint32_t n = 0U;

  do {
    digits[n++] = (char)('0' + (int)(value % 10U));
    value /= 10U;
  } while (value != 0U);
  while (n != 0U)
    *p++ = digits[--n];
  return p;
}

static char *voice_text_field(char *p, const char *name, uint32_t value)
{
  while (*name != '\0')
    *p++ = *name++;
  *p++ = '=';
  p = voice_text_number(p, value);
  *p++ = ' ';
  return p;
}

/* ENV:ZZ9K_AHI_VOICE_STATS holds the engine counters and the model's own
 * for the session that closed. The voice session is exclusive, so no second
 * tool can read the counters while a client runs. */
static void voice_write_stats(struct z9ax *ahi_data)
{
  struct z9ax_voice *v = ahi_data->voice;
  char text[256];
  char *p = text;

  p = voice_text_field(p, "late", v->status.late);
  p = voice_text_field(p, "rejected", v->status.rejected);
  p = voice_text_field(p, "underruns", v->status.underruns);
  p = voice_text_field(p, "dropped", v->status.dropped);
  p = voice_text_field(p, "ringfull", v->ring_full);
  p = voice_text_field(p, "ticks", v->model.ticks);
  p = voice_text_field(p, "frames", v->model.now - v->start_frame);
  p = voice_text_field(p, "resyncs", v->resyncs);
  p = voice_text_field(p, "fifodrops", v->model.fifo_drops);
  p = voice_text_field(p, "recorddrops", v->model.out_drops);
  p = voice_text_field(p, "overload", v->model.overload);
  p[-1] = '\0';
  SetVar((CONST_STRPTR)"ZZ9K_AHI_VOICE_STATS", (STRPTR)text, -1L,
         GVF_GLOBAL_ONLY);
  KPrintF((CONST_STRPTR)"ZZ9000AX: voice session closed: %s\n",
          (CONST_STRPTR)text);
}

/* The worker's first step: its signal, the timer, the session and the
 * model. Returns 1 when the worker can run; on 0 v->reason says why (0 for
 * a local failure) and nothing is left allocated. */
static int voice_worker_init(struct z9ax *ahi_data)
{
  struct z9ax_voice *v = ahi_data->voice;

  ahi_data->enable_signal = AllocSignal(-1);
  if (ahi_data->enable_signal == -1)
    return 0;
  v->lead_request = voice_env_number("ZZ9K_AHI_LEAD", 0U);
  v->bank_request =
      voice_env_number("ZZ9K_AHI_BANK", Z9AX_VOICE_BANK_DEFAULT >> 10) << 10;
  if (v->bank_request > ZZ9K_VOICE_BANK_MAX_BYTES)
    v->bank_request = ZZ9K_VOICE_BANK_MAX_BYTES;
  if (!voice_timer_setup(ahi_data))
    return 0;
  if (!voice_open_session(ahi_data))
    return 0;
  return 1;
}

/* The worker's last step: with the session live the counters go to
 * ENV:ZZ9K_AHI_VOICE_STATS and the session closes; then the timer. */
static void voice_worker_end(struct z9ax *ahi_data, int live)
{
  struct z9ax_voice *v = ahi_data->voice;

  /* A LoadSound still waiting for the copy cannot be served any more. */
  if (v->up_pending)
    voice_upload_done(v, ZZ9K_STATUS_BAD_HANDLE);
  if (live && v->session_open) {
    if (zz_ax_voice_get_status(&v->session, &v->status) != ZZ9K_STATUS_OK)
      v->status_failures++;
    voice_write_stats(ahi_data);
    zz_ax_voice_close(&v->session);
    v->session_open = 0U;
  }
  voice_timer_end(ahi_data);
}

static void VoiceWorkerProcess(void)
{
  struct Process *proc = (struct Process *)FindTask(NULL);
  struct z9ax *ahi_data = proc->pr_Task.tc_UserData;
  uint32_t timer_sig;
  uint32_t signals;
  int ready;

  ready = voice_worker_init(ahi_data);
  if (!ready) {
    voice_worker_end(ahi_data, 0);
    FreeSignal(ahi_data->enable_signal);
    ahi_data->enable_signal = -1;
    /* Publish the failure before the handshake, like the software worker. */
    ahi_data->worker_process = NULL;
  }
  timer_sig = ready ? (1UL << ahi_data->lease_timer_signal) : 0UL;
  Signal(ahi_data->t_mainproc, 1UL << ahi_data->mainproc_signal);
  if (!ready)
    return;

  for (;;) {
    signals = Wait(SIGBREAKF_CTRL_C | (1UL << ahi_data->enable_signal) |
                   timer_sig);
    if (signals & SIGBREAKF_CTRL_C)
      break;
    if (signals & timer_sig) {
      while (GetMsg(ahi_data->lease_timer_port))
        ;
      voice_timer_post(ahi_data);
    }
    voice_service(ahi_data);
  }

  voice_worker_end(ahi_data, 1);
  Forbid();
  FreeSignal(ahi_data->enable_signal);
  ahi_data->enable_signal = -1;
  Permit();
  ahi_data->worker_process = NULL;
  Signal(ahi_data->t_mainproc, 1UL << ahi_data->mainproc_signal);
}

/* Hands the worker a Start or a Stop and waits for its answer. AHI calls
 * this from task context. Returns AHIE_UNKNOWN when there is no worker to
 * answer (or the caller is the worker itself). */
static uint32_t voice_command(struct z9ax *ahi_data, uint32_t cmd)
{
  struct z9ax_voice *v = ahi_data->voice;
  struct Task *me = FindTask(NULL);
  int8_t signal;
  uint32_t result;

  if (!ahi_data->worker_process ||
      (struct Task *)ahi_data->worker_process == me)
    return AHIE_UNKNOWN;
  signal = AllocSignal(-1);
  if (signal == -1)
    return AHIE_UNKNOWN;
  Forbid();
  v->cmd_task = me;
  v->cmd_signal = signal;
  v->cmd_result = AHIE_UNKNOWN;
  v->cmd = cmd;
  Signal((struct Task *)ahi_data->worker_process,
         1UL << ahi_data->enable_signal);
  Permit();
  Wait(1UL << signal);
  result = v->cmd_result;
  FreeSignal(signal);
  return result;
}

/* Ends the worker and waits for it. Called while the ownership nodes still
 * name this driver, so MHI does not claim the card in between. */
static void voice_wait_worker(struct z9ax *ahi_data)
{
  if (ahi_data->worker_process) {
    Signal((struct Task *)ahi_data->worker_process, SIGBREAKF_CTRL_C);
    if (ahi_data->mainproc_signal != -1)
      Wait(1UL << ahi_data->mainproc_signal);
    ahi_data->worker_process = NULL;
  }
}

/* AllocAudio for a hardware-mixing mode. Returns 0 on success: no AHISF_*
 * flag, so ahi.device mixes nothing and times nothing, and ahiac_MixFreq
 * is the engine rate. Every refusal prints its reason and leaves the
 * owner, the library base and the card as they were. The voice session
 * opens before the ownership claim and is closed again when the claim
 * fails, so a refused allocation never disturbs the active owner. */
static uint32_t voice_alloc_audio(uint8_t mode,
                                  struct AHIAudioCtrlDrv *AudioCtrl)
{
  uint32_t hw_addr = Z9AXBase->hw_addr;
  struct z9ax *ahi_data;
  struct z9ax_voice *v;
  uint8_t mhi_mode;
  uint8_t taken;

  if (AudioCtrl->ahiac_Channels == 0U ||
      AudioCtrl->ahiac_Channels > ZZ9K_VOICE_MAX_VOICES) {
    KPrintF((CONST_STRPTR)"ZZ9000AX: hardware mixing refused: %lu channels,"
            " the voice engine has 32. Use a 16 bit Stereo mode or fewer"
            " channels.\n",
            (unsigned long)AudioCtrl->ahiac_Channels);
    return AHISF_ERROR;
  }
  if (Z9AXBase->zorro_version == 2U) {
    voice_refused(Z9AX_VOICE_REASON_ZORRO2, 0U);
    return AHISF_ERROR;
  }
  Forbid();
  taken = (Z9AXBase->owner || Z9AXBase->allocating) ? 1U : 0U;
  Permit();
  if (taken)
    return AHISF_ERROR;
  if ((read_reg(hw_addr, ZZ_REG_AUDIO_CONFIG) & 1U) == 0U) {
    KPrintF((CONST_STRPTR)"ZZ9000AX: hardware mixing refused: ZZ9000AX audio"
            " not detected.\n");
    return AHISF_ERROR;
  }

  ahi_data = AllocVec(sizeof(struct z9ax), MEMF_PUBLIC | MEMF_FAST | MEMF_CLEAR);
  v = ahi_data ? (struct z9ax_voice *)AllocVec(sizeof(struct z9ax_voice),
                                               MEMF_PUBLIC | MEMF_CLEAR)
               : NULL;
  if (!ahi_data || !v) {
    if (v)
      FreeVec(v);
    if (ahi_data)
      FreeVec(ahi_data);
    KPrintF((CONST_STRPTR)"ZZ9000AX: hardware mixing refused: no memory for"
            " the driver data.\n");
    return AHISF_ERROR;
  }
  ahi_data->play_stop = 1;
  ahi_data->record_stop = 1;
  ahi_data->flags = Z9AXBase->flags;
  ahi_data->hw_addr = hw_addr;
  ahi_data->audioctrl = AudioCtrl;
  ahi_data->ahi_base = AHIsubBase;
  ahi_data->worker_signal = -1;
  ahi_data->enable_signal = -1;
  ahi_data->mainproc_signal = -1;
  ahi_data->lease_timer_signal = -1;
  ahi_data->zorro_version = Z9AXBase->zorro_version;
  ahi_data->t_mainproc = FindTask(NULL);
  ahi_data->voice_mode = mode;
  ahi_data->voice = v;
  v->linear = (uint8_t)(mode == Z9AX_VOICE_LINEAR);
  v->zorro = Z9AXBase->zorro_version;
  v->channels = AudioCtrl->ahiac_Channels;
  v->sounds = AudioCtrl->ahiac_Sounds;
  v->cmd = Z9AX_VOICE_CMD_NONE;
  v->cmd_signal = -1;
  InitSemaphore(&v->upload_lock);

  /* Serialize before touching the driver-global zz9k.library base. */
  Forbid();
  if (Z9AXBase->owner || Z9AXBase->allocating) {
    Permit();
    FreeVec(v);
    FreeVec(ahi_data);
    return AHISF_ERROR;
  }
  Z9AXBase->allocating = ahi_data;
  Permit();
  AudioCtrl->ahiac_DriverData = ahi_data;

  ahi_data->mainproc_signal = AllocSignal(-1);
  if (ahi_data->mainproc_signal == -1) {
    KPrintF((CONST_STRPTR)"ZZ9000AX: AllocSignal failed\n");
    goto fail;
  }

  ZZ9KBase = OpenLibrary((STRPTR)"zz9k.library", 0);
  v->reason = ZZ9KBase ? voice_probe(v) : Z9AX_VOICE_REASON_LIBRARY;
  if (v->reason != 0U)
    goto fail;

  Forbid();
  ahi_data->worker_process = CreateNewProcTags(
      NP_Entry, (uint32_t)&VoiceWorkerProcess, NP_Name,
      (uint32_t)device_name, NP_Priority, WORKER_PRIORITY, TAG_DONE);
  if (ahi_data->worker_process)
    ahi_data->worker_process->pr_Task.tc_UserData = ahi_data;
  Permit();
  if (!ahi_data->worker_process) {
    KPrintF((CONST_STRPTR)"ZZ9000AX: CreateNewProcTags failed\n");
    goto fail;
  }

  /* The worker signals once it opened the session, or cleared itself. */
  Wait(1UL << ahi_data->mainproc_signal);
  if (!ahi_data->worker_process)
    goto fail;

  /* The ownership claim of the software path: only a fabric-qualified MHI
   * may coexist, because the voice slot is mixed by the fabric. The
   * interrupt nodes join the shared chain, so they are filled in first. */
  prepare_irq_struct(ahi_data);
  Forbid();
  mhi_mode = mhi_mode_locked();
  if (Z9AXBase->owner || Z9AXBase->allocating != ahi_data ||
      (mhi_mode != Z9AX_MHI_MODE_NONE && mhi_mode != Z9AX_MHI_MODE_FABRIC)) {
    if (Z9AXBase->allocating == ahi_data)
      Z9AXBase->allocating = NULL;
    Permit();
    KPrintF((CONST_STRPTR)"ZZ9000AX: hardware mixing refused: audio hardware"
            " already owned.\n");
    goto fail;
  }
  Z9AXBase->owner = ahi_data;
  Z9AXBase->allocating = NULL;
  install_irq_server_locked(ahi_data);
  Permit();

  AudioCtrl->ahiac_MixFreq = ZZ9K_VOICE_OUTPUT_RATE;
  KPrintF((CONST_STRPTR)"ZZ9000AX: hardware mixing: %lu channels, %s"
          " interpolation, bank %lu bytes, lead %lu frames, ring %lu"
          " records%s.\n",
          (unsigned long)v->channels, v->linear ? "linear" : "nearest",
          (unsigned long)v->session.grant.bank_bytes, (unsigned long)v->lead,
          (unsigned long)v->session.grant.ring_records,
          v->stereo_ok ? ", stereo samples" : "");
  return 0;

fail:
  voice_wait_worker(ahi_data);
  Forbid();
  if (Z9AXBase->allocating == ahi_data)
    Z9AXBase->allocating = NULL;
  Permit();
  if (v->reason != 0U)
    voice_refused(v->reason, v->open_status);
  else
    KPrintF((CONST_STRPTR)"ZZ9000AX: hardware mixing refused: the voice worker"
            " could not start.\n");
  if (ZZ9KBase) {
    CloseLibrary(ZZ9KBase);
    ZZ9KBase = NULL;
  }
  if (ahi_data->mainproc_signal != -1) {
    FreeSignal(ahi_data->mainproc_signal);
    ahi_data->mainproc_signal = -1;
  }
  FreeVec(v);
  FreeVec(ahi_data);
  AudioCtrl->ahiac_DriverData = NULL;
  return AHISF_ERROR;
}

/* FreeAudio for a hardware-mixing mode: the worker writes the counters and
 * closes the session, then the library base and the ownership nodes go. */
static void voice_free_audio(struct AHIAudioCtrlDrv *AudioCtrl)
{
  struct z9ax *ahi_data = AudioCtrl->ahiac_DriverData;

  /* Keeps the interrupt server inert while the worker shuts down. */
  ahi_data->play_stop = 1;
  ahi_data->record_stop = 1;
  voice_wait_worker(ahi_data);
  if (ZZ9KBase) {
    CloseLibrary(ZZ9KBase);
    ZZ9KBase = NULL;
  }
  destroy_interrupt(ahi_data);
  if (ahi_data->mainproc_signal != -1) {
    FreeSignal(ahi_data->mainproc_signal);
    ahi_data->mainproc_signal = -1;
  }
  FreeVec(ahi_data->voice);
  ahi_data->voice = NULL;
  FreeVec(ahi_data);
  AudioCtrl->ahiac_DriverData = NULL;
}

/* Queues one AHI call for the worker. Safe from any context, including
 * interrupts and the client's hooks. */
static void voice_push(struct z9ax *ahi_data, const VmCall *call)
{
  Disable();
  vm_fifo_push(&ahi_data->voice->model, call);
  Enable();
}

/* Copies the rest of an upload into the bank, the way LoadSound hands it
 * over. The session belongs to the worker that opened it and the firmware
 * refuses a request from any other task, so the mailbox requests are the
 * worker's: LoadSound fills the request under upload_lock, wakes the worker
 * and waits for its answer. Bank allocation, the held-sound check and the
 * define stay in the caller. Returns a ZZ9K_STATUS_*. */
static int voice_upload(struct z9ax *ahi_data, uint32_t bank_offset,
                        const uint8_t *data, uint32_t length)
{
  struct z9ax_voice *v = ahi_data->voice;
  struct Task *me = FindTask(NULL);
  struct Task *worker = (struct Task *)ahi_data->worker_process;
  int8_t signal;
  int status = ZZ9K_STATUS_OK;

  if (!worker)
    return ZZ9K_STATUS_BAD_HANDLE;
  if (worker == me) {
    /* The worker is the owner: it copies without a hand-over. */
    while (length != 0U && status == ZZ9K_STATUS_OK) {
      uint32_t done = 0U;

      status = voice_copy_step(v, bank_offset, data, length, &done);
      data += done;
      bank_offset += done;
      length -= done;
    }
    return status;
  }
  signal = AllocSignal(-1);
  if (signal == -1)
    return ZZ9K_STATUS_NO_MEMORY;
  ObtainSemaphore(&v->upload_lock);
  Forbid();
  v->up_status = ZZ9K_STATUS_BAD_HANDLE;
  if (ahi_data->worker_process) {
    v->up_task = me;
    v->up_signal = signal;
    v->up_data = data;
    v->up_length = length;
    v->up_offset = bank_offset;
    v->up_pending = 1U;
    Signal(worker, 1UL << ahi_data->enable_signal);
  }
  Permit();
  while (v->up_pending)
    Wait(1UL << signal);
  status = (int)v->up_status;
  ReleaseSemaphore(&v->upload_lock);
  FreeSignal(signal);
  return status;
}

static void voice_unload_sound(struct z9ax *ahi_data, uint16_t sound)
{
  struct z9ax_voice *v = ahi_data->voice;

  if (sound >= VM_MAX_SOUNDS)
    return;
  Forbid();
  if (v->model.sounds[sound].used == VM_SOUND_LOADED)
    v->unload_pending[sound >> 5] |= 1UL << (sound & 31U);
  Permit();
}

/* AHI_LoadSound: an AHIST_SAMPLE is copied into the card bank once, through
 * the staging buffer, and defined over its range. The client's memory must
 * stay unchanged until AHI_UnloadSound, as for any driver that uploads.
 * AHIST_DYNAMICSAMPLE is refused: the engine plays from its own bank and a
 * snapshot would silently play stale data. */
static uint32_t voice_load_sound(struct z9ax *ahi_data, uint16_t sound,
                                 uint32_t type,
                                 const struct AHISampleInfo *info)
{
  /* AHISampleInfo as the AHI developer <devices/ahi.h> has it: the NDK copy
   * ends after the address. */
  const struct {
    ULONG ahisi_Type;
    APTR ahisi_Address;
    ULONG ahisi_Length;
  } *sample = (const void *)info;
  struct z9ax_voice *v = ahi_data->voice;
  uint32_t view;
  uint32_t bytes;
  uint32_t bank_offset = 0U;
  uint32_t status;
  uint8_t taken;
  uint8_t defined;

  if (type != AHIST_SAMPLE) {
    KPrintF((CONST_STRPTR)"ZZ9000AX: hardware mixing plays AHIST_SAMPLE"
            " sounds only; AHIST_DYNAMICSAMPLE (Play16, ZZPlay AHI output)"
            " needs a 16 bit Stereo mode.\n");
    return AHIE_BADSOUNDTYPE;
  }
  switch (sample->ahisi_Type) {
    case AHIST_M8S:
      view = VM_VIEW_S8;
      break;
    case AHIST_M16S:
      view = VM_VIEW_S16;
      break;
    case AHIST_S8S:
      view = VM_VIEW_S8S;
      break;
    case AHIST_S16S:
      view = VM_VIEW_S16S;
      break;
    default:
      return AHIE_BADSAMPLETYPE;
  }
  if ((view == VM_VIEW_S8S || view == VM_VIEW_S16S) && !v->stereo_ok) {
    KPrintF((CONST_STRPTR)"ZZ9000AX: hardware mixing: stereo samples need"
            " firmware that advertises the stereo voice flag.\n");
    return AHIE_BADSAMPLETYPE;
  }
  if (sound >= v->sounds || sound >= VM_MAX_SOUNDS)
    return AHIE_BADSOUNDTYPE;
  if (sample->ahisi_Address == NULL || sample->ahisi_Length == 0U)
    return AHIE_BADSAMPLETYPE;
  if (((uint64_t)sample->ahisi_Length <<
       zz9k_voice_format_frame_shift(view)) >
      (uint64_t)v->session.grant.bank_bytes) {
    KPrintF((CONST_STRPTR)"ZZ9000AX: hardware mixing: sound %ld does not fit"
            " the %lu byte sample bank.\n",
            (long)sound, (unsigned long)v->session.grant.bank_bytes);
    return AHIE_NOMEM;
  }
  bytes = sample->ahisi_Length << zz9k_voice_format_frame_shift(view);
  /* A sound that is still loaded is replaced, as ahi.device replaces its
   * own entry; a voice that plays it stops first. */
  voice_unload_sound(ahi_data, sound);
  voice_run_unloads(ahi_data);
  Forbid();
  vm_bank_poll(&v->model, v->status.horizon);
  if (vm_sound_held(&v->model, sound)) {
    Permit();
    KPrintF((CONST_STRPTR)"ZZ9000AX: hardware mixing: sound %ld was unloaded"
            " and its bank range is still in use by the card.\n",
            (long)sound);
    return AHIE_NOMEM;
  }
  taken = vm_bank_alloc(&v->model, bytes, &bank_offset) == 0 ? 1U : 0U;
  Permit();
  if (!taken) {
    KPrintF((CONST_STRPTR)"ZZ9000AX: hardware mixing: the sample bank is"
            " full (%lu bytes free); ENV:ZZ9K_AHI_BANK (KiB) raises the"
            " request.\n",
            (unsigned long)vm_bank_free_bytes(&v->model));
    return AHIE_NOMEM;
  }
  status = (uint32_t)voice_upload(ahi_data, bank_offset,
                                  (const uint8_t *)sample->ahisi_Address,
                                  bytes);
  if (status != ZZ9K_STATUS_OK) {
    Forbid();
    vm_bank_release(&v->model, bank_offset, bytes);
    Permit();
    KPrintF((CONST_STRPTR)"ZZ9000AX: hardware mixing: sample upload failed"
            " (status %ld).\n",
            (long)status);
    return AHIE_UNKNOWN;
  }
  Forbid();
  defined = vm_sound_define(&v->model, sound, sample->ahisi_Type, bank_offset,
                            sample->ahisi_Length) == 0 ? 1U : 0U;
  if (!defined)
    vm_bank_release(&v->model, bank_offset, bytes);
  Permit();
  return defined ? AHIE_OK : AHIE_BADSAMPLETYPE;
}

/* AHI_SetEffect: the two effects a hardware-mixing driver can give. Any
 * other effect is AHIE_UNKNOWN, as the Paula driver answers in its DMA
 * mode; a cancel of an effect that was never set is AHIE_OK. */
static uint32_t voice_set_effect(struct z9ax *ahi_data, uint8_t *effect)
{
  struct z9ax_voice *v = ahi_data->voice;
  uint32_t kind = *(uint32_t *)effect;
  VmCall call;

  if (kind == AHIET_MASTERVOLUME || kind == (AHIET_MASTERVOLUME | AHIET_CANCEL)) {
    memset(&call, 0, sizeof(call));
    call.kind = VM_CALL_MASTER;
    call.a = kind == AHIET_MASTERVOLUME
                 ? (uint32_t)((struct AHIEffMasterVolume *)effect)->ahiemv_Volume
                 : 0x10000U;
    voice_push(ahi_data, &call);
    return AHIE_OK;
  }
  if (kind == AHIET_CHANNELINFO || kind == (AHIET_CHANNELINFO | AHIET_CANCEL)) {
    Forbid();
    v->channelinfo =
        kind == AHIET_CHANNELINFO ? (struct AHIEffChannelInfo *)effect : NULL;
    Permit();
    return AHIE_OK;
  }
  return AHIE_UNKNOWN;
}

static uint32_t __attribute__((used)) intAHIsub_AllocAudio(struct TagItem *tagList asm("a1"), struct AHIAudioCtrlDrv *AudioCtrl asm("a2")) {
  // TW: Just take the values from where init() has already stored them.
  uint32_t hw_addr = Z9AXBase->hw_addr;
  int zorro = Z9AXBase->zorro_version;
  if(!hw_addr) return AHISF_ERROR; // TW: Only AHISF_xxx return codes are allowed here.
  if(!zorro) return AHISF_ERROR; // TW: Only AHISF_xxx return codes are allowed here.
  /* A hardware-mixing mode takes its own path: no mixing, no timing. */
  {
    uint8_t vmode = voice_mode_of(tagList);

    if (vmode != Z9AX_VOICE_NONE)
      return voice_alloc_audio(vmode, AudioCtrl);
  }

  /* Fast rejection for the normal second-open case. The reservation below
   * closes the blocking control-plane TOCTOU window; this check avoids doing
   * allocations for an owner that is already established. */
  Forbid();
  if (Z9AXBase->owner || Z9AXBase->allocating) {
    Permit();
    return AHISF_ERROR;
  }
  Permit();

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
    int descriptor_verified = 0;
    uint32_t descriptor = zz9000_read_z2_aperture_verified(
        hw_addr, Z9AXBase->hw_size, &fw_caps, &descriptor_verified, NULL);
    enum ZZApertureNegotiation status = zz_z2_aperture_negotiate(
        descriptor, Z9AXBase->hw_size, fw_caps, &layout);

    /* Fail closed on an unconfirmed descriptor as well: a wrong
     * generation could carve the direct-ring reservation over the
     * live host window. */
    if (!descriptor_verified ||
        status == ZZ_APERTURE_INVALID ||
        (status == ZZ_APERTURE_VALID &&
         layout.audio.size != ZZ_Z2_AUDIO_SIZE)) {
      if (record_buf) FreeVec(record_buf);
      FreeVec(lease_accum);
      FreeVec(audio_buf);
      FreeVec(ahi_data);
      return AHISF_ERROR;
    }
    if (status == ZZ_APERTURE_VALID)
      offset_tx = zz_aperture_memory_offset(layout.audio.base);
    if (status == ZZ_APERTURE_VALID &&
        (layout.descriptor & ZZ_Z2_APERTURE_INFO_GENERATION_MASK) ==
            ZZ_Z2_APERTURE_INFO_GENERATION_2) {
      uint32_t direct_base =
          layout.host_window.base + layout.host_window.size;
      uint32_t direct_size = layout.audio.base - direct_base;

      if (direct_size == ZZ_Z2_DIRECT_RING_RESERVE_SIZE) {
        ahi_data->z2_direct_ring_base = direct_base;
        ahi_data->z2_direct_ring_size = direct_size;
      }
    }
  }
  uint32_t offset_rx = offset_tx + ZZ_AX_RX_BUFFER_DELTA;
  ahi_data->audio_hw_buf_addr = hw_addr + 0x10000 + offset_tx;
  ahi_data->audio_rx_hw_buf_addr = hw_addr + 0x10000 + offset_rx;

  /* Serialize AllocAudio before touching the driver-global zz9k.library
   * base or control-plane capability state. A rejected peer must not close
   * or clear the active owner's globals. */
  Forbid();
  if (Z9AXBase->owner || Z9AXBase->allocating) {
    Permit();
    if (record_buf) FreeVec(record_buf);
    FreeVec(lease_accum);
    FreeVec(audio_buf);
    FreeVec(ahi_data);
    return AHISF_ERROR;
  }
  Z9AXBase->allocating = ahi_data;
  Permit();
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
        (uint8_t)(fabric_rate_capped() &&
                  (zorro != 2 ||
                   ahi_data->z2_direct_ring_size ==
                       ZZ_Z2_DIRECT_RING_RESERVE_SIZE));
  }
  // Atomic ownership claim: reject another low-level AHI allocation and
  // any MHI owner that cannot prove fabric compatibility before touching
  // shared hardware. AHI's low-level API is exclusive; full duplex is
  // AHISF_PLAY|AHISF_RECORD on one AudioCtrl, not two independent
  // AudioCtrls. Publishing owner and installing the ISR under the same
  // Forbid closes both AHI/AHI and AHI/MHI TOCTOU windows. Only two
  // qualified fabric tokens may coexist; legacy/current-fallback MHI
  // retains the classic exclusion. The HW-side interrupt stays OFF until
  // the worker is up; Start() publishes the requested direction mask.
  uint8_t mhi_mode;
  prepare_irq_struct(ahi_data);
  Forbid();
  mhi_mode = mhi_mode_locked();
  if (Z9AXBase->owner || Z9AXBase->allocating != ahi_data ||
      (mhi_mode != Z9AX_MHI_MODE_NONE &&
       (!ahi_data->fabric_mode || mhi_mode != Z9AX_MHI_MODE_FABRIC))) {
    if (Z9AXBase->allocating == ahi_data)
      Z9AXBase->allocating = NULL;
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
  Z9AXBase->allocating = NULL;
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

  // Wait for the worker's handshake: it signals only after its early
  // init is done -- signal allocation, and in fabric mode the pacing
  // timer port/request/device with the first request posted.
  Wait(1L << ahi_data->mainproc_signal);

  // Worker may have failed to allocate its signals or bring up the
  // fabric pacing timer; it clears itself in that case.
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
  // or the worker signalled back with worker_process cleared (signal
  // alloc failed, or the fabric pacing timer could not be opened). We
  // must never reach fail: with a live worker, otherwise it would be
  // orphaned.
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
  if (((struct z9ax *)AudioCtrl->ahiac_DriverData)->voice) {
    voice_free_audio(AudioCtrl);
    return;
  }

  struct z9ax *ahi_data = AudioCtrl->ahiac_DriverData;
  uint32_t release_attempt;

  // Make sure the worker's mix loop won't try to touch hardware after we tear down.
  ahi_data->play_stop = 1;
  ahi_data->record_stop = 1;

  /* Surrender while the worker still owns timer.device, because a
   * failed mailbox completion records an elapsed-time retry deadline.
   * Retry the idempotent generation a bounded number of times before
   * joining the worker; an unavailable firmware will still revoke the
   * now-heartbeatless generation after teardown. */
  if (ahi_data->fabric_mode) {
    for (release_attempt = 0U;
         release_attempt < 3U &&
             (ahi_data->lease_held ||
              ahi_data->lease_release_pending);
         release_attempt++)
      fabric_lease_release(ahi_data);
  }

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
  if (ahi_data->voice) {
    if (Flags & AHISF_PLAY)
      voice_command(ahi_data, Z9AX_VOICE_CMD_STOP);
    return;
  }

  stop_play = (Flags & AHISF_PLAY) != 0;
  if (Flags & (AHISF_PLAY | AHISF_RECORD)) {
    Forbid();
    if (Flags & AHISF_PLAY) {
      ahi_data->play_stop = 1;
      ahi_data->play_transport_generation++;
      ahi_data->buf_offset = 0;
      ahi_data->player_frames_left = 0U;
      ahi_data->player_remainder = 0U;
      ahi_data->player_freq = 0U;
    }
    if (Flags & AHISF_RECORD) ahi_data->record_stop = 1;
    update_hw_interrupts(ahi_data);
    Permit();
  }

  if (stop_play) {
    if (ahi_data->fabric_mode) {
      /* Surrender the lease so neither its staged accumulator period
       * nor its ring runway can survive Stop(PLAY). Start() acquires a
       * fresh paused lease and re-primes it from the new AHI timeline.
       * play_stop was set before Permit(), so the worker cannot stage
       * more PCM while this blocking mailbox release runs. */
      ahi_data->lease_accum_fill = 0U;
      fabric_lease_release(ahi_data);
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
  uint32_t start_generation = 0U;
  int acquire_needed = 0;
  uint32_t result = AHIE_OK;
  if (!ahi_data) return AHIE_OK;
  if (ahi_data->voice)
    return (flags & AHISF_PLAY) ?
        voice_command(ahi_data, Z9AX_VOICE_CMD_START) : AHIE_OK;

  if ((flags & AHISF_PLAY) && ahi_data->fabric_mode) {
    Forbid();
    start_generation = ahi_data->play_transport_generation;
    if (ahi_data->lease_release_pending ||
        ahi_data->lease_release_in_progress ||
        ahi_data->lease_acquire_uncertain ||
        ahi_data->lease_acquire_in_progress) {
      result = AHIE_UNKNOWN;
    } else if (!ahi_data->lease_held) {
      ahi_data->lease_acquire_in_progress = 1U;
      acquire_needed = 1;
    }
    Permit();
    /* Lease mode: acquire at every play Start after Stop surrendered
     * the old timeline (or after a mid-session revocation zeroed
     * lease_held), and let the credit-paced worker take it from here.
     * Only one blocking acquire may install the shared session. */
    if (acquire_needed) {
      if (!fabric_lease_acquire(ahi_data,
                                AudioCtrl->ahiac_MixFreq)) {
        KPrintF((CONST_STRPTR)"ZZ9000AX: fabric lease refused at "
                "%lu Hz; Start(PLAY) fails.\n",
                (unsigned long)AudioCtrl->ahiac_MixFreq);
        result = AHIE_UNKNOWN;
        Forbid();
        ahi_data->lease_acquire_in_progress = 0U;
        Permit();
      }
    }
    if (result == AHIE_OK) {
      Forbid();
      if (ahi_data->play_transport_generation != start_generation) {
        Permit();
        /* Stop completed while Start was inspecting or acquiring the
         * lease. Keep other acquirers excluded until this stale grant
         * is surrendered, and do not clear play_stop. */
        fabric_lease_release(ahi_data);
        Forbid();
        if (acquire_needed)
          ahi_data->lease_acquire_in_progress = 0U;
        Permit();
        result = AHIE_UNKNOWN;
      } else {
        ahi_data->buf_offset = 0;
        ahi_data->player_frames_left = 0U;
        ahi_data->player_remainder = 0U;
        ahi_data->player_freq = 0U;
        /* PAUSED stays as acquired: the worker clears it once two
         * periods are staged (primed-ring prefill). */
        ahi_data->play_stop = 0;
        if (acquire_needed)
          ahi_data->lease_acquire_in_progress = 0U;
        update_hw_interrupts(ahi_data);
        Permit();
      }
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
    ahi_data->player_frames_left = 0U;
    ahi_data->player_remainder = 0U;
    ahi_data->player_freq = 0U;
    ahi_data->play_sequence = play_sequence;
    ahi_data->play_stop = 0;
    update_hw_interrupts(ahi_data);
    Permit();
  }

  if ((flags & AHISF_RECORD) && ahi_data->record_capable &&
      (!(flags & AHISF_PLAY) || result == AHIE_OK)) {
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
              AudioCtrl->ahiac_MixFreq / 50U);
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
  /* A hardware-mixing mode runs at the engine rate, offers the engine's
   * voices as channels and does not record. Its tag list is the only thing
   * GetAttr can rely on before AllocAudio. */
  BOOL voice = voice_mode_of(tagList) != Z9AX_VOICE_NONE;
  BOOL can_record =
      !voice && Z9AXBase && recording_supported(Z9AXBase->hw_addr);

  // TW: We can't rely on AudioCtrl->ahiac_DriverData being valid during this function call!

  switch(attr)
    {
    case AHIDB_Bits:
      return 16;
    case AHIDB_Frequencies:
      return voice ? 1 : ZZ_NUM_FREQS;
    case AHIDB_Frequency:
      if (voice)
        return arg == 0 ? (int32_t)ZZ9K_VOICE_OUTPUT_RATE : def;
      if (arg < 0 || arg >= ZZ_NUM_FREQS)
        return def;
      return freqs[arg];
    case AHIDB_Index:
      if (voice)
        return 0;
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
      // Ceiling of the AHI Prefs channel selector; see
      // ZZ_AX_AHI_MAX_CHANNELS for why this is a CPU budget, not a
      // hardware channel count (the card always gets one stereo stream).
      // In a hardware-mixing mode every AHI channel is a voice of the card.
      return voice ? (int32_t)ZZ9K_VOICE_MAX_VOICES : ZZ_AX_AHI_MAX_CHANNELS;
    case AHIDB_MaxPlaySamples:
      // Frames per individual MixerFunc call, not per player tick.
      // Slow ticks span multiple calls without enlarging the bounce buffer.
      // In a hardware-mixing mode there is no mixing buffer: its ceiling
      // is the sample bank.
      return voice ? (int32_t)ZZ9K_VOICE_BANK_MAX_BYTES : BOUNCE_MAX_FRAMES;
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
  struct z9ax *ahi_data = AudioCtrl->ahiac_DriverData;

  if (!ahi_data || !ahi_data->voice)
    return AHIS_UNKNOWN;
  return voice_set_effect(ahi_data, effect);
}

static uint32_t __attribute__((used)) intAHIsub_LoadSound(uint16_t sound asm("d0"), uint32_t type asm("d1"), struct AHISampleInfo *info asm("a0"), struct AHIAudioCtrlDrv *AudioCtrl asm("a2"))
{
  struct z9ax *ahi_data = AudioCtrl->ahiac_DriverData;

  if (!ahi_data || !ahi_data->voice)
    return AHIS_UNKNOWN;
  return voice_load_sound(ahi_data, sound, type, info);
}

static uint32_t __attribute__((used)) intAHIsub_UnloadSound(uint16_t sound asm("d0"), struct AHIAudioCtrlDrv *AudioCtrl asm("a2"))
{
  struct z9ax *ahi_data = AudioCtrl->ahiac_DriverData;

  if (ahi_data && ahi_data->voice)
    voice_unload_sound(ahi_data, sound);
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
  /* Worker reads the live hooks and PlayerFreq at each player boundary. */
}

static uint32_t __attribute__((used)) intAHIsub_SetVol(uint16_t channel asm("d0"), uint32_t volume asm("d1"), uint32_t pan asm("d2"), struct AHIAudioCtrlDrv *AudioCtrl asm("a2"), uint32_t flags asm("d3"))
{
  struct z9ax *ahi_data = AudioCtrl->ahiac_DriverData;
  VmCall call;

  if (!ahi_data || !ahi_data->voice)
    return AHIS_UNKNOWN;
  memset(&call, 0, sizeof(call));
  call.kind = VM_CALL_VOL;
  call.imm = (uint8_t)((flags & AHISF_IMM) != 0U);
  call.channel = channel;
  call.a = volume;
  call.b = (int32_t)pan;
  voice_push(ahi_data, &call);
  return AHIS_UNKNOWN;
}

static uint32_t __attribute__((used)) intAHIsub_SetFreq(uint16_t channel asm("d0"), uint32_t freq asm("d1"), struct AHIAudioCtrlDrv *AudioCtrl asm("a2"), uint32_t flags asm("d2"))
{
  struct z9ax *ahi_data = AudioCtrl->ahiac_DriverData;
  VmCall call;

  if (!ahi_data || !ahi_data->voice)
    return AHIS_UNKNOWN;
  memset(&call, 0, sizeof(call));
  call.kind = VM_CALL_FREQ;
  call.imm = (uint8_t)((flags & AHISF_IMM) != 0U);
  call.channel = channel;
  /* AHI_MIXFREQ is the mode's rate, which is the engine rate here. */
  call.a = freq == AHI_MIXFREQ ? ZZ9K_VOICE_OUTPUT_RATE : freq;
  voice_push(ahi_data, &call);
  return AHIS_UNKNOWN;
}

static uint32_t __attribute__((used)) intAHIsub_SetSound(uint16_t channel asm("d0"), uint16_t sound asm("d1"), uint32_t offset asm("d2"), int32_t length asm("d3"), struct AHIAudioCtrlDrv *AudioCtrl asm("a2"), uint32_t flags asm("d4"))
{
  struct z9ax *ahi_data = AudioCtrl->ahiac_DriverData;
  VmCall call;

  if (!ahi_data || !ahi_data->voice)
    return AHIS_UNKNOWN;
  memset(&call, 0, sizeof(call));
  call.kind = VM_CALL_SOUND;
  call.imm = (uint8_t)((flags & AHISF_IMM) != 0U);
  call.channel = channel;
  call.sound = sound;
  call.a = offset;
  call.b = length;
  voice_push(ahi_data, &call);
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
