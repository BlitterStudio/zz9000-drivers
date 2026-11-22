/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Host tests for the AHI channel model: call sequences in, records out. The
 * differential test against the real engine is voice_ref_test.c.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "voice_model.h"

static int failures;

#define CHECK(cond, ...)                                              \
  do {                                                                \
    if (!(cond)) {                                                    \
      printf("FAIL %s:%d: ", __FILE__, __LINE__);                     \
      printf(__VA_ARGS__);                                            \
      printf("\n");                                                   \
      failures++;                                                     \
    }                                                                 \
  } while (0)

typedef struct App {
  VmModel *m;
  void (*player)(struct App *);
  void (*sound)(struct App *, uint16_t);
  uint32_t sound_calls;
  uint16_t sound_log[64];
  uint32_t player_calls;
  uint32_t disabled;
  uint32_t freq16;
} App;

static void app_player(void *user)
{
  App *a = (App *)user;

  a->player_calls++;
  if (a->player) {
    a->player(a);
  }
}

static void app_sound(void *user, uint16_t ch)
{
  App *a = (App *)user;

  if (a->sound_calls < 64U) {
    a->sound_log[a->sound_calls] = ch;
  }
  a->sound_calls++;
  if (a->sound) {
    a->sound(a, ch);
  }
}

static int app_blocked(void *user)
{
  return ((App *)user)->disabled != 0U;
}

static uint32_t app_freq(void *user)
{
  return ((App *)user)->freq16;
}

static void set_sound(VmModel *m, uint16_t ch, uint16_t sound, uint32_t off,
                      int32_t len, int imm)
{
  VmCall c;

  memset(&c, 0, sizeof(c));
  c.kind = VM_CALL_SOUND;
  c.imm = (uint8_t)imm;
  c.channel = ch;
  c.sound = sound;
  c.a = off;
  c.b = len;
  CHECK(vm_fifo_push(m, &c) == 0, "fifo full");
}

static void set_freq(VmModel *m, uint16_t ch, uint32_t hz, int imm)
{
  VmCall c;

  memset(&c, 0, sizeof(c));
  c.kind = VM_CALL_FREQ;
  c.imm = (uint8_t)imm;
  c.channel = ch;
  c.a = hz;
  CHECK(vm_fifo_push(m, &c) == 0, "fifo full");
}

static void set_vol(VmModel *m, uint16_t ch, int32_t vol, int32_t pan, int imm)
{
  VmCall c;

  memset(&c, 0, sizeof(c));
  c.kind = VM_CALL_VOL;
  c.imm = (uint8_t)imm;
  c.channel = ch;
  c.a = (uint32_t)vol;
  c.b = pan;
  CHECK(vm_fifo_push(m, &c) == 0, "fifo full");
}

static void set_master(VmModel *m, uint32_t vol)
{
  VmCall c;

  memset(&c, 0, sizeof(c));
  c.kind = VM_CALL_MASTER;
  c.a = vol;
  CHECK(vm_fifo_push(m, &c) == 0, "fifo full");
}

static VmModel *model;
static App app;

/* Sounds: 0 mono 8-bit 1000 frames at bank 0; 1 mono 8-bit 2000 frames;
 * 2 mono 16-bit 500 frames; 3 stereo 16-bit 300 frames. */
static void setup(uint32_t channels, int stereo)
{
  VmConfig cfg;

  if (!model) {
    model = (VmModel *)malloc(sizeof(*model));
  }
  memset(&app, 0, sizeof(app));
  app.freq16 = 50U << 16;
  memset(&cfg, 0, sizeof(cfg));
  cfg.channels = channels;
  cfg.sounds = 16U;
  cfg.bank_bytes = 1U << 20;
  cfg.stereo_views = (uint32_t)stereo;
  cfg.hooks.player = app_player;
  cfg.hooks.sound = app_sound;
  cfg.hooks.blocked = app_blocked;
  cfg.hooks.player_freq = app_freq;
  cfg.hooks.user = &app;
  vm_init(model, &cfg);
  app.m = model;
  CHECK(vm_sound_define(model, 0U, VM_AHI_M8S, 0U, 1000U) == 0, "define 0");
  CHECK(vm_sound_define(model, 1U, VM_AHI_M8S, 1024U, 2000U) == 0, "define 1");
  CHECK(vm_sound_define(model, 2U, VM_AHI_M16S, 4096U, 500U) == 0, "define 2");
  if (stereo) {
    CHECK(vm_sound_define(model, 3U, VM_AHI_S16S, 8192U, 300U) == 0,
          "define 3");
  }
}

static void clear_records(void)
{
  uint32_t n;

  vm_records(model, &n);
  vm_records_taken(model, n);
}

static uint32_t rec_count(void)
{
  uint32_t n;

  vm_records(model, &n);
  return n;
}

static const ZZ9KVoiceRecord *rec(uint32_t i)
{
  uint32_t n;
  const ZZ9KVoiceRecord *r = vm_records(model, &n);

  return i < n ? &r[i] : NULL;
}

static int is_op(uint32_t i, uint8_t op, uint8_t voice, uint32_t stamp)
{
  const ZZ9KVoiceRecord *r = rec(i);

  return r && r->op == op && r->voice == voice && r->stamp == stamp;
}

static void dump(void)
{
  uint32_t n;
  uint32_t i;
  const ZZ9KVoiceRecord *r = vm_records(model, &n);

  for (i = 0U; i < n; i++) {
    printf("  [%u] stamp=%u voice=%u op=%u flags=%u a0=%u a1=%u\n", i,
           (unsigned)r[i].stamp, r[i].voice, r[i].op, r[i].flags,
           (unsigned)r[i].arg0, (unsigned)r[i].arg1);
  }
}

/* ---- Tests ---- */

static void test_imm_set_start_loop(void)
{
  const ZZ9KVoiceRecord *r;

  setup(4U, 1);
  vm_start(model, 1000U);
  clear_records();

  /* Sound, frequency, volume, all IMM, in one batch from PlayerFunc. */
  set_sound(model, 0U, 0U, 0U, 0, 1);
  set_freq(model, 0U, 8000U, 1);
  set_vol(model, 0U, 0x10000, 0x8000, 1);
  vm_tick(model);

  /* SET rate and volume, START, LOOP at one stamp, in that order; no sound
   * before both sound and frequency are set. */
  CHECK(rec_count() == 3U, "records %u", rec_count());
  if (rec_count() == 3U) {
    r = rec(0);
    CHECK(r->op == ZZ9K_VOICE_OP_SET && r->stamp == 1000U &&
              r->flags == (ZZ9K_VOICE_SET_RATE | ZZ9K_VOICE_SET_RATE_HZ |
                           ZZ9K_VOICE_SET_VOLUME) &&
              r->arg0 == 8000U && r->arg1 == ((0x8000U << 16) | 0x8000U),
          "set rate/vol");
    r = rec(1);
    CHECK(r->op == ZZ9K_VOICE_OP_START && r->stamp == 1000U &&
              r->flags == VM_VIEW_S8 && r->arg0 == 0U && r->arg1 == 1000U,
          "start");
    r = rec(2);
    CHECK(r->op == ZZ9K_VOICE_OP_LOOP && r->stamp == 1000U && r->arg0 == 0U &&
              r->arg1 == 1000U,
          "loop");
  } else {
    dump();
  }
  /* SoundFunc ran once at the first pass start. */
  CHECK(app.sound_calls == 1U && app.sound_log[0] == 0U, "sound calls %u",
        app.sound_calls);
}

static void test_no_sound_before_freq(void)
{
  setup(2U, 1);
  vm_start(model, 0U);
  clear_records();
  set_sound(model, 1U, 1U, 0U, 0, 1);
  vm_tick(model);
  CHECK(rec_count() == 0U, "sound without frequency made %u records",
        rec_count());
  set_freq(model, 1U, 22050U, 1);
  vm_run_async(model);
  CHECK(rec_count() >= 2U && is_op(0, ZZ9K_VOICE_OP_SET, 1U, 960U),
        "frequency after sound starts the voice");
}

static void test_queued_same_view_is_loop(void)
{
  setup(2U, 1);
  vm_start(model, 0U);
  set_sound(model, 0U, 0U, 0U, 400, 1);
  set_freq(model, 0U, 48000U, 1);
  set_vol(model, 0U, 0x10000, 0, 1);
  vm_tick(model);
  clear_records();
  /* A queued SetSound in the same type is a LOOP, not a START. */
  set_sound(model, 0U, 1U, 100U, 200, 0);
  vm_tick(model);
  CHECK(rec_count() >= 1U, "no loop record");
  {
    uint32_t i;
    uint32_t loops = 0U;
    uint32_t starts = 0U;

    for (i = 0U; i < rec_count(); i++) {
      loops += rec(i)->op == ZZ9K_VOICE_OP_LOOP;
      starts += rec(i)->op == ZZ9K_VOICE_OP_START;
    }
    CHECK(loops >= 1U && starts == 0U, "loops %u starts %u", loops, starts);
  }
}

static void test_queued_nosound_silences_at_pass_end(void)
{
  uint32_t i;

  setup(2U, 1);
  vm_start(model, 0U);
  set_sound(model, 0U, 0U, 0U, 100, 1);
  set_freq(model, 0U, 48000U, 1);
  set_vol(model, 0U, 0x10000, 0, 1);
  vm_tick(model);
  clear_records();
  set_sound(model, 0U, VM_NOSOUND, 0U, 0, 0);
  vm_tick(model);
  /* LOOP length 0: the engine stops at the pass end by itself. */
  for (i = 0U; i < rec_count(); i++) {
    if (rec(i)->op == ZZ9K_VOICE_OP_LOOP) {
      break;
    }
  }
  CHECK(i < rec_count() && rec(i)->arg1 == 0U, "queued NOSOUND is LOOP 0");
}

static void test_other_type_is_start_at_boundary(void)
{
  uint32_t i;
  uint32_t starts = 0U;
  uint32_t first_start = 0U;

  setup(2U, 1);
  vm_start(model, 0U);
  /* 8-bit sound plays 480 frames per pass at 24 kHz: 960 frames x 1/2 ... */
  set_sound(model, 0U, 0U, 0U, 100, 1);
  set_freq(model, 0U, 24000U, 1);
  set_vol(model, 0U, 0x10000, 0, 1);
  vm_tick(model);
  clear_records();
  /* A 16-bit sound queued: different view, so the model STARTs it at the
   * predicted boundary: pass 1 is 100 frames at 24 kHz = 200 output frames,
   * and pass 1 began at stamp 0. */
  set_sound(model, 0U, 2U, 0U, 50, 0);
  vm_tick(model);
  for (i = 0U; i < rec_count(); i++) {
    if (rec(i)->op == ZZ9K_VOICE_OP_START) {
      if (starts++ == 0U) {
        first_start = rec(i)->stamp;
      }
    }
  }
  CHECK(starts >= 1U && first_start == 1000U, "first START at %u",
        first_start);
}

static void test_backward_pass_records(void)
{
  setup(2U, 1);
  vm_start(model, 0U);
  /* Offset 99, length -100: samples 99 down to 0. */
  set_sound(model, 0U, 0U, 99U, -100, 1);
  set_freq(model, 0U, 48000U, 1);
  set_vol(model, 0U, 0x10000, 0, 1);
  clear_records();
  vm_tick(model);
  /* SET, START at 99, LOOP (0, 100), DIRECTION backward. */
  CHECK(is_op(0, ZZ9K_VOICE_OP_SET, 0U, 0U), "set");
  CHECK(is_op(1, ZZ9K_VOICE_OP_START, 0U, 0U) && rec(1)->arg0 == 99U &&
            rec(1)->arg1 == 0U,
        "start backward");
  CHECK(is_op(2, ZZ9K_VOICE_OP_LOOP, 0U, 0U) && rec(2)->arg0 == 0U &&
            rec(2)->arg1 == 100U,
        "loop is the region");
  CHECK(is_op(3, ZZ9K_VOICE_OP_DIRECTION, 0U, 0U) &&
            rec(3)->flags == ZZ9K_VOICE_DIR_BACKWARD,
        "direction");
}

static void test_freq_zero_pause_and_resume(void)
{
  uint32_t i;
  int stop = 0;
  int start = 0;

  setup(2U, 1);
  vm_start(model, 0U);
  set_sound(model, 0U, 1U, 0U, 0, 1);
  set_freq(model, 0U, 48000U, 1);
  set_vol(model, 0U, 0x10000, 0, 1);
  vm_tick(model);
  clear_records();
  set_freq(model, 0U, 0U, 1);
  vm_tick(model);
  for (i = 0U; i < rec_count(); i++) {
    stop |= rec(i)->op == ZZ9K_VOICE_OP_STOP && rec(i)->stamp == 960U;
  }
  CHECK(stop, "frequency 0 stops the voice");
  clear_records();
  set_freq(model, 0U, 48000U, 1);
  vm_tick(model);
  for (i = 0U; i < rec_count(); i++) {
    if (rec(i)->op == ZZ9K_VOICE_OP_START) {
      /* Resumes at the frame the pause left it: 960 frames in, then 960
       * paused, so the position is 960. */
      start = rec(i)->arg0 == 1024U + 960U;
    }
  }
  CHECK(start, "frequency back resumes at the same point");
}

static void sound_alternate(App *a, uint16_t ch)
{
  /* Each pass installs the 200-frame region at 300 of sound 1. */
  set_sound(a->m, ch, 1U, 300U, 200, 0);
}

static void test_soundfunc_multi_segment(void)
{
  uint32_t i;
  uint32_t loops = 0U;
  uint32_t starts = 0U;

  setup(1U, 1);
  vm_start(model, 0U);
  set_sound(model, 0U, 0U, 0U, 100, 1);
  set_freq(model, 0U, 48000U, 1);
  set_vol(model, 0U, 0x10000, 0, 1);
  app.sound = sound_alternate;
  clear_records();
  vm_tick(model);
  /* 100-frame passes at 48 kHz: the first SoundFunc call is at the IMM start
   * (stamp 0), then pass 1 ends at 100 and the 200-frame region repeats: 100
   * to 299, 300 to 499, ... One tick of 960 frames sees boundaries at 100,
   * 300, 500, 700, 900: 1 + 5 SoundFunc calls. */
  CHECK(app.sound_calls == 6U, "sound calls %u", app.sound_calls);
  for (i = 0U; i < rec_count(); i++) {
    loops += rec(i)->op == ZZ9K_VOICE_OP_LOOP;
    starts += rec(i)->op == ZZ9K_VOICE_OP_START;
  }
  /* The first call defines the region (one LOOP at stamp 1 replacing the
   * repeat of the first region); later calls repeat it: no more records. */
  CHECK(starts == 1U, "starts %u", starts);
  CHECK(loops == 2U, "loops %u (initial repeat + the new region)", loops);
}

static void test_allocator(void)
{
  uint32_t a;
  uint32_t b;
  uint32_t c;
  uint32_t d;

  setup(1U, 1);
  CHECK(vm_bank_alloc(model, 100U, &a) == 0 && a == 0U, "first at 0");
  CHECK(vm_bank_alloc(model, 10U, &b) == 0 && b == 100U, "aligned: %u", b);
  CHECK(vm_bank_alloc(model, 4U, &c) == 0 && c == 112U, "third: %u", c);
  CHECK(vm_bank_free_bytes(model) == (1U << 20) - 116U, "free bytes");
  /* Not running: a removed sound frees at once. */
  vm_sound_define(model, 5U, VM_AHI_M8S, b, 10U);
  CHECK(vm_sound_remove(model, 5U) == 0, "remove");
  CHECK(vm_bank_alloc(model, 12U, &d) == 0 && d == 100U,
        "freed range is reused: %u", d);
  CHECK(vm_bank_alloc(model, 1U << 20, &d) != 0, "exhaustion");
  CHECK(vm_bank_alloc(model, 0U, &d) == 0, "zero bytes still gives a unit");
}

static void test_coalesce(void)
{
  uint32_t o[3];
  uint32_t big;

  setup(1U, 1);
  CHECK(vm_bank_alloc(model, 400U, &o[0]) == 0, "a");
  CHECK(vm_bank_alloc(model, 400U, &o[1]) == 0, "b");
  CHECK(vm_bank_alloc(model, 400U, &o[2]) == 0, "c");
  vm_sound_define(model, 6U, VM_AHI_M8S, o[0], 400U);
  vm_sound_define(model, 7U, VM_AHI_M8S, o[1], 400U);
  vm_sound_define(model, 8U, VM_AHI_M8S, o[2], 400U);
  vm_sound_remove(model, 6U);
  vm_sound_remove(model, 8U);
  vm_sound_remove(model, 7U);
  /* All three neighbours merged with the tail: one range again. */
  CHECK(vm_bank_free_bytes(model) == (1U << 20), "all free again");
  CHECK(vm_bank_alloc(model, 1U << 20, &big) == 0 && big == 0U,
        "one whole range after coalescing");
}

static void test_quarantine(void)
{
  uint32_t off;
  uint32_t again;

  setup(2U, 1);
  vm_start(model, 5000U);
  CHECK(vm_bank_alloc(model, 1U << 20, &off) == 0, "take the whole bank");
  vm_sound_define(model, 4U, VM_AHI_M8S, 0U, 1000U);
  set_sound(model, 0U, 4U, 0U, 0, 1);
  set_freq(model, 0U, 8000U, 1);
  vm_tick(model);
  /* Unload while the voice plays: STOP at the model time, range held. */
  clear_records();
  CHECK(vm_sound_remove(model, 4U) == 0, "remove");
  CHECK(rec_count() == 1U && rec(0)->op == ZZ9K_VOICE_OP_STOP,
        "unload stops the voice: %u records", rec_count());
  CHECK(vm_bank_alloc(model, 16U, &again) != 0, "range is quarantined");
  vm_bank_poll(model, 5000U);
  CHECK(vm_bank_alloc(model, 16U, &again) != 0,
        "horizon below the stop stamp keeps it");
  vm_bank_poll(model, 5000U + 960U + 1U);
  CHECK(vm_bank_alloc(model, 16U, &again) == 0 && again == 0U,
        "horizon past the stop stamp releases it");
}

static void test_level_mapping(void)
{
  uint16_t l;
  uint16_t r;

  vm_volume_to_engine(0x10000, 0x8000, &l, &r);
  CHECK(l == 0x8000U && r == 0x8000U, "centre %04x %04x", l, r);
  vm_volume_to_engine(0x10000, 0, &l, &r);
  CHECK(l == 0xFFFFU && r == 0U, "left %04x %04x", l, r);
  vm_volume_to_engine(0x10000, 0x10000, &l, &r);
  CHECK(l == 0U && r == 0xFFFFU, "right %04x %04x", l, r);
  vm_volume_to_engine(0x8000, 0x8000, &l, &r);
  CHECK(l == 0x4000U && r == 0x4000U, "half volume %04x %04x", l, r);
  vm_volume_to_engine(-0x10000, -0x8000, &l, &r);
  CHECK(l == 0x8000U && r == 0x8000U, "negative uses the absolute value");
  vm_volume_to_engine(0, 0x8000, &l, &r);
  CHECK(l == 0U && r == 0U, "silent");
  CHECK(vm_master_gain(0x10000, 32U) == 8U, "32 channels at 1.0");
  CHECK(vm_master_gain(0x10000, 1U) == 256U, "1 channel at 1.0");
  CHECK(vm_master_gain(0x10000, 8U) == 32U, "8 channels at 1.0");
  CHECK(vm_master_gain(0x20000, 8U) == 64U, "boosted");
  CHECK(vm_master_gain(0xFFFFFFFFU, 1U) == VM_MASTER_MAX_GAIN, "capped");
  CHECK(vm_tick_frames16(50U << 16) == ((uint64_t)960U << 16), "50 Hz tick");
  CHECK(vm_tick_frames16(0U) == ((uint64_t)960U << 16), "no PlayerFreq");
}

static void test_master_record(void)
{
  setup(8U, 1);
  clear_records();
  vm_start(model, 7680U);
  CHECK(is_op(0, ZZ9K_VOICE_OP_MASTER, 0U, 7680U) && rec(0)->arg0 == 32U,
        "first record is MASTER at the lead");
  clear_records();
  set_master(model, 0x20000U);
  vm_tick(model);
  CHECK(rec_count() == 1U && rec(0)->op == ZZ9K_VOICE_OP_MASTER &&
            rec(0)->arg0 == 64U,
        "master volume 2.0 over 8 channels");
}

static void test_disable_blocks_the_fifo(void)
{
  setup(2U, 1);
  vm_start(model, 0U);
  clear_records();
  set_sound(model, 0U, 0U, 0U, 0, 1);
  set_freq(model, 0U, 8000U, 1);
  app.disabled = 1U;
  vm_tick(model);
  CHECK(rec_count() == 0U && vm_fifo_pending(model) == 2U,
        "nothing applied while Disable is nested");
  app.disabled = 0U;
  vm_tick(model);
  CHECK(vm_fifo_pending(model) == 0U && rec_count() >= 2U,
        "applied after Enable");
}

static void test_records_are_nondecreasing(void)
{
  uint32_t t;
  uint32_t last = 0U;
  uint32_t i;
  uint32_t n;
  const ZZ9KVoiceRecord *r;

  setup(8U, 1);
  vm_start(model, 100U);
  for (i = 0U; i < 8U; i++) {
    set_sound(model, (uint16_t)i, (uint16_t)(i & 1U), 0U, (int32_t)(40 + i * 7),
              1);
    set_freq(model, (uint16_t)i, 20000U + i * 3000U, 1);
    set_vol(model, (uint16_t)i, 0x10000, (int32_t)(i * 0x2000), 1);
  }
  app.sound = sound_alternate;
  for (t = 0U; t < 40U; t++) {
    app.freq16 = (50U + (t & 3U) * 5U) << 16;
    vm_tick(model);
  }
  r = vm_records(model, &n);
  CHECK(n >= 20U, "records %u", n);
  for (i = 0U; i < n; i++) {
    CHECK((int32_t)(r[i].stamp - last) >= 0, "stamp decreases at %u", i);
    last = r[i].stamp;
  }
  CHECK(model->overload == 0U, "overload %u", model->overload);
}

static void test_stop_start_resume(void)
{
  setup(2U, 1);
  vm_start(model, 0U);
  set_sound(model, 0U, 1U, 0U, 0, 1);
  set_freq(model, 0U, 48000U, 1);
  set_vol(model, 0U, 0x10000, 0x8000, 1);
  vm_tick(model);
  vm_tick(model);
  vm_stop(model);
  CHECK(rec_count() == 0U, "stop drops the unsent records");
  vm_start(model, 40000U);
  CHECK(is_op(0, ZZ9K_VOICE_OP_MASTER, 0U, 40000U), "master first");
  /* The voice resumes at the model position: 1920 frames in at 48 kHz. */
  {
    uint32_t i;
    int found = 0;

    for (i = 0U; i < rec_count(); i++) {
      if (rec(i)->op == ZZ9K_VOICE_OP_START) {
        found = rec(i)->stamp == 40000U && rec(i)->arg0 == 1024U + 1920U;
      }
    }
    CHECK(found, "START resumes at the saved position");
  }
}

/* More sounds are unloaded, while playback runs, than the quarantine can
 * track. A range the engine may still read must never come back early, and
 * a sound whose range is still held cannot be redefined over it. */
static void test_quarantine_never_reuses_early(void)
{
  const uint32_t total = 1U << 20;
  const uint32_t per_round = 16U;   /* the model of setup() has 16 sounds */
  /* One round past what the quarantine tracks: that round's sounds are held. */
  const uint32_t rounds = VM_MAX_QUARANTINE / per_round + 1U;
  const uint32_t pending = rounds * per_round * 4U;
  uint32_t round;
  uint32_t i;
  uint32_t probe;

  setup(2U, 1);
  vm_start(model, 5000U);
  /* Every round loads the sound table with 4-byte sounds and unloads it
   * again, so more ranges wait for the engine than the quarantine tracks.
   * The last sound of each round plays on channel 0 when it is unloaded. */
  for (round = 1U; round <= rounds; round++) {
    for (i = 0U; i < per_round; i++) {
      uint32_t bank;

      CHECK(vm_bank_alloc(model, 4U, &bank) == 0, "round %u: alloc %u", round,
            i);
      CHECK(vm_sound_define(model, i, VM_AHI_M8S, bank, 4U) == 0,
            "round %u: define %u", round, i);
    }
    set_sound(model, 0U, (uint16_t)(per_round - 1U), 0U, 0, 1);
    set_freq(model, 0U, 48000U, 1);
    vm_run_async(model);
    for (i = 0U; i < per_round; i++)
      CHECK(vm_sound_remove(model, i) == 0, "round %u: remove %u", round, i);
    /* The horizon has not moved: nothing may be handed out again. */
    vm_bank_poll(model, 5000U);
    CHECK(vm_bank_free_bytes(model) == total - round * per_round * 4U,
          "round %u: %u of %u bytes free before the horizon passes", round,
          vm_bank_free_bytes(model), total);
    CHECK(vm_bank_alloc(model, 4U, &probe) == 0 &&
              probe == round * per_round * 4U,
          "round %u: a new sound gets fresh memory (offset %u)", round, probe);
    vm_bank_release(model, probe, 4U);
  }
  /* The tracking is full: the ranges stay with their sounds, and a sound
   * whose range is still held cannot be defined over another range. */
  CHECK(vm_sound_define(model, 0U, VM_AHI_M8S, 0U, 4U) != 0,
        "a held sound is not redefined");
  CHECK(vm_sound_held(model, 0U), "sound 0 is held");
  CHECK(vm_bank_free_bytes(model) == total - pending,
        "held ranges stay allocated");
  vm_bank_poll(model, 5000U + 2U * 7680U);
  CHECK(vm_bank_free_bytes(model) == total,
        "the horizon passing the stops frees every range");
  CHECK(vm_sound_define(model, 0U, VM_AHI_M8S, 0U, 4U) == 0,
        "the sound can be loaded again");
}

/* A pass that ends exactly at the model time, then an immediate pause from
 * another task: the queued sound must still follow, and its SoundFunc must
 * run, when the channel resumes. */
static void test_async_pause_at_pass_end(void)
{
  const ZZ9KVoiceRecord *r;
  uint32_t count;
  uint32_t i;
  uint32_t starts = 0U;
  uint32_t last_arg0 = 0U;
  uint32_t last_arg1 = 0U;

  setup(1U, 1);
  /* Sound 9: 960 frames, one 50 Hz tick at 48 kHz. Sound 1 plays next. */
  CHECK(vm_sound_define(model, 9U, VM_AHI_M8S, 8192U, 960U) == 0, "define 9");
  vm_start(model, 0U);
  set_sound(model, 0U, 9U, 0U, 0, 1);
  set_freq(model, 0U, 48000U, 1);
  set_vol(model, 0U, 0x10000, 0, 1);
  set_sound(model, 0U, 1U, 100U, 200, 0);
  vm_tick(model);
  CHECK(model->now == 960U, "one tick ends where the pass ends");
  CHECK(app.sound_calls == 1U, "SoundFunc at the first pass");

  set_freq(model, 0U, 0U, 1);
  vm_run_async(model);
  clear_records();
  set_freq(model, 0U, 48000U, 1);
  vm_run_async(model);

  r = vm_records(model, &count);
  for (i = 0U; i < count; i++) {
    if (r[i].op == ZZ9K_VOICE_OP_START) {
      starts++;
      last_arg0 = r[i].arg0;
      last_arg1 = r[i].arg1;
    }
  }
  CHECK(starts == 1U && last_arg0 == 1024U + 100U && last_arg1 == 200U,
        "the resume plays the queued pass (START %u at %u, length %u)", starts,
        last_arg0, last_arg1);
  CHECK(app.sound_calls == 2U && model->sound_stamp == 960U,
        "SoundFunc ran at the queued pass (%u calls, last at %u)",
        app.sound_calls, model->sound_stamp);
}

int main(void)
{
  test_imm_set_start_loop();
  test_no_sound_before_freq();
  test_queued_same_view_is_loop();
  test_queued_nosound_silences_at_pass_end();
  test_other_type_is_start_at_boundary();
  test_backward_pass_records();
  test_freq_zero_pause_and_resume();
  test_soundfunc_multi_segment();
  test_allocator();
  test_coalesce();
  test_quarantine();
  test_level_mapping();
  test_master_record();
  test_disable_blocks_the_fifo();
  test_records_are_nondecreasing();
  test_stop_start_resume();
  test_quarantine_never_reuses_early();
  test_async_pause_at_pass_end();
  if (failures) {
    printf("%d failure(s)\n", failures);
    return 1;
  }
  printf("voice_model_test: ok\n");
  return 0;
}
