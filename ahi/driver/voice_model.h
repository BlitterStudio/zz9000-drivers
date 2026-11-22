/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * AHI channel model for the ZZ9000AX hardware-mixing AudioModes.
 *
 * ahi.device does no mixing in these modes: it hands every AHI_SetSound,
 * AHI_SetFreq and AHI_SetVol call to the driver. This module turns those
 * calls into timestamped voice engine records (zz9k/voice.h) and predicts,
 * with the engine's own integer arithmetic, the engine frame at which each
 * pass of a sound ends, so the driver can call SoundFunc at that point of the
 * timeline and keep ahi.device's rules: a sound repeats until replaced, a
 * call without AHISF_IMM takes effect when the current pass ends, frequency
 * 0 pauses a channel.
 *
 * The module is pure C. It takes no locks and calls no AHI hooks itself: the
 * driver passes callbacks, drains its call FIFO under Disable(), and owns the
 * ring. Everything here runs on the one worker process, except
 * vm_fifo_push(), which the driver calls from any context under Disable().
 *
 * Positions are view frames. The driver defines four engine samples over the
 * whole card bank, one per sample type; a sound is a bank byte range inside
 * a view, and START and LOOP address it as the view plus a byte offset.
 */

#ifndef VOICE_MODEL_H
#define VOICE_MODEL_H

#include <stddef.h>
#include <stdint.h>

#include "zz9k/voice.h"

#define VM_MAX_CHANNELS 32U
#define VM_MAX_SOUNDS   256U
#define VM_FIFO_SIZE    256U
#define VM_OUT_RECORDS  2048U
#define VM_MAX_FREE_RANGES 64U
#define VM_MAX_QUARANTINE  64U

/* A queued tick or boundary walk gives up on a channel after this many pass
 * boundaries in one window, so a client that asks for one-frame passes cannot
 * hold the worker forever. The count is in vm->overload. */
#define VM_MAX_BOUNDARIES_PER_TICK 512U

/* Engine sample ids of the four whole-bank views, indexed by view. */
#define VM_VIEW_S8   0U
#define VM_VIEW_S16  1U
#define VM_VIEW_S8S  2U
#define VM_VIEW_S16S 3U
#define VM_VIEWS     4U

/* AHI sound types as they come in AHISampleInfo (devices/ahi.h). */
#define VM_AHI_M8S  0U
#define VM_AHI_M16S 1U
#define VM_AHI_S8S  2U
#define VM_AHI_S16S 3U

#define VM_NOSOUND 0xFFFFU

/* Call kinds queued by the AHIsub_Set* entry points. */
#define VM_CALL_SOUND 1U
#define VM_CALL_FREQ  2U
#define VM_CALL_VOL   3U
#define VM_CALL_MASTER 4U /* AHIET_MASTERVOLUME, a = Fixed volume */

#define VM_MASTER_MAX_GAIN 0xFFFFU /* the engine's Q8.8 ceiling */

typedef struct VmCall {
  uint8_t kind;
  uint8_t imm;
  uint16_t channel;
  uint16_t sound;     /* SOUND: sound id or VM_NOSOUND */
  uint16_t pad;
  uint32_t a;         /* SOUND: offset, FREQ: Hz, VOL: volume (Fixed) */
  int32_t b;          /* SOUND: length, VOL: pan (Fixed) */
} VmCall;

/* What the model needs from the driver. Hooks run on the worker. */
typedef struct VmHooks {
  void (*player)(void *user);                     /* PlayerFunc, may be NULL */
  void (*sound)(void *user, uint16_t channel);    /* SoundFunc, may be NULL */
  void (*lock)(void *user);                       /* Disable(), may be NULL */
  void (*unlock)(void *user);                     /* Enable(), may be NULL */
  /* True while the client holds AHIsub_Disable(): the FIFO is not drained,
   * so a PlayA sequence is never split. May be NULL. */
  int (*blocked)(void *user);
  /* AHIA_PlayerFreq (Fixed, Hz), read after PlayerFunc. NULL or 0 is 50 Hz. */
  uint32_t (*player_freq)(void *user);
  void *user;
} VmHooks;

typedef struct VmConfig {
  uint32_t channels;        /* 1..32 */
  uint32_t sounds;          /* AHIA_Sounds */
  uint32_t bank_bytes;      /* granted bank size */
  uint32_t stereo_views;    /* S8S and S16S views are defined */
  VmHooks hooks;
} VmConfig;

typedef struct VmSound {
  uint8_t used;
  uint8_t view;
  uint32_t bank;      /* byte offset in the bank (4-byte aligned) */
  uint32_t frames;    /* length in sample frames */
} VmSound;

/* One AHI-visible pass of a channel: what ahi.device keeps as the current
 * and the next sound. lo and n are view frames. */
typedef struct VmPass {
  uint8_t sound_ok;
  uint8_t freq_ok;
  uint8_t view;
  uint8_t backward;
  uint16_t sound;
  uint32_t base;      /* view frame of the sound start (CHANNELINFO base) */
  uint32_t lo;        /* first frame of the region */
  uint32_t n;         /* frames in the region */
  uint32_t hz;
  uint16_t vl, vr;    /* engine volumes, from vol and pan with the master */
  int32_t vol;        /* AHI_SetVol volume and pan as given (Fixed) */
  int32_t pan;
} VmPass;

typedef struct VmChannel {
  VmPass cur;
  VmPass next;
  uint8_t eos;          /* a pass started and SoundFunc is owed */
  uint8_t dirty;        /* calls applied since the last reconcile */
  /* Engine mirror. pos is valid at frame t0, the next frame to render. */
  uint8_t e_active;
  uint8_t e_bw;
  uint8_t e_view;
  uint8_t e_has_view;
  uint32_t e_end;       /* forward pass end, view frames */
  uint32_t e_ls;        /* programmed loop start, view frames */
  uint32_t e_ll;        /* programmed loop length, view frames, 0 = none */
  uint32_t e_hz;        /* programmed rate, 0 = none */
  uint32_t e_step;
  uint32_t e_vl, e_vr;
  uint8_t e_vol_valid;
  uint32_t e_idx;
  uint32_t e_frac;
  uint32_t e_t0;
  uint32_t e_bnd;       /* absolute frame of the next pass boundary */
  uint8_t e_bnd_valid;
} VmChannel;

typedef struct VmFree {
  uint32_t off;
  uint32_t len;
} VmFree;

typedef struct VmQuarantine {
  uint32_t off;
  uint32_t len;
  uint32_t stamp;     /* free once the engine horizon passes this frame */
} VmQuarantine;

typedef struct VmModel {
  VmConfig cfg;
  uint32_t master_m16;    /* AHIET_MASTERVOLUME, Fixed */
  uint32_t master_gain;   /* last Q8.8 gain sent or to send */
  uint8_t running;
  uint8_t master_sent;
  uint32_t now;           /* frame: every boundary before it is processed */
  uint64_t t;             /* tick clock, 16.16 frames */
  uint32_t last_stamp;

  VmSound sounds[VM_MAX_SOUNDS];
  VmChannel ch[VM_MAX_CHANNELS];

  /* Call FIFO: single producer side under Disable(), consumed on the
   * worker with the same protection. */
  VmCall fifo[VM_FIFO_SIZE];
  uint32_t fifo_head;
  uint32_t fifo_tail;
  uint32_t fifo_drops;

  /* Records waiting for the ring, in nondecreasing stamp order. */
  ZZ9KVoiceRecord out[VM_OUT_RECORDS];
  uint32_t out_n;
  uint32_t out_total;     /* records produced */
  uint32_t out_drops;     /* records lost to a full buffer */
  uint32_t out_tick;      /* start of the unsorted tail of out[] */

  uint32_t overload;      /* walks cut short by VM_MAX_BOUNDARIES_PER_TICK */
  uint32_t ticks;
  uint32_t sound_stamp;   /* pass start of the SoundFunc call in progress */

  /* Bank allocator. */
  VmFree free_list[VM_MAX_FREE_RANGES];
  uint32_t free_n;
  VmQuarantine quarantine[VM_MAX_QUARANTINE];
  uint32_t quarantine_n;
} VmModel;

/* Level mapping (KTD7). */
void vm_volume_to_engine(int32_t volume, int32_t pan, uint16_t *left,
                         uint16_t *right);
uint32_t vm_master_gain(uint32_t master_m16, uint32_t channels);
/* The engine volumes of one channel for AHI_SetVol(volume, pan) under the
 * given AHIET_MASTERVOLUME. The engine gain (master_gain above) is a whole
 * number of 1/256, so the voice volumes carry the rounding residual and the
 * sum is ahi.device's to within a rounding step for every channel count. */
void vm_levels(uint32_t master_m16, uint32_t channels, int32_t volume,
               int32_t pan, uint16_t *left, uint16_t *right);

/* Tick length in 16.16 engine frames for a PlayerFreq of freq16 (Hz, 16.16).
 * Zero or an absurd rate gives a 50 Hz tick. */
uint64_t vm_tick_frames16(uint32_t freq16);

/* Setup. cfg is copied. */
void vm_init(VmModel *m, const VmConfig *cfg);

/* Bank allocator and sound table. vm_alloc returns 0 and the byte offset, or
 * -1 when no range fits. Sizes round up to 4 bytes. */
int vm_bank_alloc(VmModel *m, uint32_t bytes, uint32_t *offset);
/* Gives back a range vm_bank_alloc handed out when the upload failed before
 * the sound was defined. */
void vm_bank_release(VmModel *m, uint32_t offset, uint32_t bytes);
void vm_bank_poll(VmModel *m, uint32_t horizon);
/* Defines sound id over the bank range [bank, bank + frames * frame size).
 * Returns 0, or -1 for a bad id, type or view. */
int vm_sound_define(VmModel *m, uint32_t id, uint32_t ahi_type,
                    uint32_t bank, uint32_t frames);
/* Frees the sound at once from the AHI side: channels using it fall silent
 * (STOP at the model time) and the bank range is held back until the engine
 * horizon passes that stamp. Returns 0, or -1 for an unknown id. */
int vm_sound_remove(VmModel *m, uint32_t id);
uint32_t vm_bank_free_bytes(const VmModel *m);

/* Producer side, any context, under Disable(). Returns 0, or -1 and counts a
 * drop when the FIFO is full. */
int vm_fifo_push(VmModel *m, const VmCall *call);
uint32_t vm_fifo_pending(const VmModel *m);

/* Playback control. vm_start sets the first stamp (the granted lead on the
 * first start, the playing frame plus the lead after a stop) and emits the
 * MASTER record and every channel that was sounding. vm_stop captures the
 * state; the driver resets the engine session. */
void vm_start(VmModel *m, uint32_t stamp);
void vm_stop(VmModel *m);

/* AHIET_MASTERVOLUME goes through the call FIFO as VM_CALL_MASTER. */

/* The worker's work, in this order every loop:
 *  - vm_run_async: drain calls from other contexts at the model time and run
 *    owed SoundFunc calls.
 *  - vm_tick: one PlayerFunc period; the length of the period follows the
 *    PlayerFreq the player_freq hook returns after PlayerFunc ran.
 * Both append to the record buffer and keep it sorted. */
void vm_run_async(VmModel *m);
void vm_tick(VmModel *m);

/* True when the tick that would run next is within run_ahead frames of the
 * playing frame (KTD5): its stamp must stay a lead ahead of the horizon. */
int vm_tick_due(const VmModel *m, uint32_t playing_frame,
                uint32_t run_ahead);

/* Records waiting for the ring. vm_records_taken drops the first n. */
const ZZ9KVoiceRecord *vm_records(const VmModel *m, uint32_t *count);
void vm_records_taken(VmModel *m, uint32_t n);

/* AHIET_CHANNELINFO: the playing offset of every channel, in sample frames
 * from the start of its sound, at the model time. */
void vm_channel_offsets(const VmModel *m, uint32_t *offsets, uint32_t count);

#endif /* VOICE_MODEL_H */
