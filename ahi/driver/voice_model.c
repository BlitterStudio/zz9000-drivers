/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * AHI channel model for the ZZ9000AX hardware-mixing AudioModes. See
 * voice_model.h for the contract and docs/ahi-voice-mixing-spec.md for the
 * mapping from AHI calls to engine records.
 *
 * The code keeps two pictures of each channel. The AHI picture is what
 * ahi.device keeps in software mode: the current pass, the next pass that
 * replaces it when the current one ends, and the end-of-sound flag that owes
 * the client a SoundFunc call. The engine picture mirrors the card's voice
 * with the engine's own integer arithmetic (zz9k/voice.h): the position as
 * an index plus a 16-bit fraction at a known frame, the rate, the pass end
 * and the programmed loop. The engine wraps into its programmed loop on its
 * own, so a repeating sound costs no records. The model finds each wrap
 * frame ahead of time, gives the client its SoundFunc call there, and sends
 * a record only when the client changed something.
 */

#include "voice_model.h"

#define VM_FRAC_ONE 0x10000U

static int32_t vm_sdiff(uint32_t a, uint32_t b)
{
  return (int32_t)(a - b);
}

static void vm_zero(void *p, size_t n)
{
  uint8_t *b = (uint8_t *)p;

  while (n--) {
    *b++ = 0U;
  }
}

static uint32_t vm_view_shift(uint32_t view)
{
  return zz9k_voice_format_frame_shift(view);
}

/* ---- Level mapping ---- */

/* ahi.device's pan law (sound.c) in 16.16: left = (v/2) * ((1 - pan)/2) << 2.
 * Negative volume (invert) and negative pan (surround) use the absolute
 * value, which the autodoc allows for a driver that cannot do either. */
static void vm_ideal_levels(int32_t volume, int32_t pan, uint32_t *left,
                            uint32_t *right)
{
  uint32_t v = volume < 0 ? (uint32_t)(-(int64_t)volume) : (uint32_t)volume;
  uint32_t p = pan < 0 ? (uint32_t)(-(int64_t)pan) : (uint32_t)pan;

  if (v > 0x10000U) {
    v = 0x10000U;
  }
  if (p > 0x10000U) {
    p = 0x10000U;
  }
  *left = ((v >> 1) * ((0x10000U - p) >> 1)) >> 14;
  *right = ((v >> 1) * (p >> 1)) >> 14;
}

void vm_volume_to_engine(int32_t volume, int32_t pan, uint16_t *left,
                         uint16_t *right)
{
  uint32_t l;
  uint32_t r;

  vm_ideal_levels(volume, pan, &l, &r);
  *left = (uint16_t)(l > 0xFFFFU ? 0xFFFFU : l);
  *right = (uint16_t)(r > 0xFFFFU ? 0xFFFFU : r);
}

/* The ideal master gain in 1/65536 of Q8.8 units: master / (channels * 256)
 * in Q8.8 is master * 256 / channels in this scale. */
static uint64_t vm_gain_hp(uint32_t master_m16, uint32_t channels)
{
  if (channels == 0U) {
    channels = 1U;
  }
  return ((uint64_t)master_m16 << 8) / channels;
}

uint32_t vm_master_gain(uint32_t master_m16, uint32_t channels)
{
  uint64_t hp = vm_gain_hp(master_m16, channels);
  /* Rounded up: the voices then carry a residual of at most 1, so a loud
   * voice never clips against the 16-bit volume limit. */
  uint32_t gain = (uint32_t)((hp + 0xFFFFU) >> 16);

  if (gain == 0U && master_m16 != 0U) {
    gain = 1U;
  }
  if (gain > VM_MASTER_MAX_GAIN) {
    gain = VM_MASTER_MAX_GAIN;
  }
  return gain;
}

void vm_levels(uint32_t master_m16, uint32_t channels, int32_t volume,
               int32_t pan, uint16_t *left, uint16_t *right)
{
  uint64_t hp = vm_gain_hp(master_m16, channels);
  uint32_t gain = vm_master_gain(master_m16, channels);
  uint32_t l;
  uint32_t r;
  uint64_t factor;

  vm_ideal_levels(volume, pan, &l, &r);
  if (gain == 0U) {
    *left = 0U;
    *right = 0U;
    return;
  }
  /* The engine's gain is a whole number of 1/256. The voices carry the
   * rounding residual, so the mix is as ahi.device's to within a rounding
   * step, whatever the channel count. */
  factor = hp / gain;
  l = (uint32_t)(((uint64_t)l * factor) >> 16);
  r = (uint32_t)(((uint64_t)r * factor) >> 16);
  *left = (uint16_t)(l > 0xFFFFU ? 0xFFFFU : l);
  *right = (uint16_t)(r > 0xFFFFU ? 0xFFFFU : r);
}

uint64_t vm_tick_frames16(uint32_t freq16)
{
  uint64_t len;

  if (freq16 == 0U) {
    freq16 = 50U << 16;
  }
  len = ((uint64_t)ZZ9K_VOICE_OUTPUT_RATE << 32) / freq16;
  if (len < ((uint64_t)16U << 16)) {
    len = (uint64_t)16U << 16;
  }
  if (len > ((uint64_t)96000U << 16)) {
    len = (uint64_t)96000U << 16;
  }
  return len;
}

/* ---- Record buffer ---- */

static void vm_emit(VmModel *m, uint32_t stamp, uint32_t voice, uint32_t op,
                    uint32_t flags, uint32_t a0, uint32_t a1)
{
  ZZ9KVoiceRecord *r;

  if (m->out_n >= VM_OUT_RECORDS) {
    m->out_drops++;
    return;
  }
  r = &m->out[m->out_n++];
  zz9k_voice_build_record(r, stamp, (uint8_t)voice, (uint8_t)op,
                          (uint16_t)flags, a0, a1);
  m->out_total++;
}

/* Sorts the records of the current tick by stamp (stable, wrap-aware) and
 * lifts any stamp that landed below what the ring already holds. */
static void vm_flush_tick(VmModel *m)
{
  uint32_t i;
  uint32_t j;
  uint32_t floor_stamp = m->last_stamp;

  for (i = m->out_tick + 1U; i < m->out_n; i++) {
    ZZ9KVoiceRecord key = m->out[i];

    j = i;
    while (j > m->out_tick && vm_sdiff(m->out[j - 1U].stamp, key.stamp) > 0) {
      m->out[j] = m->out[j - 1U];
      j--;
    }
    m->out[j] = key;
  }
  for (i = m->out_tick; i < m->out_n; i++) {
    if (vm_sdiff(m->out[i].stamp, floor_stamp) < 0) {
      m->out[i].stamp = floor_stamp;
    } else {
      floor_stamp = m->out[i].stamp;
    }
  }
  m->last_stamp = floor_stamp;
  m->out_tick = m->out_n;
}

const ZZ9KVoiceRecord *vm_records(const VmModel *m, uint32_t *count)
{
  *count = m->out_n;
  return m->out;
}

void vm_records_taken(VmModel *m, uint32_t n)
{
  uint32_t i;

  if (n > m->out_n) {
    n = m->out_n;
  }
  for (i = n; i < m->out_n; i++) {
    m->out[i - n] = m->out[i];
  }
  m->out_n -= n;
  m->out_tick = m->out_n;
}

/* ---- Engine mirror ---- */

static void vm_bnd_update(VmChannel *c)
{
  uint32_t bound;
  uint32_t k;

  c->e_bnd_valid = 0U;
  if (!c->e_active || c->e_step == 0U) {
    return;
  }
  bound = c->e_bw ? c->e_ls : c->e_end;
  k = zz9k_voice_pass_frames(c->e_bw, c->e_idx, c->e_frac, bound, c->e_step);
  if (k == ZZ9K_VOICE_PASS_NEVER) {
    return;
  }
  c->e_bnd = c->e_t0 + k;
  c->e_bnd_valid = 1U;
}

/* Moves the mirrored position of a running voice to stamp. No pass boundary
 * may lie in between: callers process boundaries in frame order. */
static void vm_mirror_to(VmChannel *c, uint32_t stamp)
{
  if (c->e_active && c->e_step != 0U) {
    uint32_t dt = stamp - c->e_t0;
    uint64_t pos = ((uint64_t)c->e_idx << 16) | c->e_frac;
    uint64_t d = (uint64_t)dt * c->e_step;

    if (c->e_bw) {
      pos = pos > d ? pos - d : 0U;
    } else {
      pos += d;
    }
    c->e_idx = (uint32_t)(pos >> 16);
    c->e_frac = (uint32_t)(pos & 0xFFFFU);
  }
  c->e_t0 = stamp;
}

/* The engine reaches the end of the current pass at frame f (the first frame
 * of the next pass). Updates the mirror like the engine's advance() and
 * returns the overshoot in frames beyond the pass edge for the AHI carry. */
static uint32_t vm_engine_wrap(VmChannel *c, uint32_t f)
{
  uint32_t k = f - c->e_t0;
  uint64_t pos = ((uint64_t)c->e_idx << 16) | c->e_frac;
  uint64_t d = (uint64_t)k * c->e_step;
  uint32_t carry;

  if (!c->e_bw) {
    uint32_t idx;
    uint32_t over;

    pos += d;
    idx = (uint32_t)(pos >> 16);
    over = idx - c->e_end;
    carry = over;
    if (c->e_ll == 0U) {
      c->e_active = 0U;
      c->e_idx = idx;
      c->e_frac = (uint32_t)(pos & 0xFFFFU);
    } else {
      if (over >= c->e_ll) {
        over = 0U;
      }
      c->e_idx = c->e_ls + over;
      c->e_frac = (uint32_t)(pos & 0xFFFFU);
      c->e_end = c->e_ls + c->e_ll;
    }
  } else {
    uint32_t idx;
    uint32_t frac;
    uint32_t under;

    if (pos < d) {
      idx = 0U;
      frac = 0U;
    } else {
      pos -= d;
      idx = (uint32_t)(pos >> 16);
      frac = (uint32_t)(pos & 0xFFFFU);
    }
    under = c->e_ls - idx;
    carry = under;
    if (c->e_ll == 0U) {
      c->e_active = 0U;
      c->e_idx = idx;
      c->e_frac = frac;
    } else {
      if (under >= c->e_ll) {
        under = 0U;
      }
      c->e_end = c->e_ls + c->e_ll;
      c->e_idx = c->e_end - under - 1U;
      c->e_frac = frac;
    }
  }
  c->e_t0 = f;
  vm_bnd_update(c);
  return carry;
}

/* ---- Pass geometry ---- */

/* Fills the sound fields of pass for an AHI_SetSound(offset, length). */
static int vm_pass_from_call(const VmModel *m, VmPass *p, uint32_t sound,
                             uint32_t offset, int32_t length)
{
  const VmSound *s;
  uint32_t shift;
  uint32_t total;
  uint32_t base;

  if (sound >= VM_MAX_SOUNDS || sound >= m->cfg.sounds) {
    return -1;
  }
  s = &m->sounds[sound];
  if (!s->used || s->frames == 0U) {
    return -1;
  }
  shift = vm_view_shift(s->view);
  base = s->bank >> shift;
  total = s->frames;
  if (length == 0) {
    length = (int32_t)(total > 0x7FFFFFFFU ? 0x7FFFFFFFU : total);
  }
  if (offset >= total) {
    offset = total - 1U;
  }
  p->sound = (uint16_t)sound;
  p->view = s->view;
  p->base = base;
  if (length < 0) {
    uint32_t want = (uint32_t)(-(int64_t)length);

    /* Backward: offset is the first frame, the pass runs down to
     * offset + length + 1. */
    if (want > offset + 1U) {
      want = offset + 1U;
    }
    p->backward = 1U;
    p->n = want;
    p->lo = base + offset + 1U - want;
  } else {
    uint32_t want = (uint32_t)length;

    if (want > total - offset) {
      want = total - offset;
    }
    p->backward = 0U;
    p->n = want;
    p->lo = base + offset;
  }
  return 0;
}

static uint32_t vm_pass_first(const VmPass *p)
{
  return p->backward ? p->lo + p->n - 1U : p->lo;
}

/* ---- Records ---- */

static void vm_emit_set(VmModel *m, VmChannel *c, uint32_t ci, uint32_t stamp,
                        uint32_t hz, uint32_t vl, uint32_t vr)
{
  uint32_t flags = 0U;

  if (hz != 0U && hz != c->e_hz) {
    flags |= ZZ9K_VOICE_SET_RATE | ZZ9K_VOICE_SET_RATE_HZ;
  }
  if (!c->e_vol_valid || vl != c->e_vl || vr != c->e_vr) {
    flags |= ZZ9K_VOICE_SET_VOLUME;
  }
  if (flags == 0U) {
    return;
  }
  vm_emit(m, stamp, ci, ZZ9K_VOICE_OP_SET, flags,
          (flags & ZZ9K_VOICE_SET_RATE) ? hz : 0U, (vl << 16) | vr);
  if (flags & ZZ9K_VOICE_SET_RATE) {
    c->e_hz = hz;
    c->e_step = zz9k_voice_rate_step(hz);
  }
  if (flags & ZZ9K_VOICE_SET_VOLUME) {
    c->e_vl = vl;
    c->e_vr = vr;
    c->e_vol_valid = 1U;
  }
}

static void vm_emit_loop(VmModel *m, VmChannel *c, uint32_t ci,
                         uint32_t stamp, uint32_t ls, uint32_t ll)
{
  uint32_t shift = vm_view_shift(c->e_view);

  if (ls == c->e_ls && ll == c->e_ll) {
    return;
  }
  vm_emit(m, stamp, ci, ZZ9K_VOICE_OP_LOOP, 0U, ls << shift, ll << shift);
  c->e_ls = ls;
  c->e_ll = ll;
}

/* Makes the engine wrap into the region the next pass needs when the current
 * pass ends. A forward voice can only do that for a same-view forward next
 * pass that is going to sound; a next pass that will not sound becomes a
 * loop of length 0 so the voice stops at the pass end; anything else is
 * started at the boundary. A backward voice keeps the current region as its
 * lower bound and is handled at the boundary. */
static void vm_program_loop(VmModel *m, VmChannel *c, uint32_t ci,
                            uint32_t stamp)
{
  uint32_t ls = 0U;
  uint32_t ll = 0U;

  if (!c->e_active || c->e_bw) {
    return;
  }
  if (c->next.sound_ok && c->next.freq_ok && !c->next.backward &&
      c->next.view == c->e_view) {
    ls = c->next.lo;
    ll = c->next.n;
  }
  vm_emit_loop(m, c, ci, stamp, ls, ll);
}

/* STARTs the voice for pass p at stamp. The caller emitted the SET. idx is
 * the frame to start at. Mirrors the engine: START clears the loop and the
 * direction. */
static void vm_start_pass(VmModel *m, VmChannel *c, uint32_t ci,
                          uint32_t stamp, const VmPass *p, uint32_t idx)
{
  uint32_t shift = vm_view_shift(p->view);

  c->e_view = p->view;
  c->e_has_view = 1U;
  if (!p->backward) {
    uint32_t end = p->lo + p->n;

    if (idx < p->lo || idx >= end) {
      idx = p->lo;
    }
    vm_emit(m, stamp, ci, ZZ9K_VOICE_OP_START, p->view, idx << shift,
            (end - idx) << shift);
    c->e_end = end;
  } else {
    uint32_t last = p->lo + p->n - 1U;

    if (idx < p->lo || idx > last) {
      idx = last;
    }
    vm_emit(m, stamp, ci, ZZ9K_VOICE_OP_START, p->view, idx << shift, 0U);
    c->e_end = p->lo + p->n;
  }
  c->e_active = 1U;
  c->e_bw = 0U;
  c->e_idx = idx;
  c->e_frac = 0U;
  c->e_t0 = stamp;
  c->e_ls = 0U;
  c->e_ll = 0U;
  if (p->backward) {
    vm_emit_loop(m, c, ci, stamp, p->lo, p->n);
    vm_emit(m, stamp, ci, ZZ9K_VOICE_OP_DIRECTION, ZZ9K_VOICE_DIR_BACKWARD, 0U,
            0U);
    c->e_bw = 1U;
  }
  vm_bnd_update(c);
}

static void vm_stop_voice(VmModel *m, VmChannel *c, uint32_t ci,
                          uint32_t stamp)
{
  if (!c->e_active) {
    return;
  }
  vm_mirror_to(c, stamp);
  vm_emit(m, stamp, ci, ZZ9K_VOICE_OP_STOP, 0U, 0U, 0U);
  c->e_active = 0U;
  c->e_bnd_valid = 0U;
}

/* Starts or resumes the voice for the current pass from the mirrored
 * position, as it stands at stamp. */
static void vm_run_cur(VmModel *m, VmChannel *c, uint32_t ci, uint32_t stamp,
                       uint32_t idx)
{
  vm_emit_set(m, c, ci, stamp, c->cur.hz, c->cur.vl, c->cur.vr);
  vm_start_pass(m, c, ci, stamp, &c->cur, idx);
  vm_program_loop(m, c, ci, stamp);
}

/* ---- AHI calls ----
 *
 * A call only changes the AHI picture and marks the channel. The records
 * follow in vm_reconcile after the whole batch of calls, which sees the final
 * state of the batch. */

#define VM_DIRTY   1U
#define VM_RESTART 2U /* an IMM SetSound: the pass starts over */

static void vm_apply_sound(VmModel *m, VmChannel *c, uint32_t sound,
                           uint32_t offset, int32_t length, int imm)
{
  VmPass p;

  if (sound == VM_NOSOUND) {
    c->next.sound_ok = 0U;
    c->dirty |= VM_DIRTY;
    if (imm) {
      c->cur.sound_ok = 0U;
      c->eos = 1U;
    }
    return;
  }
  p = c->next;
  if (vm_pass_from_call(m, &p, sound, offset, length) != 0) {
    return;
  }
  p.sound_ok = 1U;
  c->next = p;
  c->dirty |= VM_DIRTY;
  if (!imm) {
    return;
  }
  /* IMM: the pass replaces the current one and starts at its first frame.
   * Rate and volume stay as they were. */
  c->cur.sound_ok = 1U;
  c->cur.sound = p.sound;
  c->cur.view = p.view;
  c->cur.backward = p.backward;
  c->cur.base = p.base;
  c->cur.lo = p.lo;
  c->cur.n = p.n;
  c->eos = 1U;
  c->e_idx = vm_pass_first(&c->cur);
  c->e_frac = 0U;
  c->dirty |= VM_RESTART;
}

static void vm_apply_freq(VmChannel *c, uint32_t hz, int imm)
{
  c->next.hz = hz;
  c->next.freq_ok = hz != 0U ? 1U : 0U;
  c->dirty |= VM_DIRTY;
  if (imm) {
    c->cur.hz = hz;
    c->cur.freq_ok = c->next.freq_ok;
  }
}

static void vm_apply_vol(VmModel *m, VmChannel *c, int32_t volume, int32_t pan,
                         int imm)
{
  uint16_t l;
  uint16_t r;

  vm_levels(m->master_m16, m->cfg.channels, volume, pan, &l, &r);
  c->next.vol = volume;
  c->next.pan = pan;
  c->next.vl = l;
  c->next.vr = r;
  c->dirty |= VM_DIRTY;
  if (imm) {
    c->cur.vol = volume;
    c->cur.pan = pan;
    c->cur.vl = l;
    c->cur.vr = r;
  }
}

static void vm_emit_master(VmModel *m, uint32_t stamp)
{
  uint32_t gain = vm_master_gain(m->master_m16, m->cfg.channels);

  m->master_gain = gain;
  vm_emit(m, stamp, 0U, ZZ9K_VOICE_OP_MASTER, 0U, gain, 0U);
}

/* AHIET_MASTERVOLUME. The engine gain is a whole number of 1/256 and the
 * voice volumes carry the rounding residual, so every channel's levels move
 * with the master. */
static void vm_apply_master(VmModel *m, uint32_t master_m16, uint32_t stamp)
{
  uint32_t ci;

  m->master_m16 = master_m16;
  for (ci = 0U; ci < m->cfg.channels; ci++) {
    VmChannel *c = &m->ch[ci];

    vm_levels(master_m16, m->cfg.channels, c->cur.vol, c->cur.pan, &c->cur.vl,
              &c->cur.vr);
    vm_levels(master_m16, m->cfg.channels, c->next.vol, c->next.pan,
              &c->next.vl, &c->next.vr);
    c->dirty |= VM_DIRTY;
  }
  if (m->running &&
      vm_master_gain(master_m16, m->cfg.channels) != m->master_gain) {
    vm_emit_master(m, stamp);
  }
}

static void vm_apply_call(VmModel *m, const VmCall *call, int imm,
                          uint32_t stamp)
{
  VmChannel *c;

  if (call->kind == VM_CALL_MASTER) {
    vm_apply_master(m, call->a, stamp);
    return;
  }
  if (call->channel >= m->cfg.channels) {
    return;
  }
  c = &m->ch[call->channel];
  switch (call->kind) {
    case VM_CALL_SOUND:
      vm_apply_sound(m, c, call->sound, call->a, call->b, imm);
      break;
    case VM_CALL_FREQ:
      vm_apply_freq(c, call->a, imm);
      break;
    case VM_CALL_VOL:
      vm_apply_vol(m, c, (int32_t)call->a, call->b, imm);
      break;
    default:
      break;
  }
}

/* Brings the engine in line with the AHI picture after a batch of calls. */
static void vm_reconcile(VmModel *m, VmChannel *c, uint32_t ci,
                         uint32_t stamp)
{
  uint32_t flags = c->dirty;

  c->dirty = 0U;
  if (!(flags & VM_DIRTY)) {
    return;
  }
  if (!m->running) {
    if (flags & VM_RESTART) {
      c->e_active = 0U;
    }
    return;
  }
  if (!(c->cur.sound_ok && c->cur.freq_ok)) {
    if (c->e_active) {
      if (!(flags & VM_RESTART)) {
        vm_mirror_to(c, stamp);
      }
      vm_emit(m, stamp, ci, ZZ9K_VOICE_OP_STOP, 0U, 0U, 0U);
      c->e_active = 0U;
      c->e_bnd_valid = 0U;
    }
    c->e_t0 = stamp;
    return;
  }
  if (c->e_active && !(flags & VM_RESTART)) {
    if (c->cur.hz != c->e_hz) {
      /* A new rate moves the next boundary: carry the position to the stamp
       * first. A volume change leaves the mirror alone, because the stamp
       * may lie past the pass end. */
      vm_mirror_to(c, stamp);
      vm_emit_set(m, c, ci, stamp, c->cur.hz, c->cur.vl, c->cur.vr);
      vm_bnd_update(c);
    } else {
      vm_emit_set(m, c, ci, stamp, 0U, c->cur.vl, c->cur.vr);
    }
    vm_program_loop(m, c, ci, stamp);
    return;
  }
  /* A restart or a resume from a pause: START from the mirrored index. */
  c->e_active = 0U;
  vm_run_cur(m, c, ci, stamp, c->e_idx);
}

/* ---- FIFO ---- */

int vm_fifo_push(VmModel *m, const VmCall *call)
{
  uint32_t next = (m->fifo_tail + 1U) % VM_FIFO_SIZE;

  if (next == m->fifo_head) {
    m->fifo_drops++;
    return -1;
  }
  m->fifo[m->fifo_tail] = *call;
  m->fifo_tail = next;
  return 0;
}

uint32_t vm_fifo_pending(const VmModel *m)
{
  return (m->fifo_tail + VM_FIFO_SIZE - m->fifo_head) % VM_FIFO_SIZE;
}

static int vm_fifo_pop(VmModel *m, VmCall *out)
{
  const VmHooks *h = &m->cfg.hooks;
  int got = 0;

  if (h->blocked && h->blocked(h->user)) {
    return 0;
  }
  if (h->lock) {
    h->lock(h->user);
  }
  if (m->fifo_head != m->fifo_tail) {
    *out = m->fifo[m->fifo_head];
    m->fifo_head = (m->fifo_head + 1U) % VM_FIFO_SIZE;
    got = 1;
  }
  if (h->unlock) {
    h->unlock(h->user);
  }
  return got;
}

static void vm_drain(VmModel *m, uint32_t stamp, int imm_allowed)
{
  VmCall call;
  uint32_t ci;

  while (vm_fifo_pop(m, &call)) {
    vm_apply_call(m, &call, imm_allowed && call.imm, stamp);
  }
  /* One reconcile per channel after the whole batch, so a SetSound, SetFreq
   * and SetVol sequence makes one START and not a record per call. */
  for (ci = 0U; ci < m->cfg.channels; ci++) {
    vm_reconcile(m, &m->ch[ci], ci, stamp);
  }
}

/* ---- Boundaries and SoundFunc ---- */

static void vm_run_sound_hook(VmModel *m, uint32_t ci, uint32_t stamp)
{
  VmChannel *c = &m->ch[ci];

  c->eos = 0U;
  m->sound_stamp = stamp;
  if (m->cfg.hooks.sound) {
    m->cfg.hooks.sound(m->cfg.hooks.user, (uint16_t)ci);
    /* Calls made inside SoundFunc define the following pass: never IMM. */
    vm_drain(m, stamp, 0);
  }
}

static void vm_run_eos(VmModel *m, uint32_t stamp)
{
  uint32_t ci;

  for (ci = 0U; ci < m->cfg.channels; ci++) {
    if (m->ch[ci].eos) {
      vm_run_sound_hook(m, ci, stamp);
    }
  }
}

/* The channel's current pass ends at c->e_bnd: install the next pass. */
static void vm_process_boundary(VmModel *m, uint32_t ci, int inline_sound)
{
  VmChannel *c = &m->ch[ci];
  uint32_t f = c->e_bnd;
  uint32_t carry = vm_engine_wrap(c, f);
  int running;
  int engine_in_cur;

  c->cur = c->next;
  c->eos = 1U;
  if (c->cur.sound_ok && carry >= c->cur.n) {
    carry = 0U;
  }
  running = c->cur.sound_ok && c->cur.freq_ok;
  if (!running) {
    if (c->e_active) {
      vm_stop_voice(m, c, ci, f);
    }
    if (c->cur.sound_ok) {
      uint32_t first = vm_pass_first(&c->cur);

      c->e_idx = c->cur.backward ? first - carry : c->cur.lo + carry;
      c->e_frac = 0U;
    }
    c->e_active = 0U;
    c->e_bnd_valid = 0U;
  } else {
    engine_in_cur = c->e_active && c->e_has_view && c->e_view == c->cur.view &&
                    c->e_bw == c->cur.backward && c->e_ls == c->cur.lo &&
                    c->e_ll == c->cur.n;
    if (engine_in_cur) {
      vm_emit_set(m, c, ci, f, c->cur.hz, c->cur.vl, c->cur.vr);
      /* A new rate moves the next boundary. */
      vm_bnd_update(c);
    } else {
      uint32_t idx = c->cur.backward ? vm_pass_first(&c->cur) - carry
                                     : c->cur.lo + carry;

      c->e_active = 0U;
      vm_run_cur(m, c, ci, f, idx);
    }
  }
  if (inline_sound) {
    vm_run_sound_hook(m, ci, f);
  }
}

/* Processes every pass boundary before limit, in frame order. */
static void vm_walk(VmModel *m, uint32_t limit, int inline_sound)
{
  uint32_t iterations = 0U;

  for (;;) {
    uint32_t best = VM_MAX_CHANNELS;
    uint32_t ci;

    for (ci = 0U; ci < m->cfg.channels; ci++) {
      const VmChannel *c = &m->ch[ci];

      if (!c->e_active || !c->e_bnd_valid || vm_sdiff(c->e_bnd, limit) >= 0) {
        continue;
      }
      if (best == VM_MAX_CHANNELS ||
          vm_sdiff(c->e_bnd, m->ch[best].e_bnd) < 0) {
        best = ci;
      }
    }
    if (best == VM_MAX_CHANNELS) {
      return;
    }
    if (++iterations > VM_MAX_BOUNDARIES_PER_TICK) {
      m->overload++;
      return;
    }
    vm_process_boundary(m, best, inline_sound);
  }
}

/* ---- Time ---- */

void vm_tick(VmModel *m)
{
  uint32_t s0 = m->now;
  uint64_t t1;
  uint32_t s1;

  /* Boundaries exactly at the tick start were reached at the end of the
   * previous mix period in ahi.device: install them before PlayerFunc, but
   * owe their SoundFunc call until after it. */
  vm_walk(m, s0 + 1U, 0);
  if (m->cfg.hooks.player) {
    m->cfg.hooks.player(m->cfg.hooks.user);
  }
  vm_drain(m, s0, 1);
  vm_run_eos(m, s0);
  /* PlayerFunc may change AHIA_PlayerFreq: the next tick follows it. */
  t1 = m->t + vm_tick_frames16(m->cfg.hooks.player_freq
                                   ? m->cfg.hooks.player_freq(m->cfg.hooks.user)
                                   : 0U);
  s1 = (uint32_t)(t1 >> 16);
  vm_walk(m, s1, 1);
  m->t = t1;
  m->now = s1;
  m->ticks++;
  vm_flush_tick(m);
}

void vm_run_async(VmModel *m)
{
  vm_drain(m, m->now, 1);
  vm_run_eos(m, m->now);
  vm_flush_tick(m);
}

int vm_tick_due(const VmModel *m, uint32_t playing_frame, uint32_t run_ahead)
{
  return vm_sdiff((uint32_t)(m->t >> 16), playing_frame) <
         (int32_t)run_ahead;
}

/* ---- Playback control ---- */

void vm_start(VmModel *m, uint32_t stamp)
{
  uint32_t ci;

  m->running = 1U;
  m->now = stamp;
  m->t = (uint64_t)stamp << 16;
  m->last_stamp = stamp;
  m->out_tick = m->out_n;
  vm_emit_master(m, stamp);
  for (ci = 0U; ci < m->cfg.channels; ci++) {
    VmChannel *c = &m->ch[ci];

    c->e_active = 0U;
    c->e_bnd_valid = 0U;
    c->e_has_view = 0U;
    c->e_hz = 0U;
    c->e_step = 0U;
    c->e_vol_valid = 0U;
    c->e_ls = 0U;
    c->e_ll = 0U;
    c->e_t0 = stamp;
    if (c->cur.sound_ok && c->cur.freq_ok) {
      vm_run_cur(m, c, ci, stamp, c->e_idx);
    }
  }
  vm_flush_tick(m);
}

void vm_stop(VmModel *m)
{
  uint32_t ci;

  for (ci = 0U; ci < m->cfg.channels; ci++) {
    VmChannel *c = &m->ch[ci];

    if (c->e_active) {
      vm_mirror_to(c, m->now);
    }
    c->e_active = 0U;
    c->e_bnd_valid = 0U;
  }
  m->running = 0U;
  m->out_n = 0U;
  m->out_tick = 0U;
}

void vm_channel_offsets(const VmModel *m, uint32_t *offsets, uint32_t count)
{
  uint32_t ci;

  for (ci = 0U; ci < count; ci++) {
    uint32_t off = 0U;

    if (ci < m->cfg.channels) {
      const VmChannel *c = &m->ch[ci];
      uint32_t idx = c->e_idx;

      if (c->e_active && c->e_step != 0U) {
        uint32_t dt = m->now - c->e_t0;
        uint64_t pos = ((uint64_t)c->e_idx << 16) | c->e_frac;
        uint64_t d = (uint64_t)dt * c->e_step;

        pos = c->e_bw ? (pos > d ? pos - d : 0U) : pos + d;
        idx = (uint32_t)(pos >> 16);
      }
      off = idx >= c->cur.base ? idx - c->cur.base : 0U;
    }
    offsets[ci] = off;
  }
}

/* ---- Setup and sounds ---- */

void vm_init(VmModel *m, const VmConfig *cfg)
{
  uint32_t ci;
  uint32_t usable;

  vm_zero(m, sizeof(*m));
  m->cfg = *cfg;
  if (m->cfg.channels == 0U) {
    m->cfg.channels = 1U;
  }
  if (m->cfg.channels > VM_MAX_CHANNELS) {
    m->cfg.channels = VM_MAX_CHANNELS;
  }
  if (m->cfg.sounds > VM_MAX_SOUNDS) {
    m->cfg.sounds = VM_MAX_SOUNDS;
  }
  m->master_m16 = 0x10000U;
  for (ci = 0U; ci < VM_MAX_CHANNELS; ci++) {
    m->ch[ci].next.vl = 0U;
    m->ch[ci].cur.vl = 0U;
  }
  usable = m->cfg.bank_bytes & ~3U;
  if (usable != 0U) {
    m->free_list[0].off = 0U;
    m->free_list[0].len = usable;
    m->free_n = 1U;
  }
}

int vm_sound_define(VmModel *m, uint32_t id, uint32_t ahi_type, uint32_t bank,
                    uint32_t frames)
{
  uint32_t view;
  VmSound *s;

  if (id >= m->cfg.sounds || id >= VM_MAX_SOUNDS) {
    return -1;
  }
  switch (ahi_type) {
    case VM_AHI_M8S:
      view = VM_VIEW_S8;
      break;
    case VM_AHI_M16S:
      view = VM_VIEW_S16;
      break;
    case VM_AHI_S8S:
      view = VM_VIEW_S8S;
      break;
    case VM_AHI_S16S:
      view = VM_VIEW_S16S;
      break;
    default:
      return -1;
  }
  if ((view == VM_VIEW_S8S || view == VM_VIEW_S16S) && !m->cfg.stereo_views) {
    return -1;
  }
  s = &m->sounds[id];
  s->used = 0U;
  s->view = (uint8_t)view;
  s->bank = bank;
  s->frames = frames;
  s->used = 1U;
  return 0;
}

/* ---- Bank allocator ---- */

static void vm_free_insert(VmModel *m, uint32_t off, uint32_t len)
{
  uint32_t i = 0U;

  while (i < m->free_n && m->free_list[i].off < off) {
    i++;
  }
  /* Merge with the neighbour behind. */
  if (i > 0U && m->free_list[i - 1U].off + m->free_list[i - 1U].len == off) {
    m->free_list[i - 1U].len += len;
    if (i < m->free_n &&
        m->free_list[i - 1U].off + m->free_list[i - 1U].len ==
            m->free_list[i].off) {
      uint32_t j;

      m->free_list[i - 1U].len += m->free_list[i].len;
      for (j = i + 1U; j < m->free_n; j++) {
        m->free_list[j - 1U] = m->free_list[j];
      }
      m->free_n--;
    }
    return;
  }
  /* Merge with the neighbour ahead. */
  if (i < m->free_n && off + len == m->free_list[i].off) {
    m->free_list[i].off = off;
    m->free_list[i].len += len;
    return;
  }
  if (m->free_n >= VM_MAX_FREE_RANGES) {
    return; /* too fragmented to track: the range is lost until close */
  }
  {
    uint32_t j;

    for (j = m->free_n; j > i; j--) {
      m->free_list[j] = m->free_list[j - 1U];
    }
  }
  m->free_list[i].off = off;
  m->free_list[i].len = len;
  m->free_n++;
}

int vm_bank_alloc(VmModel *m, uint32_t bytes, uint32_t *offset)
{
  uint32_t i;

  if (bytes == 0U) {
    bytes = 4U;
  }
  bytes = (bytes + 3U) & ~3U;
  if (bytes < 4U) {
    return -1;
  }
  for (i = 0U; i < m->free_n; i++) {
    VmFree *f = &m->free_list[i];

    if (f->len >= bytes) {
      uint32_t j;

      *offset = f->off;
      f->off += bytes;
      f->len -= bytes;
      if (f->len == 0U) {
        for (j = i + 1U; j < m->free_n; j++) {
          m->free_list[j - 1U] = m->free_list[j];
        }
        m->free_n--;
      }
      return 0;
    }
  }
  return -1;
}

/* Gives back a range that nothing references (an upload that failed before
 * the sound was defined), so it is free at once. */
void vm_bank_release(VmModel *m, uint32_t offset, uint32_t bytes)
{
  if (bytes == 0U) {
    bytes = 4U;
  }
  vm_free_insert(m, offset, (bytes + 3U) & ~3U);
}

void vm_bank_poll(VmModel *m, uint32_t horizon)
{
  uint32_t i = 0U;

  while (i < m->quarantine_n) {
    VmQuarantine *q = &m->quarantine[i];

    if (vm_sdiff(horizon, q->stamp) > 0) {
      uint32_t j;

      vm_free_insert(m, q->off, q->len);
      for (j = i + 1U; j < m->quarantine_n; j++) {
        m->quarantine[j - 1U] = m->quarantine[j];
      }
      m->quarantine_n--;
    } else {
      i++;
    }
  }
}

uint32_t vm_bank_free_bytes(const VmModel *m)
{
  uint32_t i;
  uint32_t total = 0U;

  for (i = 0U; i < m->free_n; i++) {
    total += m->free_list[i].len;
  }
  return total;
}

int vm_sound_remove(VmModel *m, uint32_t id)
{
  VmSound *s;
  uint32_t ci;
  uint32_t off;
  uint32_t len;

  if (id >= m->cfg.sounds || id >= VM_MAX_SOUNDS || !m->sounds[id].used) {
    return -1;
  }
  s = &m->sounds[id];
  for (ci = 0U; ci < m->cfg.channels; ci++) {
    VmChannel *c = &m->ch[ci];

    if (c->cur.sound_ok && c->cur.sound == id) {
      c->cur.sound_ok = 0U;
      if (m->running) {
        vm_stop_voice(m, c, ci, m->now);
      }
      c->e_active = 0U;
    }
    if (c->next.sound_ok && c->next.sound == id) {
      c->next.sound_ok = 0U;
    }
  }
  off = s->bank;
  len = s->frames << vm_view_shift(s->view);
  s->used = 0U;
  /* The bank range is a whole number of 4-byte units from the allocator. */
  len = (len + 3U) & ~3U;
  if (len == 0U) {
    return 0;
  }
  if (m->running && m->quarantine_n < VM_MAX_QUARANTINE) {
    VmQuarantine *q = &m->quarantine[m->quarantine_n++];

    q->off = off;
    q->len = len;
    q->stamp = m->now;
  } else {
    vm_free_insert(m, off, len);
  }
  vm_flush_tick(m);
  return 0;
}
