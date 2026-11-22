/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Differential test of the AHI channel model against the real voice engine.
 *
 * A pseudo-random "client" drives AHI calls from a PlayerFunc and a
 * SoundFunc. Run 1 sends them through the model; its records go to the real
 * engine (firmware audio_voice.c), which renders frame by frame. Run 2 gives
 * the same client to an independent AHI-semantics frame stepper, the oracle:
 * current pass, next pass, end-of-sound flag, one frame at a time, with the
 * engine's own position arithmetic. After every frame the engine's voices
 * must equal the oracle's channels, and both runs must have called SoundFunc
 * at the same frames in the same order. A model that predicts a pass
 * boundary one frame off, or sends a record that is late, stale or wrong,
 * fails here.
 *
 * Where the engine cannot continue an AHI position (it restarts a voice with
 * a zero fraction) the oracle applies the same rule: a START clears the
 * fraction. Every other position, step and volume is exact.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The engine source is included so the test can call its apply(): every
 * command due at a frame is applied by hand and the voices are compared at
 * the start of the frame, before it is mixed. */
#include "audio_voice.c"
#include "voice_model.h"

#define NCH_MAX 32U
#define BANK_BYTES (256U * 1024U)
#define LEAD 7680U

static int failures;
static uint32_t total_records;
static uint32_t total_sound_calls;

/* AHI_VOICE_TRACE=seed:channel:from:to prints one channel's oracle calls and
 * the model's records for a frame range. */
static int trace_on;
static uint32_t trace_ch;
static uint32_t trace_from;
static uint32_t trace_to;
static uint32_t o_frame;

#define FAIL(...)                                    \
  do {                                               \
    if (failures < 20) {                             \
      printf("FAIL %s:%d: ", __FILE__, __LINE__);    \
      printf(__VA_ARGS__);                           \
      printf("\n");                                  \
    }                                                \
    failures++;                                      \
  } while (0)

/* ---- Shared sound table ---- */

typedef struct TSound {
  uint32_t ahi_type;
  uint32_t frames;
} TSound;

static const TSound sounds[] = {
    {VM_AHI_M8S, 3000U}, {VM_AHI_M8S, 800U},  {VM_AHI_M16S, 2000U},
    {VM_AHI_S8S, 1500U}, {VM_AHI_S16S, 1200U}, {VM_AHI_M16S, 40U},
    {VM_AHI_M8S, 17U},
};
#define NSOUNDS (sizeof(sounds) / sizeof(sounds[0]))

static uint8_t bank[BANK_BYTES];

typedef struct SoundPlace {
  uint8_t view;
  uint32_t base;   /* view frames */
  uint32_t frames;
} SoundPlace;

static SoundPlace place[NSOUNDS];

static uint32_t view_shift(uint32_t view)
{
  return zz9k_voice_format_frame_shift(view);
}

/* ---- The client ---- */

typedef struct Api {
  void (*sound)(void *ctx, uint32_t ch, uint32_t id, uint32_t off, int32_t len,
                int imm);
  void (*freq)(void *ctx, uint32_t ch, uint32_t hz, int imm);
  void (*vol)(void *ctx, uint32_t ch, int32_t vol, int32_t pan, int imm);
  void *ctx;
} Api;

typedef struct Client {
  uint32_t seed;
  uint32_t rng[NCH_MAX];
  uint32_t nch;
  uint32_t tick;
  uint32_t freq16;
  uint32_t stereo;
  uint32_t shorts;
  /* log of SoundFunc calls */
  uint32_t log_stamp[1 << 17];
  uint8_t log_ch[1 << 17];
  uint32_t log_n;
} Client;

static uint32_t rnd(Client *c, uint32_t ch)
{
  uint32_t x = c->rng[ch];

  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  c->rng[ch] = x;
  return x;
}

static void client_init(Client *c, uint32_t seed, uint32_t nch,
                        uint32_t stereo, uint32_t shorts)
{
  uint32_t i;

  memset(c, 0, sizeof(*c));
  c->seed = seed;
  c->nch = nch;
  c->stereo = stereo;
  c->shorts = shorts;
  c->freq16 = 50U << 16;
  for (i = 0U; i < NCH_MAX; i++) {
    c->rng[i] = 0x9E3779B9U * (seed + 1U) + 0x85EBCA6BU * (i + 1U);
    if (c->rng[i] == 0U) {
      c->rng[i] = 1U;
    }
  }
}

static uint32_t pick_sound(Client *c, uint32_t ch)
{
  for (;;) {
    uint32_t s = rnd(c, ch) % (uint32_t)NSOUNDS;

    if (!c->stereo && (sounds[s].ahi_type == VM_AHI_S8S ||
                       sounds[s].ahi_type == VM_AHI_S16S)) {
      continue;
    }
    return s;
  }
}

static void pick_region(Client *c, uint32_t ch, uint32_t id, uint32_t *off,
                        int32_t *len)
{
  uint32_t frames = sounds[id].frames;
  uint32_t o = rnd(c, ch) % frames;
  uint32_t maxlen = c->shorts ? 40U : 600U;
  uint32_t l;

  if (rnd(c, ch) % 4U == 0U) {
    uint32_t room = o + 1U;

    l = 1U + rnd(c, ch) % (room < maxlen ? room : maxlen);
    if (l < 4U && room >= 4U) {
      l = 4U;
    }
    *off = o;
    *len = -(int32_t)l;
  } else {
    uint32_t room = frames - o;

    l = 1U + rnd(c, ch) % (room < maxlen ? room : maxlen);
    if (l < 4U && room >= 4U) {
      l = 4U;
    }
    *off = o;
    *len = (int32_t)l;
  }
}

static uint32_t pick_hz(Client *c, uint32_t ch, int allow_zero)
{
  uint32_t r = rnd(c, ch) % 100U;

  if (allow_zero && r < 6U) {
    return 0U;
  }
  if (r < 15U) {
    return 8000U + rnd(c, ch) % 8000U;
  }
  return 400U + rnd(c, ch) % 47000U;
}

static void pick_vol(Client *c, uint32_t ch, int32_t *vol, int32_t *pan)
{
  *vol = (int32_t)(rnd(c, ch) % 0x10001U);
  *pan = (int32_t)(rnd(c, ch) % 0x10001U);
  if (rnd(c, ch) % 9U == 0U) {
    *vol = -*vol;
  }
}

/* PlayerFunc: a handful of actions on a handful of channels. */
static void client_player(Client *c, const Api *api)
{
  uint32_t ch;

  c->tick++;
  if (c->tick % 7U == 0U) {
    /* PlayerFreq changes with the tempo: not a whole number of frames. */
    c->freq16 = ((40U + (c->tick * 13U) % 60U) << 16) +
                ((c->tick * 7919U) & 0xFFFFU);
  }
  for (ch = 0U; ch < c->nch; ch++) {
    uint32_t r = rnd(c, ch) % 100U;
    uint32_t id;
    uint32_t off;
    int32_t len;
    int32_t vol;
    int32_t pan;

    if (r < 12U) {
      id = pick_sound(c, ch);
      pick_region(c, ch, id, &off, &len);
      pick_vol(c, ch, &vol, &pan);
      api->sound(api->ctx, ch, id, off, len, 1);
      api->freq(api->ctx, ch, pick_hz(c, ch, 1), 1);
      api->vol(api->ctx, ch, vol, pan, 1);
    } else if (r < 18U) {
      api->freq(api->ctx, ch, pick_hz(c, ch, 1), 1);
    } else if (r < 24U) {
      pick_vol(c, ch, &vol, &pan);
      api->vol(api->ctx, ch, vol, pan, 1);
    } else if (r < 32U) {
      id = pick_sound(c, ch);
      pick_region(c, ch, id, &off, &len);
      api->sound(api->ctx, ch, id, off, len, 0);
    } else if (r < 35U) {
      api->freq(api->ctx, ch, pick_hz(c, ch, 1), 0);
    } else if (r < 38U) {
      pick_vol(c, ch, &vol, &pan);
      api->vol(api->ctx, ch, vol, pan, 0);
    } else if (r < 40U) {
      api->sound(api->ctx, ch, VM_NOSOUND, 0U, 0, 1);
    }
  }
}

/* SoundFunc: the next pass of this channel only, never IMM. */
static void client_sound(Client *c, uint32_t ch, const Api *api)
{
  uint32_t r = rnd(c, ch) % 100U;
  uint32_t id;
  uint32_t off;
  int32_t len;
  int32_t vol;
  int32_t pan;

  if (r < 40U) {
    return;
  }
  if (r < 78U) {
    id = pick_sound(c, ch);
    pick_region(c, ch, id, &off, &len);
    api->sound(api->ctx, ch, id, off, len, 0);
  } else if (r < 84U) {
    api->sound(api->ctx, ch, VM_NOSOUND, 0U, 0, 0);
  } else if (r < 92U) {
    api->freq(api->ctx, ch, pick_hz(c, ch, 1), 0);
  } else {
    pick_vol(c, ch, &vol, &pan);
    api->vol(api->ctx, ch, vol, pan, 0);
  }
}

/* ---- Run 1: the model ---- */

typedef struct ModelRun {
  VmModel *m;
  Client *client;
  Api api;
} ModelRun;

static void mr_sound(void *ctx, uint32_t ch, uint32_t id, uint32_t off,
                     int32_t len, int imm)
{
  ModelRun *r = (ModelRun *)ctx;
  VmCall c;

  memset(&c, 0, sizeof(c));
  c.kind = VM_CALL_SOUND;
  c.imm = (uint8_t)imm;
  c.channel = (uint16_t)ch;
  c.sound = (uint16_t)id;
  c.a = off;
  c.b = len;
  if (id == VM_NOSOUND) {
    c.sound = VM_NOSOUND;
  }
  if (vm_fifo_push(r->m, &c) != 0) {
    FAIL("fifo full");
  }
}

static void mr_freq(void *ctx, uint32_t ch, uint32_t hz, int imm)
{
  ModelRun *r = (ModelRun *)ctx;
  VmCall c;

  memset(&c, 0, sizeof(c));
  c.kind = VM_CALL_FREQ;
  c.imm = (uint8_t)imm;
  c.channel = (uint16_t)ch;
  c.a = hz;
  vm_fifo_push(r->m, &c);
}

static void mr_vol(void *ctx, uint32_t ch, int32_t vol, int32_t pan, int imm)
{
  ModelRun *r = (ModelRun *)ctx;
  VmCall c;

  memset(&c, 0, sizeof(c));
  c.kind = VM_CALL_VOL;
  c.imm = (uint8_t)imm;
  c.channel = (uint16_t)ch;
  c.a = (uint32_t)vol;
  c.b = pan;
  vm_fifo_push(r->m, &c);
}

static void mr_player(void *user)
{
  ModelRun *r = (ModelRun *)user;

  client_player(r->client, &r->api);
}

static uint32_t mr_player_freq(void *user)
{
  return ((ModelRun *)user)->client->freq16;
}

static void mr_sound_hook(void *user, uint16_t ch)
{
  ModelRun *r = (ModelRun *)user;
  Client *c = r->client;

  if (c->log_n < (1U << 17)) {
    c->log_stamp[c->log_n] = r->m->sound_stamp;
    c->log_ch[c->log_n] = (uint8_t)ch;
    c->log_n++;
  }
  client_sound(c, ch, &r->api);
}

/* ---- Run 2: the oracle ---- */

typedef struct OPass {
  int sound_ok;
  int freq_ok;
  uint32_t view;
  uint32_t bw;
  uint32_t lo;
  uint32_t n;
  uint32_t hz;
  uint16_t vl;
  uint16_t vr;
} OPass;

typedef struct OChan {
  OPass cur;
  OPass next;
  uint32_t idx;
  uint32_t frac;
  int eos;
} OChan;

typedef struct Oracle {
  OChan ch[NCH_MAX];
  uint32_t nch;
  Client *client;
  Api api;
  uint32_t frame;
} Oracle;

static void geometry(OPass *p, uint32_t id, uint32_t off, int32_t len)
{
  const SoundPlace *s = &place[id];

  p->view = s->view;
  if (len == 0) {
    len = (int32_t)s->frames;
  }
  if (len < 0) {
    p->bw = 1U;
    p->n = (uint32_t)-len;
    p->lo = s->base + off + (uint32_t)(len + 1);
  } else {
    p->bw = 0U;
    p->n = (uint32_t)len;
    p->lo = s->base + off;
  }
}

static uint32_t first_of(const OPass *p)
{
  return p->bw ? p->lo + p->n - 1U : p->lo;
}

static int running(const OPass *p)
{
  return p->sound_ok && p->freq_ok;
}

static void o_sound(void *ctx, uint32_t ch, uint32_t id, uint32_t off,
                    int32_t len, int imm)
{
  Oracle *o = (Oracle *)ctx;
  OChan *c = &o->ch[ch];

  if (trace_on && ch == trace_ch && o_frame >= trace_from && o_frame <= trace_to) {
    printf("  f=%u oracle sound id=%u off=%u len=%d imm=%d\n", o_frame, id,
           off, len, imm);
  }

  if (id == VM_NOSOUND) {
    c->next.sound_ok = 0;
    if (imm) {
      c->cur.sound_ok = 0;
      c->eos = 1;
    }
    return;
  }
  geometry(&c->next, id, off, len);
  c->next.sound_ok = 1;
  if (imm) {
    c->cur.sound_ok = 1;
    c->cur.view = c->next.view;
    c->cur.bw = c->next.bw;
    c->cur.lo = c->next.lo;
    c->cur.n = c->next.n;
    c->idx = first_of(&c->cur);
    c->frac = 0U;
    c->eos = 1;
  }
}

static void o_freq(void *ctx, uint32_t ch, uint32_t hz, int imm)
{
  Oracle *o = (Oracle *)ctx;
  OChan *c = &o->ch[ch];

  if (trace_on && ch == trace_ch && o_frame >= trace_from && o_frame <= trace_to) {
    printf("  f=%u oracle freq hz=%u imm=%d\n", o_frame, hz, imm);
  }

  c->next.hz = hz;
  c->next.freq_ok = hz != 0U;
  if (imm) {
    int was = running(&c->cur);

    c->cur.hz = hz;
    c->cur.freq_ok = hz != 0U;
    if (!was && running(&c->cur)) {
      c->frac = 0U; /* a resume is a START */
    }
  }
}

static void o_vol(void *ctx, uint32_t ch, int32_t vol, int32_t pan, int imm)
{
  Oracle *o = (Oracle *)ctx;
  OChan *c = &o->ch[ch];

  if (trace_on && ch == trace_ch && o_frame >= trace_from && o_frame <= trace_to) {
    printf("  f=%u oracle vol vol=%d pan=%d imm=%d\n", o_frame, vol, pan, imm);
  }
  uint16_t l;
  uint16_t r;

  vm_levels(0x10000U, o->nch, vol, pan, &l, &r);
  c->next.vl = l;
  c->next.vr = r;
  if (imm) {
    c->cur.vl = l;
    c->cur.vr = r;
  }
}

/* One frame of mixing for channel c: the AHI mixer's pass handling with the
 * engine's position arithmetic. */
static void o_advance(OChan *c)
{
  uint32_t step;
  uint32_t carry = 0U;
  int ended = 0;

  if (!running(&c->cur)) {
    return;
  }
  step = zz9k_voice_rate_step(c->cur.hz);
  if (!c->cur.bw) {
    uint32_t f = c->frac + (step & 0xFFFFU);

    c->idx += (step >> 16) + (f >> 16);
    c->frac = f & 0xFFFFU;
    if (c->idx >= c->cur.lo + c->cur.n) {
      carry = c->idx - (c->cur.lo + c->cur.n);
      ended = 1;
    }
  } else {
    uint32_t sub = step & 0xFFFFU;
    uint32_t ip = step >> 16;
    int below;

    if (c->frac < sub) {
      c->frac = c->frac + 0x10000U - sub;
      ip++;
    } else {
      c->frac -= sub;
    }
    below = ip > c->idx;
    if (below) {
      c->idx = 0U;
      c->frac = 0U;
    } else {
      c->idx -= ip;
    }
    if (below || c->idx < c->cur.lo) {
      carry = c->cur.lo - c->idx;
      ended = 1;
    }
  }
  if (ended) {
    OPass old = c->cur;
    int continuous;

    c->cur = c->next;
    c->eos = 1;
    if (c->cur.sound_ok && carry >= c->cur.n) {
      carry = 0U;
    }
    continuous = running(&c->cur) && old.view == c->cur.view &&
                 old.bw == c->cur.bw &&
                 (!old.bw || (old.lo == c->cur.lo && old.n == c->cur.n));
    if (c->cur.sound_ok) {
      c->idx = c->cur.bw ? first_of(&c->cur) - carry : c->cur.lo + carry;
    }
    if (!continuous) {
      c->frac = 0U;
    }
  }
}

/* ---- Harness ---- */

typedef struct Rec {
  struct audio_voice_cmd *v;
  uint32_t n;
  uint32_t cap;
} Rec;

static void rec_add(Rec *r, const ZZ9KVoiceRecord *x)
{
  struct audio_voice_cmd *c;

  if (r->n == r->cap) {
    r->cap = r->cap ? r->cap * 2U : 1024U;
    r->v = (struct audio_voice_cmd *)realloc(r->v, r->cap * sizeof(*r->v));
  }
  c = &r->v[r->n++];
  c->stamp = x->stamp;
  c->voice = x->voice;
  c->op = x->op;
  c->flags = x->flags;
  c->arg0 = x->arg0;
  c->arg1 = x->arg1;
}

static VmModel model_storage;
static struct audio_voice_engine engine;
static int16_t out_frame[2];
static Oracle oracle;
static Client client_m;
static Client client_o;

static int setup_engine(uint32_t stereo)
{
  uint32_t i;

  for (i = 0U; i < BANK_BYTES; i++) {
    bank[i] = (uint8_t)((i * 2654435761U) >> 24);
  }
  audio_voice_init(&engine, bank, BANK_BYTES, AUDIO_VOICE_CLOCK_PAL);
  for (i = 0U; i < VM_VIEWS; i++) {
    int rc;

    if (i >= VM_VIEW_S8S && !stereo) {
      continue;
    }
    rc = audio_voice_sample_set(&engine, i, 0U, BANK_BYTES, i);
    if (rc != 0) {
      return -1;
    }
  }
  return 0;
}

static void setup_sounds(VmModel *m, uint32_t stereo)
{
  uint32_t i;
  uint32_t off = 0U;

  for (i = 0U; i < NSOUNDS; i++) {
    uint32_t bytes;
    uint32_t view;
    uint32_t bank_off;

    switch (sounds[i].ahi_type) {
      case VM_AHI_M8S:
        view = VM_VIEW_S8;
        break;
      case VM_AHI_M16S:
        view = VM_VIEW_S16;
        break;
      case VM_AHI_S8S:
        view = VM_VIEW_S8S;
        break;
      default:
        view = VM_VIEW_S16S;
        break;
    }
    if (!stereo && view >= VM_VIEW_S8S) {
      continue;
    }
    bytes = sounds[i].frames << view_shift(view);
    if (vm_bank_alloc(m, bytes, &bank_off) != 0) {
      FAIL("bank alloc");
      return;
    }
    vm_sound_define(m, i, sounds[i].ahi_type, bank_off, sounds[i].frames);
    place[i].view = (uint8_t)view;
    place[i].base = bank_off >> view_shift(view);
    place[i].frames = sounds[i].frames;
    off += bytes;
  }
  (void)off;
}

static void model_lock(void *u)
{
  (void)u;
}

static int run_sequence(uint32_t seed, uint32_t nch, uint32_t stereo,
                        uint32_t shorts, uint32_t frames)
{
  ModelRun mr;
  VmConfig cfg;
  Rec recs;
  uint32_t used;
  uint32_t f;
  uint32_t end = LEAD + frames;
  uint32_t errors_before = (uint32_t)failures;
  uint64_t t;
  Oracle *o = &oracle;
  uint32_t oi;

  memset(&recs, 0, sizeof(recs));
  if (setup_engine(stereo) != 0) {
    FAIL("engine rejected the views");
    return -1;
  }

  /* ---- Run 1: model -> records ---- */
  memset(&cfg, 0, sizeof(cfg));
  cfg.channels = nch;
  cfg.sounds = (uint32_t)NSOUNDS;
  cfg.bank_bytes = BANK_BYTES;
  cfg.stereo_views = stereo;
  cfg.hooks.player = mr_player;
  cfg.hooks.player_freq = mr_player_freq;
  cfg.hooks.sound = mr_sound_hook;
  cfg.hooks.lock = model_lock;
  cfg.hooks.unlock = model_lock;
  cfg.hooks.user = &mr;
  vm_init(&model_storage, &cfg);
  setup_sounds(&model_storage, stereo);
  client_init(&client_m, seed, nch, stereo, shorts);
  mr.m = &model_storage;
  mr.client = &client_m;
  mr.api.sound = mr_sound;
  mr.api.freq = mr_freq;
  mr.api.vol = mr_vol;
  mr.api.ctx = &mr;

  vm_start(&model_storage, LEAD);
  {
    uint32_t n;
    const ZZ9KVoiceRecord *r = vm_records(&model_storage, &n);
    uint32_t i;

    for (i = 0U; i < n; i++) {
      rec_add(&recs, &r[i]);
    }
    vm_records_taken(&model_storage, n);
  }
  while ((int32_t)(model_storage.now - end) < 2000) {
    uint32_t n;
    const ZZ9KVoiceRecord *r;
    uint32_t i;

    vm_tick(&model_storage);
    r = vm_records(&model_storage, &n);
    for (i = 0U; i < n; i++) {
      rec_add(&recs, &r[i]);
    }
    vm_records_taken(&model_storage, n);
  }
  if (model_storage.overload != 0U) {
    FAIL("seed %u: walk overload %u", seed, model_storage.overload);
  }
  if (trace_on) {
    uint32_t i;

    for (i = 0U; i < recs.n; i++) {
      const struct audio_voice_cmd *r = &recs.v[i];

      if ((r->voice == trace_ch || r->op == AUDIO_VOICE_OP_MASTER) &&
          r->stamp >= trace_from && r->stamp <= trace_to) {
        printf("  stamp=%u model rec op=%u flags=%u a0=%u a1=%u\n", r->stamp,
               r->op, r->flags, r->arg0, r->arg1);
      }
    }
  }

  /* ---- Run 2: oracle set up ---- */
  memset(o, 0, sizeof(*o));
  o->nch = nch;
  client_init(&client_o, seed, nch, stereo, shorts);
  o->client = &client_o;
  o->api.sound = o_sound;
  o->api.freq = o_freq;
  o->api.vol = o_vol;
  o->api.ctx = o;
  t = (uint64_t)LEAD << 16;
  used = 0U;

  for (f = 0U; f < end; f++) {
    uint32_t ch;

    o_frame = f;

    if (f >= LEAD) {
      /* Tick at the start of this frame? */
      while ((uint32_t)(t >> 16) == f) {
        client_player(&client_o, &o->api);
        t += vm_tick_frames16(client_o.freq16);
      }
      for (oi = 0U; oi < nch; oi++) {
        OChan *c = &o->ch[oi];

        if (c->eos) {
          c->eos = 0;
          if (client_o.log_n < (1U << 17)) {
            client_o.log_stamp[client_o.log_n] = f;
            client_o.log_ch[client_o.log_n] = (uint8_t)oi;
            client_o.log_n++;
          }
          client_sound(&client_o, oi, &o->api);
        }
      }
    }
    /* The engine applies the commands stamped for this frame, in ring order,
     * before it mixes the frame. Do that by hand, with the engine's own
     * apply(), so the voices can be compared at the start of the frame. */
    while (used < recs.n && (int32_t)(recs.v[used].stamp - f) <= 0) {
      if (recs.v[used].stamp != f) {
        FAIL("seed %u: record stamped %u applied at frame %u", seed,
             recs.v[used].stamp, f);
        goto done;
      }
      apply(&engine, &recs.v[used]);
      used++;
    }
    if (f >= LEAD) {
      for (ch = 0U; ch < nch; ch++) {
        const OChan *c = &o->ch[ch];
        const struct audio_voice *v = &engine.voice[ch];
        int run = running(&c->cur);

        if (run != (v->active != 0U)) {
          FAIL("seed %u ch %u frame %u: oracle %s engine %s", seed, ch, f,
               run ? "running" : "idle", v->active ? "running" : "idle");
          goto done;
        }
        if (run) {
          if (v->pos != c->idx || v->frac != c->frac ||
              v->step != zz9k_voice_rate_step(c->cur.hz) ||
              v->vol_l != c->cur.vl || v->vol_r != c->cur.vr ||
              (v->backward != 0U) != (c->cur.bw != 0U)) {
            FAIL("seed %u ch %u frame %u: oracle idx %u frac %u step %u vol "
                 "%u/%u bw %u; engine pos %u frac %u step %u vol %u/%u bw %u",
                 seed, ch, f, c->idx, c->frac,
                 zz9k_voice_rate_step(c->cur.hz), c->cur.vl, c->cur.vr,
                 c->cur.bw, v->pos, v->frac, v->step, v->vol_l, v->vol_r,
                 v->backward);
            goto done;
          }
        }
      }
    }
    audio_voice_render(&engine, out_frame, 1U, NULL, 0U);
    if (f >= LEAD) {
      for (oi = 0U; oi < nch; oi++) {
        o_advance(&o->ch[oi]);
      }
    }
  }
done:
  if (engine.late != 0U || engine.rejected != 0U) {
    FAIL("seed %u: engine late %u rejected %u", seed, engine.late,
         engine.rejected);
  }
  {
    uint32_t a = 0U;
    uint32_t b = 0U;

    /* SoundFunc calls before the end frame, same frames, same order. */
    for (;;) {
      while (a < client_m.log_n &&
             (int32_t)(client_m.log_stamp[a] - end) >= 0) {
        a++;
      }
      if (a >= client_m.log_n || b >= client_o.log_n) {
        break;
      }
      if (client_m.log_stamp[a] != client_o.log_stamp[b] ||
          client_m.log_ch[a] != client_o.log_ch[b]) {
        FAIL("seed %u: SoundFunc #%u model ch %u at %u, oracle ch %u at %u",
             seed, b, client_m.log_ch[a], client_m.log_stamp[a],
             client_o.log_ch[b], client_o.log_stamp[b]);
        break;
      }
      a++;
      b++;
    }
  }
  total_records += recs.n;
  total_sound_calls += client_o.log_n;
  if (nch >= 4U && client_o.log_n < 100U) {
    FAIL("seed %u: only %u SoundFunc calls; the sequence did not exercise "
         "anything",
         seed, client_o.log_n);
  }
  free(recs.v);
  return (uint32_t)failures == errors_before ? 0 : -1;
}

/* ---- Levels: the engine's output against the ideal ahi.device mix ----
 *
 * N channels each play a constant sample at a random volume and pan. The
 * ideal output of ahi.device's mixer is the sum of sample * left volume,
 * divided by the channel count and scaled by AHIET_MASTERVOLUME. The engine
 * path (voice volumes, whole 1/256 master gain with the residual folded into
 * the voices) must match it to within a couple of LSB for every channel
 * count, the 32-channel master of 1/32 included. */
static int test_levels(void)
{
  static const uint32_t counts[] = {1U, 2U, 3U, 7U, 8U, 16U, 20U, 24U, 32U};
  static const uint32_t masters[] = {0x10000U, 0x20000U, 0x8000U, 0x1999U};
  int32_t worst = 0;
  uint32_t k;
  uint32_t mi;
  uint32_t trial;
  uint32_t rng = 12345U;

  for (k = 0U; k < sizeof(counts) / sizeof(counts[0]); k++) {
    uint32_t n = counts[k];

    for (mi = 0U; mi < sizeof(masters) / sizeof(masters[0]); mi++) {
      uint32_t master = masters[mi];

      for (trial = 0U; trial < 40U; trial++) {
        VmConfig cfg;
        Rec recs;
        double ideal_l = 0.0;
        double ideal_r = 0.0;
        int16_t out[32 * 2];
        uint32_t i;
        int32_t el;
        int32_t er;

        memset(&recs, 0, sizeof(recs));
        memset(&cfg, 0, sizeof(cfg));
        cfg.channels = n;
        cfg.sounds = 4U;
        cfg.bank_bytes = BANK_BYTES;
        vm_init(&model_storage, &cfg);
        vm_sound_define(&model_storage, 0U, VM_AHI_M8S, 0U, 32U * 16U);
        if (setup_engine(0U) != 0) {
          FAIL("levels: engine");
          return -1;
        }
        {
          VmCall c;

          memset(&c, 0, sizeof(c));
          c.kind = VM_CALL_MASTER;
          c.a = master;
          vm_fifo_push(&model_storage, &c);
        }
        for (i = 0U; i < n; i++) {
          int32_t s8;
          int32_t vol;
          int32_t pan;
          uint32_t vl;
          uint32_t vr;
          VmCall c;

          rng = rng * 1664525U + 1013904223U;
          s8 = (int32_t)((rng >> 24) & 0xFFU) - 128;
          rng = rng * 1664525U + 1013904223U;
          vol = (int32_t)(0x4000U + (rng >> 16) % 0xC001U);
          rng = rng * 1664525U + 1013904223U;
          pan = (int32_t)((rng >> 16) % 0x10001U);
          memset(&bank[i * 16U], (int)(uint8_t)s8, 16U);
          memset(&c, 0, sizeof(c));
          c.kind = VM_CALL_SOUND;
          c.imm = 1U;
          c.channel = (uint16_t)i;
          c.sound = 0U;
          c.a = i * 16U;
          c.b = 16;
          vm_fifo_push(&model_storage, &c);
          memset(&c, 0, sizeof(c));
          c.kind = VM_CALL_FREQ;
          c.imm = 1U;
          c.channel = (uint16_t)i;
          c.a = 48000U;
          vm_fifo_push(&model_storage, &c);
          memset(&c, 0, sizeof(c));
          c.kind = VM_CALL_VOL;
          c.imm = 1U;
          c.channel = (uint16_t)i;
          c.a = (uint32_t)vol;
          c.b = pan;
          vm_fifo_push(&model_storage, &c);
          /* ahi.device: left = ((v/2) * ((1-pan)/2)) << 2, 16.16. */
          vl = (((uint32_t)vol >> 1) * ((0x10000U - (uint32_t)pan) >> 1)) >> 14;
          vr = (((uint32_t)vol >> 1) * ((uint32_t)pan >> 1)) >> 14;
          ideal_l += (double)(s8 * 256) * (double)vl / 65536.0;
          ideal_r += (double)(s8 * 256) * (double)vr / 65536.0;
        }
        ideal_l = ideal_l / (double)n * (double)master / 65536.0;
        ideal_r = ideal_r / (double)n * (double)master / 65536.0;
        if (ideal_l > 32767.0) {
          ideal_l = 32767.0;
        }
        if (ideal_l < -32768.0) {
          ideal_l = -32768.0;
        }
        if (ideal_r > 32767.0) {
          ideal_r = 32767.0;
        }
        if (ideal_r < -32768.0) {
          ideal_r = -32768.0;
        }
        vm_start(&model_storage, 8U);
        vm_tick(&model_storage);
        {
          uint32_t cnt;
          const ZZ9KVoiceRecord *r = vm_records(&model_storage, &cnt);

          for (i = 0U; i < cnt; i++) {
            rec_add(&recs, &r[i]);
          }
          vm_records_taken(&model_storage, cnt);
        }
        audio_voice_render(&engine, out, 16U, recs.v, recs.n);
        el = (int32_t)out[2 * 12] -
             (int32_t)(ideal_l + (ideal_l < 0 ? -0.5 : 0.5));
        er = (int32_t)out[2 * 12 + 1] -
             (int32_t)(ideal_r + (ideal_r < 0 ? -0.5 : 0.5));
        if (el < 0) {
          el = -el;
        }
        if (er < 0) {
          er = -er;
        }
        /* Each voice truncates once before the master gain scales it, so a
         * boost above 1.0 scales that truncation too. */
        if ((el > (master > 0x10000U ? 4 : 2) ||
             er > (master > 0x10000U ? 4 : 2)) &&
            failures < 6) {
          FAIL("levels: %u channels master %x: error L %d R %d (ideal %.1f/%.1f, "
               "engine %d/%d)",
               n, master, el, er, ideal_l, ideal_r, out[24], out[25]);
        }
        if (el > worst) {
          worst = el;
        }
        if (er > worst) {
          worst = er;
        }
        free(recs.v);
      }
    }
  }
  printf("voice_ref_test: level worst error %d LSB\n", (int)worst);
  return failures ? -1 : 0;
}

int main(int argc, char **argv)
{
  uint32_t seeds = argc > 1 ? (uint32_t)atoi(argv[1]) : 100U;
  /* "mono" runs without the stereo views, for an engine that predates them. */
  uint32_t stereo = (argc > 2 && strcmp(argv[2], "mono") == 0) ? 0U : 1U;
  uint32_t seed;
  uint32_t bad = 0U;
  uint32_t trace_seed = 0U;
  const char *tr = getenv("AHI_VOICE_TRACE");

  if (tr && sscanf(tr, "%u:%u:%u:%u", &trace_seed, &trace_ch, &trace_from,
                   &trace_to) == 4) {
    trace_on = 1;
  }
  if (!trace_on) {
    test_levels();
  }
  for (seed = 1U; seed <= seeds; seed++) {
    uint32_t nch = 1U + (seed * 7U) % 32U;
    uint32_t shorts = (seed % 5U) == 0U;

    if (trace_on && seed != trace_seed) {
      continue;
    }
    if (run_sequence(seed, nch, stereo, shorts, 60000U) != 0) {
      bad++;
      if (bad >= 5U) {
        break;
      }
    }
  }
  if (failures) {
    printf("voice_ref_test: %d failure(s)\n", failures);
    return 1;
  }
  printf("voice_ref_test: ok (%u sequences, %u records, %u SoundFunc calls)\n",
         seeds, total_records, total_sound_calls);
  return 0;
}
