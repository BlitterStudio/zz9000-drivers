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
  uint32_t fast;      /* a PlayerFunc every 48 to 240 frames, not 480 to 1200 */
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
    c->freq16 = (((c->fast ? 200U : 40U) +
                  (c->tick * 13U) % (c->fast ? 800U : 60U))
                 << 16) +
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
                        uint32_t shorts, uint32_t frames, uint32_t ramp,
                        uint32_t limiter, uint32_t interp)
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
  vm_set_quality(&model_storage, ramp, limiter, interp);
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
    /* The records are applied by hand between two frames: start the frame's
     * chunk the way audio_voice_render() will, so a fade slot taken by a
     * record is dated at this frame and not at the one before. */
    begin_chunk(&engine, out_frame, 0U, 1U);
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
        /* A ramped STOP leaves the voice active while it fades out: the
         * AHI picture of the channel is already silent. */
        int on = v->active != 0U && v->stopping == 0U;

        if (run != on) {
          FAIL("seed %u ch %u frame %u: oracle %s engine %s", seed, ch, f,
               run ? "running" : "idle", on ? "running" : "idle");
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
        vm_sound_define(&model_storage, 0U, VM_AHI_M8S, VM_GUARD_BYTES,
                        32U * 16U);
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
          memset(&bank[VM_GUARD_BYTES + i * 16U], (int)(uint8_t)s8, 16U);
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

/* ---- Unload and reload against the fade tails ----
 *
 * With a ramp, a STOP or a restart lets the old sound fade out of the
 * engine after the AHI picture has dropped it, and the client may unload the
 * sound and load another one at once. Two worlds run the same client calls
 * through the model and a real engine each. The reference world never
 * reuses a bank range: every load goes to fresh bytes. The other world uses
 * the model's allocator and polls it with its engine's horizon, as the
 * driver does, in a bank so small that a freed range comes back at once.
 * Both engines must render the same samples: a fade that reads a range the
 * allocator already handed out again plays the new sound's bytes in one
 * world and the old sound's in the other. Every sound has a zero pad on each
 * side: the cubic reader looks one frame behind the frame it plays, which is
 * the frame before the sound for a forward pass at the start of a sound and
 * the frame after it for a backward pass at the end.
 */

#define HZ_REF_BANK  (6U << 20)
#define HZ_REAL_BANK (20U * 1024U)
#define HZ_PEND      8192U
#define HZ_PCM       512U

typedef struct HzWorld {
  VmModel *m;
  struct audio_voice_engine *eng;
  uint8_t *bank;
  uint32_t bank_bytes;
  uint32_t bump;       /* the reference world's next fresh byte */
  int reuse;           /* uses the model's allocator */
  Client *client;
  ModelRun mr;
  struct audio_voice_cmd *pend;
  uint32_t pend_n;
  int16_t *pcm;
} HzWorld;

static VmModel hz_model[2];
static struct audio_voice_engine hz_engine[2];
static uint8_t hz_ref_bank[HZ_REF_BANK];
static uint8_t hz_real_bank[HZ_REAL_BANK];
static Client hz_client[2];
static struct audio_voice_cmd hz_pend[2][HZ_PEND];
static int16_t hz_pcm[2][2U * HZ_PCM];
static uint8_t hz_loaded[NSOUNDS];
static uint32_t hz_loads;
static uint32_t hz_unloads;
static uint32_t hz_max_tails;
static uint32_t hz_rng;

static uint32_t hz_rand(void)
{
  hz_rng ^= hz_rng << 13;
  hz_rng ^= hz_rng >> 17;
  hz_rng ^= hz_rng << 5;
  return hz_rng;
}

static uint32_t hz_view(uint32_t ahi_type)
{
  switch (ahi_type) {
    case VM_AHI_M8S:
      return VM_VIEW_S8;
    case VM_AHI_M16S:
      return VM_VIEW_S16;
    case VM_AHI_S8S:
      return VM_VIEW_S8S;
    default:
      return VM_VIEW_S16S;
  }
}

static void hz_fill(uint8_t *p, uint32_t bytes, uint32_t id, uint32_t gen)
{
  uint32_t x = (id + 1U) * 2654435761U ^ (gen + 1U) * 40503U;
  uint32_t i;

  for (i = 0U; i < bytes; i++) {
    x = x * 1664525U + 1013904223U;
    p[i] = (uint8_t)(x >> 24);
  }
}

/* 0 when sound id is loaded into the world with fresh content gen. */
static int hz_place(HzWorld *w, uint32_t id, uint32_t gen)
{
  uint32_t view = hz_view(sounds[id].ahi_type);
  uint32_t bytes = sounds[id].frames << view_shift(view);
  uint32_t rounded = (bytes + 3U) & ~3U;
  uint32_t off;

  if (w->reuse) {
    if (vm_sound_held(w->m, id)) {
      return -1;
    }
    if (vm_bank_alloc(w->m, bytes, &off) != 0) {
      return -1;
    }
  } else {
    off = w->bump + VM_GUARD_BYTES;
    w->bump += rounded + 2U * VM_GUARD_BYTES;
    if (w->bump > w->bank_bytes) {
      FAIL("hazard: the reference bank is exhausted");
      return -1;
    }
  }
  /* Every load writes the guards and the rounding bytes, as the driver does,
   * because the range may hold an older sound. */
  memset(w->bank + off - VM_GUARD_BYTES, 0, VM_GUARD_BYTES);
  hz_fill(w->bank + off, bytes, id, gen);
  memset(w->bank + off + bytes, 0, rounded - bytes + VM_GUARD_BYTES);
  if (vm_sound_define(w->m, id, sounds[id].ahi_type, off,
                      sounds[id].frames) != 0) {
    FAIL("hazard: define of sound %u failed", id);
    return -2;
  }
  return 0;
}

static void hz_init_world(HzWorld *w, uint32_t index, uint32_t seed,
                          uint32_t nch, uint32_t stereo, uint32_t ramp,
                          uint32_t limiter, uint32_t interp, int scripted)
{
  VmConfig cfg;
  uint32_t view;

  memset(w, 0, sizeof(*w));
  w->m = &hz_model[index];
  w->eng = &hz_engine[index];
  w->reuse = (int)index;
  w->bank = index == 0U ? hz_ref_bank : hz_real_bank;
  w->bank_bytes = index == 0U ? HZ_REF_BANK : HZ_REAL_BANK;
  if (scripted && index == 1U) {
    /* No room beyond the sounds: a reload can only take the freed range. */
    uint32_t id;

    w->bank_bytes = 0U;
    for (id = 0U; id < (uint32_t)NSOUNDS; id++) {
      w->bank_bytes +=
          (((sounds[id].frames << view_shift(hz_view(sounds[id].ahi_type))) +
            3U) & ~3U) + 2U * VM_GUARD_BYTES;
    }
  }
  w->client = &hz_client[index];
  w->pend = hz_pend[index];
  w->pcm = hz_pcm[index];
  memset(w->bank, 0, w->bank_bytes);
  audio_voice_init(w->eng, w->bank, w->bank_bytes, AUDIO_VOICE_CLOCK_PAL);
  for (view = 0U; view < VM_VIEWS; view++) {
    if (view >= VM_VIEW_S8S && !stereo) {
      continue;
    }
    if (audio_voice_sample_set(w->eng, view, 0U, w->bank_bytes, view) != 0) {
      FAIL("hazard: engine rejected view %u", view);
    }
  }
  client_init(w->client, seed, nch, stereo, 0U);
  /* Odd seeds: a PlayerFunc every few dozen frames, so a restart's fade
   * outlasts the tick the sound is unloaded in. */
  w->client->fast = seed & 1U;
  if (w->client->fast) {
    w->client->freq16 = 400U << 16;
  }
  w->mr.m = w->m;
  w->mr.client = w->client;
  w->mr.api.sound = mr_sound;
  w->mr.api.freq = mr_freq;
  w->mr.api.vol = mr_vol;
  w->mr.api.ctx = &w->mr;
  memset(&cfg, 0, sizeof(cfg));
  cfg.channels = nch;
  cfg.sounds = (uint32_t)NSOUNDS;
  cfg.bank_bytes = w->bank_bytes;
  cfg.stereo_views = stereo;
  /* A scripted world makes only the calls the script pushes. */
  cfg.hooks.player = scripted ? NULL : mr_player;
  cfg.hooks.player_freq = mr_player_freq;
  cfg.hooks.sound = scripted ? NULL : mr_sound_hook;
  cfg.hooks.lock = model_lock;
  cfg.hooks.unlock = model_lock;
  cfg.hooks.user = &w->mr;
  vm_init(w->m, &cfg);
  vm_set_quality(w->m, ramp, limiter, interp);
}

/* Takes the records the model made so far. */
static void hz_feed(HzWorld *w)
{
  uint32_t n;
  uint32_t i;
  const ZZ9KVoiceRecord *r = vm_records(w->m, &n);

  for (i = 0U; i < n; i++) {
    struct audio_voice_cmd *c;

    if (w->pend_n >= HZ_PEND) {
      FAIL("hazard: too many pending records");
      break;
    }
    c = &w->pend[w->pend_n++];
    c->stamp = r[i].stamp;
    c->voice = r[i].voice;
    c->op = r[i].op;
    c->flags = r[i].flags;
    c->arg0 = r[i].arg0;
    c->arg1 = r[i].arg1;
  }
  vm_records_taken(w->m, n);
}

/* Renders `frames` engine frames into w->pcm. */
static void hz_advance(HzWorld *w, uint32_t frames)
{
  size_t used = audio_voice_render(w->eng, w->pcm, frames, w->pend, w->pend_n);

  memmove(w->pend, w->pend + used, (w->pend_n - used) * sizeof(w->pend[0]));
  w->pend_n -= (uint32_t)used;
}

static int hz_allowed(uint32_t id, uint32_t stereo)
{
  uint32_t type = sounds[id].ahi_type;

  return stereo || (type != VM_AHI_S8S && type != VM_AHI_S16S);
}

/* The client loads one unloaded sound into both worlds, when the allocator of
 * the reusing world has room and the sound's range is no longer held. */
static void hz_try_load(HzWorld *w, uint32_t seed, uint32_t stereo,
                        uint32_t *gen)
{
  uint32_t start = hz_rand() % (uint32_t)NSOUNDS;
  uint32_t i;

  for (i = 0U; i < (uint32_t)NSOUNDS; i++) {
    uint32_t id = (start + i) % (uint32_t)NSOUNDS;

    if (hz_loaded[id] || !hz_allowed(id, stereo)) {
      continue;
    }
    if (hz_place(&w[1], id, *gen) == 0) {
      if (hz_place(&w[0], id, *gen) != 0) {
        FAIL("hazard seed %u: the reference world could not load %u", seed,
             id);
      }
      hz_loaded[id] = 1U;
      hz_loads++;
      (*gen)++;
    }
    return;
  }
}

static HzWorld hz_w[2];

/* Renders both engines up to the model time minus lag in short steps. After
 * each step the horizons are polled and the client loads: sound reload_id
 * when it is unloaded, or with reload_id < 0 a random sound half of the
 * time. */
static void hz_catch_up(HzWorld *w, uint32_t lag, int reload_id,
                        uint32_t seed, uint32_t tick, uint32_t ramp,
                        uint32_t stereo, uint32_t *gen,
                        uint32_t errors_before)
{
  int32_t avail = (int32_t)(w[1].m->now - lag - w[1].eng->frame);

  while (avail > 0 && (uint32_t)failures == errors_before) {
    uint32_t chunk = 1U + hz_rand() % 300U;
    uint32_t i;

    if ((int32_t)chunk > avail) {
      chunk = (uint32_t)avail;
    }
    hz_advance(&w[0], chunk);
    hz_advance(&w[1], chunk);
    avail -= (int32_t)chunk;
    for (i = 0U; i < chunk; i++) {
      if (w[0].pcm[2U * i] != w[1].pcm[2U * i] ||
          w[0].pcm[2U * i + 1U] != w[1].pcm[2U * i + 1U]) {
        FAIL("hazard seed %u tick %u (ramp %u): engine frame %u: reference"
             " %d/%d, reusing world %d/%d",
             seed, tick, ramp, w[1].eng->frame - chunk + i, w[0].pcm[2U * i],
             w[0].pcm[2U * i + 1U], w[1].pcm[2U * i], w[1].pcm[2U * i + 1U]);
        break;
      }
    }
    if (w[1].m->tail_n > hz_max_tails) {
      hz_max_tails = w[1].m->tail_n;
    }
    vm_bank_poll(w[0].m, w[0].eng->frame);
    vm_bank_poll(w[1].m, w[1].eng->frame);
    if (reload_id >= 0) {
      if (!hz_loaded[reload_id] && hz_place(&w[1], (uint32_t)reload_id, *gen) == 0) {
        if (hz_place(&w[0], (uint32_t)reload_id, *gen) != 0) {
          FAIL("hazard seed %u: the reference world could not load %d", seed,
               reload_id);
        }
        hz_loaded[reload_id] = 1U;
        hz_loads++;
        (*gen)++;
      }
    } else if ((hz_rand() & 1U) != 0U) {
      hz_try_load(w, seed, stereo, gen);
    }
  }
}

/* The engines render behind the model, as the card does: a record is stamped
 * a lead ahead of the engine, so an unload is stamped ahead of what the
 * engine plays. The engines advance in short steps and the client loads
 * between them, so a range can be handed out again at any frame the horizon
 * allows. */
static int run_hazard(uint32_t seed, uint32_t nch, uint32_t stereo,
                      uint32_t ramp, uint32_t limiter, uint32_t interp,
                      uint32_t ticks)
{
  HzWorld *w = hz_w;
  uint32_t errors_before = (uint32_t)failures;
  uint32_t gen = 1U;
  uint32_t lag = 200U + (seed * 811U) % 2400U;
  uint32_t t;
  uint32_t i;
  uint32_t id;

  hz_rng = 0x2545F491U * (seed + 1U);
  memset(hz_loaded, 0, sizeof(hz_loaded));
  hz_init_world(&w[0], 0U, seed, nch, stereo, ramp, limiter, interp, 0);
  hz_init_world(&w[1], 1U, seed, nch, stereo, ramp, limiter, interp, 0);
  for (id = 0U; id < (uint32_t)NSOUNDS; id++) {
    if (hz_allowed(id, stereo) && hz_place(&w[1], id, 0U) == 0 &&
        hz_place(&w[0], id, 0U) == 0) {
      hz_loaded[id] = 1U;
    }
  }
  vm_start(w[0].m, LEAD);
  vm_start(w[1].m, LEAD);
  hz_feed(&w[0]);
  hz_feed(&w[1]);
  for (t = 0U; t < ticks && (uint32_t)failures == errors_before; t++) {
    /* The client unloads a sound... */
    if ((hz_rand() & 3U) != 0U) {
      uint32_t start = hz_rand() % (uint32_t)NSOUNDS;

      for (i = 0U; i < (uint32_t)NSOUNDS; i++) {
        id = (start + i) % (uint32_t)NSOUNDS;
        if (hz_loaded[id]) {
          vm_sound_remove(w[0].m, id);
          vm_sound_remove(w[1].m, id);
          hz_loaded[id] = 0U;
          hz_unloads++;
          break;
        }
      }
    }
    vm_tick(w[0].m);
    vm_tick(w[1].m);
    hz_feed(&w[0]);
    hz_feed(&w[1]);
    hz_catch_up(w, lag, -1, seed, t, ramp, stereo, &gen, errors_before);
  }
  for (i = 0U; i < 2U; i++) {
    if (w[i].eng->late != 0U || w[i].eng->rejected != 0U) {
      FAIL("hazard seed %u: engine %u late %u rejected %u", seed, i,
           w[i].eng->late, w[i].eng->rejected);
    }
  }
  return (uint32_t)failures == errors_before ? 0 : -1;
}

/* The three ways a fade can outlive its sound, played out on purpose: a
 * restart that hands the sound to a fade slot, a STOP, and a queued pass that
 * the engine would wrap into at the end of the current one. In each the
 * sound is unloaded at once and loaded again with other content as soon as
 * the allocator lets it. */
#define HZS_RESTART 0U
#define HZS_STOP    1U
#define HZS_QUEUED  2U
#define HZS_KINDS   3U

static int run_hazard_script(uint32_t kind, uint32_t ramp, uint32_t lag,
                             uint32_t interp)
{
  HzWorld *w = hz_w;
  uint32_t errors_before = (uint32_t)failures;
  uint32_t gen = 1U;
  uint32_t seed = 1000U + kind * 100U + ramp;
  uint32_t first = kind == HZS_QUEUED ? 1U : 0U;
  uint32_t victim = 0U;
  uint32_t hz = kind == HZS_QUEUED ? 3000U : 30000U;
  uint32_t t;
  uint32_t i;

  hz_rng = 0x9E3779B9U * (seed + 1U);
  memset(hz_loaded, 0, sizeof(hz_loaded));
  hz_init_world(&w[0], 0U, seed, 2U, 1U, ramp, 0U, interp, 1);
  hz_init_world(&w[1], 1U, seed, 2U, 1U, ramp, 0U, interp, 1);
  for (i = 0U; i < (uint32_t)NSOUNDS; i++) {
    if (hz_place(&w[1], i, 0U) == 0 && hz_place(&w[0], i, 0U) == 0) {
      hz_loaded[i] = 1U;
    }
  }
  vm_start(w[0].m, LEAD);
  vm_start(w[1].m, LEAD);
  hz_feed(&w[0]);
  hz_feed(&w[1]);
  for (i = 0U; i < 2U; i++) {
    mr_sound(&w[i].mr, 0U, first, 0U, 0, 1);
    mr_freq(&w[i].mr, 0U, hz, 1);
    mr_vol(&w[i].mr, 0U, 0x10000, 0x8000, 1);
    if (kind == HZS_QUEUED) {
      mr_sound(&w[i].mr, 0U, victim, 0U, 0, 0);
    }
  }
  for (t = 0U; t < 6U; t++) {
    vm_tick(w[0].m);
    vm_tick(w[1].m);
    hz_feed(&w[0]);
    hz_feed(&w[1]);
    hz_catch_up(w, lag, -1, seed, t, ramp, 1U, &gen, errors_before);
  }
  /* Now the sound goes: restarted away from, stopped, or queued and removed. */
  for (i = 0U; i < 2U; i++) {
    if (kind == HZS_RESTART) {
      mr_sound(&w[i].mr, 0U, 1U, 0U, 0, 1);
    } else if (kind == HZS_STOP) {
      mr_sound(&w[i].mr, 0U, VM_NOSOUND, 0U, 0, 1);
    }
    vm_run_async(w[i].m);
    vm_sound_remove(w[i].m, victim);
    hz_feed(&w[i]);
  }
  hz_loaded[victim] = 0U;
  hz_unloads++;
  for (t = 6U; t < 40U && (uint32_t)failures == errors_before; t++) {
    hz_catch_up(w, lag, (int)victim, seed, t, ramp, 1U, &gen, errors_before);
    vm_tick(w[0].m);
    vm_tick(w[1].m);
    hz_feed(&w[0]);
    hz_feed(&w[1]);
  }
  for (i = 0U; i < 2U; i++) {
    if (w[i].eng->late != 0U || w[i].eng->rejected != 0U) {
      FAIL("hazard script %u: engine %u late %u rejected %u", kind, i,
           w[i].eng->late, w[i].eng->rejected);
    }
  }
  return (uint32_t)failures == errors_before ? 0 : -1;
}

static void run_hazard_scripts(void)
{
  static const uint32_t ramps[] = {1U, 64U, 256U, 512U};
  static const uint32_t lags[] = {0U, 64U, 700U, 3000U};
  uint32_t kind;
  uint32_t r;
  uint32_t l;
  uint32_t interp;

  for (kind = 0U; kind < HZS_KINDS; kind++) {
    for (r = 0U; r < sizeof(ramps) / sizeof(ramps[0]); r++) {
      for (l = 0U; l < sizeof(lags) / sizeof(lags[0]); l++) {
        for (interp = 0U; interp < 2U; interp++) {
          if (run_hazard_script(kind, ramps[r], lags[l],
                                interp != 0U ? ZZ9K_VOICE_INTERP_LINEAR : 0U) !=
              0) {
            printf("  script %u ramp %u lag %u\n", kind, ramps[r], lags[l]);
            return;
          }
        }
      }
    }
  }
}

/* ---- Asset isolation ----
 *
 * One asset plays through the real engine in two banks that differ in every
 * byte outside the asset's own allocation (the data and its guards): the
 * neighbours below and above it, and whatever else is in the bank. The
 * neighbours are uploaded, changed in place, unloaded and reloaded into the
 * reused range while the asset plays, with other bytes in each bank. The
 * samples must be identical: nothing the engine reads for the asset may lie
 * outside its allocation. The assets are 1, 2, 3 and larger frames in all four
 * formats, played forward and backward from their first and last frames, whole
 * and in part, with and without a loop over themselves, at rates below, at and
 * above 48 kHz, with nearest, linear and cubic reading, with and without a
 * ramp. */
typedef struct IsoAsset {
  uint32_t type;
  uint32_t frames;
} IsoAsset;

static const IsoAsset iso_assets[] = {
    {VM_AHI_M8S, 1U},   {VM_AHI_M8S, 2U},   {VM_AHI_M8S, 3U},
    {VM_AHI_M8S, 17U},  {VM_AHI_M16S, 1U},  {VM_AHI_M16S, 2U},
    {VM_AHI_M16S, 3U},  {VM_AHI_M16S, 40U}, {VM_AHI_S8S, 1U},
    {VM_AHI_S8S, 2U},   {VM_AHI_S8S, 3U},   {VM_AHI_S16S, 1U},
    {VM_AHI_S16S, 2U},  {VM_AHI_S16S, 3U}, {VM_AHI_S16S, 33U},
};
#define ISO_ASSETS (sizeof(iso_assets) / sizeof(iso_assets[0]))
#define ISO_BANK   4096U
#define ISO_PEND   512U
#define ISO_PCM    512U

static VmModel iso_model;
static struct audio_voice_engine iso_eng[2];
static uint8_t iso_bank[2][ISO_BANK];
static struct audio_voice_cmd iso_pend[2][ISO_PEND];
static uint32_t iso_pend_n[2];
static int16_t iso_pcm[2][2U * ISO_PCM];
static uint32_t iso_rng;

typedef struct IsoNeighbour {
  uint32_t id;
  uint32_t frames;
  uint32_t off;
  int loaded;
} IsoNeighbour;

static uint32_t iso_rand(void)
{
  iso_rng ^= iso_rng << 13;
  iso_rng ^= iso_rng >> 17;
  iso_rng ^= iso_rng << 5;
  return iso_rng;
}

static void iso_fill(uint8_t *p, uint32_t bytes, uint32_t salt)
{
  uint32_t x = salt * 2654435761U + 0x1234567U;
  uint32_t i;

  for (i = 0U; i < bytes; i++) {
    x = x * 1664525U + 1013904223U;
    p[i] = (uint8_t)(x >> 24);
  }
}

/* Loads a sound the way the driver does: the allocation's guards and the
 * rounding bytes are zero, the data is the sound's. salt[w] is the content
 * written into bank w; the asset gets the same in both, the neighbours do
 * not. Returns 0, or -1 when the allocator has no room. */
static int iso_load(uint32_t id, uint32_t type, uint32_t frames,
                    const uint32_t salt[2], uint32_t *off_out)
{
  uint32_t bytes = frames << view_shift(hz_view(type));
  uint32_t rounded = (bytes + 3U) & ~3U;
  uint32_t off;
  uint32_t w;

  if (vm_sound_held(&iso_model, id) ||
      vm_bank_alloc(&iso_model, bytes, &off) != 0) {
    return -1;
  }
  for (w = 0U; w < 2U; w++) {
    memset(iso_bank[w] + off - VM_GUARD_BYTES, 0, VM_GUARD_BYTES);
    iso_fill(iso_bank[w] + off, bytes, salt[w]);
    memset(iso_bank[w] + off + bytes, 0, rounded - bytes + VM_GUARD_BYTES);
  }
  if (vm_sound_define(&iso_model, id, type, off, frames) != 0) {
    FAIL("isolation: define of sound %u failed", id);
    return -1;
  }
  *off_out = off;
  return 0;
}

static void iso_call(uint8_t kind, uint32_t a, int32_t b, uint32_t sound,
                     int imm)
{
  VmCall c;

  memset(&c, 0, sizeof(c));
  c.kind = kind;
  c.imm = (uint8_t)imm;
  c.channel = 0U;
  c.sound = (uint16_t)sound;
  c.a = a;
  c.b = b;
  if (vm_fifo_push(&iso_model, &c) != 0) {
    FAIL("isolation: fifo full");
  }
}

/* Renders both engines up to the model time and compares them. Returns 0 when
 * they agree. */
static int iso_render(uint32_t asset, uint32_t interp, uint32_t ramp,
                      uint32_t tick)
{
  uint32_t n;
  uint32_t i;
  uint32_t w;
  const ZZ9KVoiceRecord *r = vm_records(&iso_model, &n);

  for (w = 0U; w < 2U; w++) {
    for (i = 0U; i < n; i++) {
      struct audio_voice_cmd *c;

      if (iso_pend_n[w] >= ISO_PEND) {
        FAIL("isolation: too many pending records");
        return -1;
      }
      c = &iso_pend[w][iso_pend_n[w]++];
      c->stamp = r[i].stamp;
      c->voice = r[i].voice;
      c->op = r[i].op;
      c->flags = r[i].flags;
      c->arg0 = r[i].arg0;
      c->arg1 = r[i].arg1;
    }
  }
  vm_records_taken(&iso_model, n);
  while ((int32_t)(iso_model.now - iso_eng[0].frame) > 0) {
    uint32_t chunk = iso_model.now - iso_eng[0].frame;

    if (chunk > ISO_PCM) {
      chunk = ISO_PCM;
    }
    for (w = 0U; w < 2U; w++) {
      size_t used = audio_voice_render(&iso_eng[w], iso_pcm[w], chunk,
                                       iso_pend[w], iso_pend_n[w]);

      memmove(iso_pend[w], iso_pend[w] + used,
              (iso_pend_n[w] - used) * sizeof(iso_pend[w][0]));
      iso_pend_n[w] -= (uint32_t)used;
    }
    for (i = 0U; i < 2U * chunk; i++) {
      if (iso_pcm[0][i] != iso_pcm[1][i]) {
        FAIL("isolation asset %u (type %u, %u frames) interp %u ramp %u tick "
             "%u: engine frame %u differs, %d against %d",
             asset, iso_assets[asset].type, iso_assets[asset].frames, interp,
             ramp, tick, iso_eng[0].frame - chunk + i / 2U, iso_pcm[0][i],
             iso_pcm[1][i]);
        return -1;
      }
    }
  }
  return 0;
}

static int run_isolation(uint32_t asset, uint32_t interp, uint32_t ramp)
{
  static const uint32_t rates[] = {300U,   3000U,  8363U, 22050U,
                                   47999U, 48000U, 96000U};
  IsoNeighbour nb[2];
  uint32_t errors_before = (uint32_t)failures;
  uint32_t type = iso_assets[asset].type;
  uint32_t frames = iso_assets[asset].frames;
  uint32_t salt[2];
  uint32_t off;
  uint32_t view;
  uint32_t w;
  uint32_t t;
  VmConfig cfg;

  iso_rng = 0x6D2B79F5U * (asset + 1U) + interp * 977U + ramp;
  memset(&cfg, 0, sizeof(cfg));
  cfg.channels = 1U;
  cfg.sounds = 4U;
  cfg.bank_bytes = ISO_BANK;
  cfg.stereo_views = 1U;
  vm_init(&iso_model, &cfg);
  vm_set_quality(&iso_model, ramp, 0U, interp);
  memset(iso_pend_n, 0, sizeof(iso_pend_n));
  for (w = 0U; w < 2U; w++) {
    iso_fill(iso_bank[w], ISO_BANK, 77U + w);
    audio_voice_init(&iso_eng[w], iso_bank[w], ISO_BANK, AUDIO_VOICE_CLOCK_PAL);
    for (view = 0U; view < VM_VIEWS; view++) {
      if (audio_voice_sample_set(&iso_eng[w], view, 0U, ISO_BANK, view) != 0) {
        FAIL("isolation: engine rejected view %u", view);
      }
    }
  }
  /* Two neighbours that touch the asset's allocation: one below, one above. */
  nb[0].id = 1U;
  nb[0].frames = 24U;
  nb[1].id = 2U;
  nb[1].frames = 20U;
  salt[0] = 5U;
  salt[1] = 6U;
  if (iso_load(nb[0].id, VM_AHI_M8S, nb[0].frames, salt, &nb[0].off) != 0) {
    FAIL("isolation: no room for the lower neighbour");
    return -1;
  }
  salt[0] = salt[1] = 1000U + asset;
  if (iso_load(0U, type, frames, salt, &off) != 0) {
    FAIL("isolation: no room for the asset");
    return -1;
  }
  salt[0] = 7U;
  salt[1] = 8U;
  if (iso_load(nb[1].id, VM_AHI_M8S, nb[1].frames, salt, &nb[1].off) != 0) {
    FAIL("isolation: no room for the upper neighbour");
    return -1;
  }
  nb[0].loaded = nb[1].loaded = 1;
  vm_start(&iso_model, LEAD);
  for (w = 0U; w < 2U; w++) {
    iso_pend_n[w] = 0U;
  }
  iso_call(VM_CALL_VOL, 0x10000U, 0x8000, 0U, 1);
  for (t = 0U; t < 240U && (uint32_t)failures == errors_before; t++) {
    uint32_t pick = iso_rand() % 8U;
    uint32_t o = 0U;
    int32_t len = 0;
    uint32_t i;

    /* The neighbours: unloaded, loaded again into the reused range with
     * other bytes, and rewritten while loaded. */
    if (t % 7U == 3U) {
      for (i = 0U; i < 2U; i++) {
        if (nb[i].loaded) {
          vm_sound_remove(&iso_model, nb[i].id);
          nb[i].loaded = 0;
        }
      }
    }
    for (i = 0U; i < 2U; i++) {
      salt[0] = 2000U + t * 4U + i;
      salt[1] = 3000U + t * 4U + i;
      if (!nb[i].loaded) {
        nb[i].loaded =
            iso_load(nb[i].id, VM_AHI_M8S, nb[i].frames, salt, &nb[i].off) == 0;
      } else if (t % 11U == 5U) {
        for (w = 0U; w < 2U; w++) {
          iso_fill(iso_bank[w] + nb[i].off, nb[i].frames, salt[w]);
        }
      }
    }
    /* The asset: the first and last frames, forward and backward, whole and
     * in part, then a queued pass that loops over the same sound. */
    switch (pick) {
      case 0: o = 0U; len = 0; break;
      case 1: o = frames - 1U; len = -(int32_t)frames; break;
      case 2: o = 0U; len = 1; break;
      case 3: o = frames - 1U; len = -1; break;
      case 4: o = 0U; len = -1; break;
      case 5: o = frames - 1U; len = 0; break;
      case 6: o = iso_rand() % frames; len = (int32_t)(1U + iso_rand() % frames); break;
      default: o = iso_rand() % frames; len = -(int32_t)(1U + iso_rand() % frames); break;
    }
    if (iso_rand() % 4U != 0U) {
      iso_call(VM_CALL_SOUND, o, len, 0U, 1);
      iso_call(VM_CALL_FREQ, rates[iso_rand() % (sizeof(rates) / sizeof(rates[0]))],
               0, 0U, 1);
      if (iso_rand() % 2U != 0U) {
        iso_call(VM_CALL_SOUND, o, len, 0U, 0);
      }
    }
    vm_tick(&iso_model);
    if (iso_render(asset, interp, ramp, t) != 0) {
      break;
    }
    vm_bank_poll(&iso_model, iso_eng[0].frame);
  }
  for (w = 0U; w < 2U; w++) {
    if (iso_eng[w].late != 0U || iso_eng[w].rejected != 0U) {
      FAIL("isolation: engine %u late %u rejected %u", w, iso_eng[w].late,
           iso_eng[w].rejected);
    }
  }
  return (uint32_t)failures == errors_before ? 0 : -1;
}

static void run_isolations(void)
{
  uint32_t asset;
  uint32_t interp;
  uint32_t ramp;

  for (asset = 0U; asset < (uint32_t)ISO_ASSETS; asset++) {
    for (interp = ZZ9K_VOICE_INTERP_NEAREST; interp <= ZZ9K_VOICE_INTERP_CUBIC;
         interp++) {
      for (ramp = 0U; ramp <= 64U; ramp += 64U) {
        if (run_isolation(asset, interp, ramp) != 0) {
          return;
        }
      }
    }
  }
}

typedef struct Quality {
  uint32_t ramp;
  uint32_t limiter;
  uint32_t interp;
} Quality;

/* The first entry is the old modes: no CONFIG record at all. */
static const Quality qualities[] = {
    {0U, 0U, 0U},
    {64U, 0U, 0U},
    {512U, 1U, 0U},
    {1U, 0U, ZZ9K_VOICE_INTERP_CUBIC},
    {256U, 0U, ZZ9K_VOICE_INTERP_LINEAR},
    {0U, 1U, ZZ9K_VOICE_INTERP_CUBIC},
    {37U, 1U, ZZ9K_VOICE_INTERP_NEAREST},
    {480U, 0U, ZZ9K_VOICE_INTERP_CUBIC},
};
#define NQUALITIES (sizeof(qualities) / sizeof(qualities[0]))

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
    {
      const Quality *q = &qualities[seed % NQUALITIES];

      if (run_sequence(seed, nch, stereo, shorts, 60000U, q->ramp, q->limiter,
                       q->interp) != 0) {
        bad++;
        if (bad >= 5U) {
          break;
        }
      }
    }
  }
  if (!trace_on) {
    uint32_t hseeds = seeds < 8U ? 8U : seeds;

    for (seed = 1U; seed <= hseeds && bad < 5U; seed++) {
      /* Even and odd seeds take different qualities: the player speed is
       * the low bit of the seed. */
      const Quality *q = &qualities[(seed >> 1) % NQUALITIES];

      if (run_hazard(seed, 1U + (seed * 5U) % 32U, stereo, q->ramp, q->limiter,
                     q->interp, (seed & 1U) ? 900U : 300U) != 0) {
        bad++;
      }
    }
  }
  if (!trace_on && bad < 5U) {
    run_hazard_scripts();
    run_isolations();
  }
  if (failures) {
    printf("voice_ref_test: %d failure(s)\n", failures);
    return 1;
  }
  printf("voice_ref_test: ok (%u sequences, %u records, %u SoundFunc calls)\n",
         seeds, total_records, total_sound_calls);
  if (!trace_on) {
    printf("voice_ref_test: unload and reload: %u loads, %u unloads, up to %u"
           " fade tails at once\n",
           hz_loads, hz_unloads, hz_max_tails);
  }
  return 0;
}
