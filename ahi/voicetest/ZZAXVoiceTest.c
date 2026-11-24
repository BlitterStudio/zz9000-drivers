/*
 * ZZAXVoiceTest: a low-level AHI client for the ZZ9000AX hardware-mixing
 * AudioModes ($CAFE0002, $CAFE0003 and $CAFE0004) and, for comparison, any
 * software mode.
 *
 * It drives AHI the way a tracker does: one AHIST_SAMPLE sound holding the
 * whole sample set, AHI_SetSound offsets into it, a PlayerFunc for timing and
 * a SoundFunc for loops. The hooks stay tiny: they call AHI functions and
 * touch this program's own memory, nothing else. AHISF_IMM follows the AHI
 * developer guide: set outside hooks, optional in the PlayerFunc, never set
 * in the SoundFunc.
 *
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <devices/ahi.h>
#include <devices/timer.h>
#include <dos/dos.h>
#include <dos/var.h>
#include <exec/memory.h>
#include <proto/ahi.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/timer.h>
#include <utility/hooks.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEMPLATE "MODE,SCENARIO,CHANNELS/K/N,SECONDS/K/N,CYCLES/K/N"

#define MODE_HW_NEAREST 0xCAFE0002UL
#define MODE_HW_LINEAR  0xCAFE0003UL
#define MODE_HW_CUBIC   0xCAFE0004UL
#define DEFAULT_MODE    MODE_HW_NEAREST
#define MIX_FREQ        48000UL
#define STATS_VAR       "ZZ9K_AHI_VOICE_STATS"

#define MAX_CH          32U
#define SOFT_MAX_CH     8U
#define DEFAULT_SECONDS 5UL
#define MAX_SECONDS     600UL
#define DEFAULT_CYCLES  50UL
#define MAX_CYCLES      2000UL
#define SCENARIOS       10

#define EXIT_ALLOC      10
#define EXIT_FAILED     5
#define EXIT_USAGE      20

#define VOL_FULL        0x10000L
#define VOL_HALF        0x8000L
#define PAN_LEFT        0L
#define PAN_CENTER      0x8000L
#define PAN_RIGHT       0x10000L

#define AMP             0x6000UL
#define CYCLE_FRAMES    64UL
#define CYCLE_STEP      1024UL      /* one cycle in 64 frames, 1/65536 cycle units */
#define BASE_RATE_HZ    7040UL      /* sample rate of semitone 0 of the 64-frame cycle */

#define PLAYER_HZ       50UL
#define SND_LOG_MAX     256U

static const char version[] __attribute__((used)) =
    "\0$VER: ZZAXVoiceTest 1.0 (22.11.2026)";

struct Device *TimerBase;
struct Library *AHIBase;

/* Quarter wave, 32767 * sin(i * pi / 128) for i = 0..65. */
static const WORD sine_q[66] = {
  0, 804, 1608, 2410, 3212, 4011, 4808, 5602, 6393, 7179, 7962,
  8739, 9512, 10278, 11039, 11793, 12539, 13279, 14010, 14732, 15446, 16151,
  16846, 17530, 18204, 18868, 19519, 20159, 20787, 21403, 22005, 22594, 23170,
  23731, 24279, 24811, 25329, 25832, 26319, 26790, 27245, 27683, 28105, 28510,
  28898, 29268, 29621, 29956, 30273, 30571, 30852, 31113, 31356, 31580, 31785,
  31971, 32137, 32285, 32412, 32521, 32609, 32678, 32728, 32757, 32767, 32757
};

/* 65536 * 2^(i / 12): equal-tempered semitones. */
static const ULONG semitone[12] = {
  65536, 69433, 73562, 77936, 82570, 87480,
  92682, 98193, 104032, 110218, 116772, 123715
};

enum { R_PASS = 0, R_FAIL = 1, R_NOALLOC = 2 };

struct options {
  ULONG mode;
  BOOL voice;           /* a hardware-mixing mode of the ZZ9000AX driver */
  ULONG channels;       /* 0: scenario default */
  ULONG seconds;
  ULONG cycles;
};

static struct options opt;
static BOOL g_break;
static char g_why[200];

/* Statistics accumulated over every AHI_FreeAudio of the running scenario. */
static ULONG g_stats_bad;

/* ------------------------------------------------------------------ */
/* Output                                                              */
/* ------------------------------------------------------------------ */

static void out(const char *fmt, ...)
{
  va_list ap;

  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
  fflush(stdout);
}

static int why(int rc, const char *fmt, ...)
{
  va_list ap;

  va_start(ap, fmt);
  vsnprintf(g_why, sizeof(g_why), fmt, ap);
  va_end(ap);
  return rc;
}

static const char *err_name(ULONG err)
{
  switch (err) {
  case AHIE_OK: return "AHIE_OK";
  case AHIE_NOMEM: return "AHIE_NOMEM";
  case AHIE_BADSOUNDTYPE: return "AHIE_BADSOUNDTYPE";
  case AHIE_BADSAMPLETYPE: return "AHIE_BADSAMPLETYPE";
  case AHIE_ABORTED: return "AHIE_ABORTED";
  case AHIE_UNKNOWN: return "AHIE_UNKNOWN";
  case AHIE_HALFDUPLEX: return "AHIE_HALFDUPLEX";
  default: return "unknown error";
  }
}

/* ------------------------------------------------------------------ */
/* Time                                                                */
/* ------------------------------------------------------------------ */

static struct MsgPort *g_tport;
static struct timerequest *g_treq;
static ULONG g_efreq;

static BOOL timer_open(void)
{
  struct EClockVal ev;

  if (g_treq) return TRUE;
  g_tport = CreateMsgPort();
  if (!g_tport) return FALSE;
  g_treq = (struct timerequest *)
      CreateIORequest(g_tport, sizeof(struct timerequest));
  if (!g_treq) {
    DeleteMsgPort(g_tport);
    g_tport = NULL;
    return FALSE;
  }
  if (OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_MICROHZ,
                 (struct IORequest *)g_treq, 0)) {
    DeleteIORequest((struct IORequest *)g_treq);
    DeleteMsgPort(g_tport);
    g_treq = NULL;
    g_tport = NULL;
    return FALSE;
  }
  TimerBase = g_treq->tr_node.io_Device;
  g_efreq = ReadEClock(&ev);
  return g_efreq != 0;
}

static void timer_close(void)
{
  if (!g_treq) return;
  CloseDevice((struct IORequest *)g_treq);
  DeleteIORequest((struct IORequest *)g_treq);
  DeleteMsgPort(g_tport);
  g_treq = NULL;
  g_tport = NULL;
  TimerBase = NULL;
}

/* Sleep in 20 ms steps and watch for Ctrl-C. Returns FALSE on a break. */
static BOOL wait_ms(ULONG ms)
{
  ULONG steps = (ms + 19UL) / 20UL;

  while (steps--) {
    if (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) {
      g_break = TRUE;
      return FALSE;
    }
    Delay(1);
  }
  return !g_break;
}

struct measure {
  ULONG count;
  ULONG ticks;
};

/* Count how often *counter advances over ms milliseconds, with the elapsed
 * time read from the E clock. */
static BOOL measure_calls(volatile ULONG *counter, ULONG ms, struct measure *m)
{
  struct EClockVal e0, e1;
  ULONG c0 = *counter;
  BOOL ok;

  ReadEClock(&e0);
  ok = wait_ms(ms);
  ReadEClock(&e1);
  m->count = *counter - c0;
  m->ticks = e1.ev_lo - e0.ev_lo;
  return ok;
}

/* Events per second times 100. */
static ULONG rate100(ULONG count, ULONG ticks)
{
  if (!ticks) return 0;
  return (ULONG)((unsigned long long)count * g_efreq * 100ULL / ticks);
}

static BOOL within(ULONG got, ULONG want, ULONG min_tol)
{
  ULONG tol = want / 20UL;
  ULONG diff = got > want ? got - want : want - got;

  if (tol < min_tol) tol = min_tol;
  return diff <= tol;
}

/* ------------------------------------------------------------------ */
/* Sample data, generated in code                                      */
/* ------------------------------------------------------------------ */

static WORD sine16(ULONG ph)
{
  ULONG q, x, idx, fr;
  LONG a, b, v;

  ph &= 0xffffUL;
  q = ph >> 14;
  x = ph & 0x3fffUL;
  if (q & 1) x = 0x4000UL - x;
  idx = x >> 8;
  fr = x & 0xffUL;
  a = sine_q[idx];
  b = sine_q[idx + 1];
  v = a + (((b - a) * (LONG)fr) >> 8);
  return (WORD)((q & 2) ? -v : v);
}

/* A mono tone or glide. step is the phase increment per frame in 1/65536
 * cycle; dstep is how much the step changes over the whole run. */
static void gen16(WORD *dst, ULONG frames, ULONG step, LONG dstep)
{
  ULONG ph = 0, i;

  for (i = 0; i < frames; i++) {
    dst[i] = (WORD)(((LONG)sine16(ph) * (LONG)AMP) >> 15);
    ph += step + (ULONG)(((LONG)dstep * (LONG)i) / (LONG)frames);
  }
}

static void gen8(BYTE *dst, ULONG frames, ULONG step, LONG dstep)
{
  ULONG ph = 0, i;

  for (i = 0; i < frames; i++) {
    LONG v = ((LONG)sine16(ph) * (LONG)AMP) >> 15;

    dst[i] = (BYTE)(v >> 8);
    ph += step + (ULONG)(((LONG)dstep * (LONG)i) / (LONG)frames);
  }
}

/* Interleaved stereo: a different cycle count in each channel. */
static void gen16s(WORD *dst, ULONG frames, ULONG step_l, ULONG step_r)
{
  ULONG i;

  for (i = 0; i < frames; i++) {
    dst[i * 2] = (WORD)(((LONG)sine16(i * step_l) * (LONG)AMP) >> 15);
    dst[i * 2 + 1] = (WORD)(((LONG)sine16(i * step_r) * (LONG)AMP) >> 15);
  }
}

static void gen8s(BYTE *dst, ULONG frames, ULONG step_l, ULONG step_r)
{
  ULONG i;

  for (i = 0; i < frames; i++) {
    dst[i * 2] = (BYTE)((((LONG)sine16(i * step_l) * (LONG)AMP) >> 15) >> 8);
    dst[i * 2 + 1] =
        (BYTE)((((LONG)sine16(i * step_r) * (LONG)AMP) >> 15) >> 8);
  }
}

static void *amalloc(ULONG bytes)
{
  return AllocVec(bytes, MEMF_PUBLIC | MEMF_CLEAR);
}

/* 7040 Hz * 2^(semis / 12), the sample rate that plays the 64-frame cycle at
 * a semitone above 110 Hz. semis stays below 36. */
static ULONG semi_rate(ULONG semis)
{
  ULONG r = semitone[semis % 12UL] << (semis / 12UL);

  return (BASE_RATE_HZ * r) >> 16;
}

/* ------------------------------------------------------------------ */
/* Hook context                                                        */
/* ------------------------------------------------------------------ */

struct seg_plan {
  ULONG a_off;
  ULONG b_off;
  LONG len;
};

struct vctx {
  volatile ULONG player_calls;
  volatile ULONG sound_calls;
  volatile ULONG info_calls;
  volatile ULONG ch_calls[MAX_CH];
  volatile ULONG snap[MAX_CH];
  volatile ULONG snd_log_n;
  volatile ULONG snd_log[SND_LOG_MAX];
  const struct seg_plan *volatile plan;
  volatile ULONG next_b;
  ULONG hz;              /* playback rate used by the S5 script */
  ULONG hz_b;
};

static struct vctx g_ctx;

#define HOOK_FN(name) \
  static ULONG name(struct Hook *hook asm("a0"), \
                    struct AHIAudioCtrl *actrl asm("a2"), \
                    APTR msg asm("a1"))

#define HOOK_ENTRY(fn) ((void (*)(void))(fn))

static void hook_init(struct Hook *h, void (*entry)(void), APTR data)
{
  memset(h, 0, sizeof(*h));
  h->h_Entry = (ULONG (*)())entry;
  h->h_Data = data;
}

/* PlayerFunc: count calls. */
HOOK_FN(player_count_hook)
{
  struct vctx *c = (struct vctx *)hook->h_Data;

  (void)actrl;
  (void)msg;
  c->player_calls = c->player_calls + 1;
  return 0;
}

/* SoundFunc: count calls per channel and log the PlayerFunc tick. */
HOOK_FN(sound_count_hook)
{
  struct vctx *c = (struct vctx *)hook->h_Data;
  UWORD ch = ((struct AHISoundMessage *)msg)->ahism_Channel;
  ULONG n;

  (void)actrl;
  c->sound_calls = c->sound_calls + 1;
  if (ch < MAX_CH) c->ch_calls[ch] = c->ch_calls[ch] + 1;
  n = c->snd_log_n;
  if (n < SND_LOG_MAX) {
    c->snd_log[n] = c->player_calls;
    c->snd_log_n = n + 1;
  }
  return 0;
}

/* SoundFunc of S3: alternate two regions of the one sound. The queued
 * change is made without AHISF_IMM, as the guide requires inside a
 * SoundFunc. */
HOOK_FN(sound_segment_hook)
{
  struct vctx *c = (struct vctx *)hook->h_Data;
  const struct seg_plan *p = c->plan;
  UWORD ch = ((struct AHISoundMessage *)msg)->ahism_Channel;
  ULONG bit = 1UL << (ch & 31U);

  c->sound_calls = c->sound_calls + 1;
  if (!p) return 0;
  if (c->next_b & bit) {
    c->next_b &= ~bit;
    AHI_SetSound(ch, 0, p->b_off, p->len, actrl, AHISF_NONE);
  } else {
    c->next_b |= bit;
    AHI_SetSound(ch, 0, p->a_off, p->len, actrl, AHISF_NONE);
  }
  return 0;
}

/* AHIET_CHANNELINFO: AHI fills ahieci_Offset before calling the hook. */
HOOK_FN(channel_info_hook)
{
  struct vctx *c = (struct vctx *)hook->h_Data;
  struct AHIEffChannelInfo *ci = (struct AHIEffChannelInfo *)msg;
  ULONG n = ci->ahieci_Channels;
  ULONG i;

  (void)actrl;
  if (n > MAX_CH) n = MAX_CH;
  for (i = 0; i < n; i++) c->snap[i] = ci->ahieci_Offset[i];
  c->info_calls = c->info_calls + 1;
  return 0;
}

/* ------------------------------------------------------------------ */
/* AHI session                                                         */
/* ------------------------------------------------------------------ */

struct session {
  struct AHIAudioCtrl *ctrl;
  UWORD channels;
  ULONG loaded;                 /* bit per sound id */
  BOOL playing;
  BOOL master_on;
  struct AHIEffMasterVolume mv;
  struct AHIEffChannelInfo *ci;
  ULONG mixfreq;
};

static struct Hook hook_info;

static void stats_reset(void)
{
  g_stats_bad = 0;
}

static ULONG parse_field(const char *line, const char *key, BOOL *found)
{
  size_t klen = strlen(key);
  const char *p = line;

  while (*p) {
    if ((p == line || p[-1] == ' ') && !strncmp(p, key, klen) &&
        p[klen] == '=') {
      const char *digits = p + klen + 1;
      char *end;
      ULONG value = strtoul(digits, &end, 10);

      *found = *digits >= '0' && *digits <= '9' &&
               (*end == '\0' || *end == ' ');
      return value;
    }
    p++;
  }
  *found = FALSE;
  return 0;
}

/* Read ENV:ZZ9K_AHI_VOICE_STATS after AHI_FreeAudio. The driver writes it
 * when a hardware session closes. */
static void stats_read(void)
{
  char buf[200];
  LONG n;
  size_t len;
  BOOL f[4];
  ULONG late, rejected, underruns, dropped;

  n = GetVar((CONST_STRPTR)STATS_VAR, (STRPTR)buf, sizeof(buf),
             GVF_GLOBAL_ONLY);
  if (n < 0) {
    if (opt.voice) {
      out("  stats FAIL: ENV:%s is missing after AHI_FreeAudio\n", STATS_VAR);
      g_stats_bad++;
    }
    else out("  no voice stats (software mode)\n");
    return;
  }
  buf[sizeof(buf) - 1] = '\0';
  len = strlen(buf);
  while (len && (buf[len - 1] == '\n' || buf[len - 1] == '\r' ||
                 buf[len - 1] == ' '))
    buf[--len] = '\0';
  out("  stats: %s\n", buf);

  late = parse_field(buf, "late", &f[0]);
  rejected = parse_field(buf, "rejected", &f[1]);
  underruns = parse_field(buf, "underruns", &f[2]);
  dropped = parse_field(buf, "dropped", &f[3]);
  if (!f[0] || !f[1] || !f[2] || !f[3]) {
    out("  stats FAIL: cannot read late, rejected, underruns and dropped "
        "from \"%s\"\n", buf);
    g_stats_bad++;
  } else if (late || rejected || underruns || dropped) {
    out("  stats FAIL: late=%lu rejected=%lu underruns=%lu dropped=%lu\n",
        (unsigned long)late, (unsigned long)rejected,
        (unsigned long)underruns, (unsigned long)dropped);
    g_stats_bad++;
  }
}

static BOOL session_open(struct session *s, UWORD channels, UWORD sounds,
                         struct Hook *player, Fixed pfreq, Fixed pmin,
                         Fixed pmax, struct Hook *sound)
{
  struct TagItem tags[14];
  UWORD n = 0;

  memset(s, 0, sizeof(*s));
  DeleteVar((CONST_STRPTR)STATS_VAR, GVF_GLOBAL_ONLY);

#define ADD_TAG(t, v) do { tags[n].ti_Tag = (t); tags[n].ti_Data = (ULONG)(v); n++; } while (0)
  ADD_TAG(AHIA_AudioID, opt.mode);
  ADD_TAG(AHIA_MixFreq, MIX_FREQ);
  ADD_TAG(AHIA_Channels, channels);
  ADD_TAG(AHIA_Sounds, sounds);
  if (player) {
    ADD_TAG(AHIA_PlayerFunc, player);
    ADD_TAG(AHIA_PlayerFreq, pfreq);
    ADD_TAG(AHIA_MinPlayerFreq, pmin);
    ADD_TAG(AHIA_MaxPlayerFreq, pmax);
  }
  if (sound) ADD_TAG(AHIA_SoundFunc, sound);
  tags[n].ti_Tag = TAG_DONE;
  tags[n].ti_Data = 0;
#undef ADD_TAG

  s->ctrl = AHI_AllocAudioA(tags);
  if (!s->ctrl) return FALSE;
  s->channels = channels;
  return TRUE;
}

static void alloc_hint(UWORD channels, UWORD sounds)
{
  out("ERROR: AHI_AllocAudioA failed for mode $%08lX with %u channels and "
      "%u sounds\n", (unsigned long)opt.mode, (unsigned int)channels,
      (unsigned int)sounds);
  out("  The driver prints the reason on the debug channel; the mode may be "
      "refused:\n  no voice engine, session busy, Zorro II\n");
}

static ULONG play(struct session *s, BOOL on)
{
  struct TagItem t[3];
  UWORD n = 0;
  ULONG err;

  t[n].ti_Tag = AHIC_Play;
  t[n].ti_Data = on;
  n++;
  if (on && !s->mixfreq) {
    t[n].ti_Tag = AHIC_MixFreq_Query;
    t[n].ti_Data = (ULONG)&s->mixfreq;
    n++;
  }
  t[n].ti_Tag = TAG_DONE;
  t[n].ti_Data = 0;
  err = AHI_ControlAudioA(s->ctrl, t);
  if (err == AHIE_OK) s->playing = on;
  return err;
}

static ULONG set_player_freq(struct session *s, Fixed hz)
{
  struct TagItem t[2];

  t[0].ti_Tag = AHIA_PlayerFreq;
  t[0].ti_Data = (ULONG)hz;
  t[1].ti_Tag = TAG_DONE;
  t[1].ti_Data = 0;
  return AHI_ControlAudioA(s->ctrl, t);
}

static ULONG load_sound(struct session *s, UWORD id, ULONG type, APTR mem,
                        ULONG frames)
{
  struct AHISampleInfo si;
  ULONG err;

  si.ahisi_Type = type;
  si.ahisi_Address = mem;
  si.ahisi_Length = frames;
  err = AHI_LoadSound(id, AHIST_SAMPLE, &si, s->ctrl);
  if (err == AHIE_OK) s->loaded |= 1UL << id;
  return err;
}

static void unload_sound(struct session *s, UWORD id)
{
  if (!(s->loaded & (1UL << id))) return;
  AHI_UnloadSound(id, s->ctrl);
  s->loaded &= ~(1UL << id);
}

/* Outside the hooks every change takes AHISF_IMM. The sound starts once
 * SetSound and SetFreq have both been called. */
static void voice_start(struct session *s, UWORD ch, UWORD snd, ULONG off,
                        LONG len, ULONG freq, Fixed vol, Fixed pan)
{
  AHI_SetVol(ch, vol, pan, s->ctrl, AHISF_IMM);
  AHI_SetSound(ch, snd, off, len, s->ctrl, AHISF_IMM);
  AHI_SetFreq(ch, freq, s->ctrl, AHISF_IMM);
}

static void voice_stop(struct session *s, UWORD ch)
{
  AHI_SetSound(ch, AHI_NOSOUND, 0, 0, s->ctrl, AHISF_IMM);
}

static void stop_all(struct session *s)
{
  UWORD ch;

  for (ch = 0; ch < s->channels; ch++) voice_stop(s, ch);
}

static ULONG master_set(struct session *s, Fixed vol)
{
  ULONG err;

  s->mv.ahie_Effect = AHIET_MASTERVOLUME;
  s->mv.ahiemv_Volume = vol;
  err = AHI_SetEffect(&s->mv, s->ctrl);
  if (err == AHIE_OK) s->master_on = TRUE;
  return err;
}

static ULONG info_on(struct session *s)
{
  ULONG err;

  hook_init(&hook_info, HOOK_ENTRY(channel_info_hook), &g_ctx);
  s->ci = (struct AHIEffChannelInfo *)
      amalloc(sizeof(struct AHIEffChannelInfo) + 4UL * s->channels);
  if (!s->ci) return AHIE_NOMEM;
  s->ci->ahie_Effect = AHIET_CHANNELINFO;
  s->ci->ahieci_Func = &hook_info;
  s->ci->ahieci_Channels = s->channels;
  err = AHI_SetEffect(s->ci, s->ctrl);
  if (err != AHIE_OK) {
    FreeVec(s->ci);
    s->ci = NULL;
  }
  return err;
}

/* Stop playback, turn the effects off, unload and free, then read the
 * driver statistics. */
static void session_close(struct session *s)
{
  UWORD i;

  if (!s->ctrl) return;
  if (s->playing) play(s, FALSE);
  if (s->master_on) {
    s->mv.ahie_Effect = AHIET_MASTERVOLUME | AHIET_CANCEL;
    AHI_SetEffect(&s->mv, s->ctrl);
    s->master_on = FALSE;
  }
  if (s->ci) {
    s->ci->ahie_Effect = AHIET_CHANNELINFO | AHIET_CANCEL;
    AHI_SetEffect(s->ci, s->ctrl);
    FreeVec(s->ci);
    s->ci = NULL;
  }
  for (i = 0; i < 32; i++) unload_sound(s, i);
  AHI_FreeAudio(s->ctrl);
  s->ctrl = NULL;
  stats_read();
}

static UWORD pick_channels(UWORD def, UWORD min)
{
  ULONG n = opt.channels ? opt.channels : def;

  if (!opt.channels && !opt.voice && n > SOFT_MAX_CH) n = SOFT_MAX_CH;
  if (n < min) n = min;
  if (n > MAX_CH) n = MAX_CH;
  return (UWORD)n;
}

static ULONG mix_rate(const struct session *s)
{
  return s->mixfreq ? s->mixfreq : MIX_FREQ;
}

/* Two snapshots of the channel info offsets, spaced apart, to tell a
 * moving channel from a frozen one. Returns TRUE if the offset of ch
 * differs between any two of four samples. */
static BOOL offset_moves(UWORD ch)
{
  ULONG first = g_ctx.snap[ch];
  UWORD i;

  for (i = 0; i < 4; i++) {
    if (!wait_ms(70)) return FALSE;
    if (g_ctx.snap[ch] != first) return TRUE;
  }
  return FALSE;
}

/* ------------------------------------------------------------------ */
/* S1 tone per channel                                                 */
/* ------------------------------------------------------------------ */

static int scen_tones(void)
{
  struct session s;
  UWORD n = pick_channels(8, 1), ch;
  WORD *cycle = (WORD *)amalloc(CYCLE_FRAMES * 2UL);
  ULONG err, info_err;
  int rc = R_PASS;

  if (!cycle) return why(R_FAIL, "out of memory");
  gen16(cycle, CYCLE_FRAMES, CYCLE_STEP, 0);
  memset(&g_ctx, 0, sizeof(g_ctx));
  if (!session_open(&s, n, 1, NULL, 0, 0, 0, NULL)) {
    alloc_hint(n, 1);
    FreeVec(cycle);
    return R_NOALLOC;
  }
  info_err = info_on(&s);
  err = load_sound(&s, 0, AHIST_M16S, cycle, CYCLE_FRAMES);
  if (err != AHIE_OK) {
    rc = why(R_FAIL, "AHI_LoadSound M16S returned %s", err_name(err));
    goto done;
  }
  err = play(&s, TRUE);
  if (err != AHIE_OK) {
    rc = why(R_FAIL, "AHI_ControlAudio play returned %lu", (unsigned long)err);
    goto done;
  }
  for (ch = 0; ch < n; ch++) {
    Fixed pan = n > 1 ? (Fixed)(((ULONG)ch * 0x10000UL) / (n - 1U))
                      : PAN_CENTER;

    voice_start(&s, ch, 0, 0, 0, semi_rate(ch), VOL_FULL, pan);
  }
  out("  %u channels, sample rates %lu to %lu Hz (tones %lu to %lu Hz), pan "
      "left to right, mix %lu Hz\n", (unsigned int)n,
      (unsigned long)semi_rate(0), (unsigned long)semi_rate(n - 1U),
      (unsigned long)(semi_rate(0) / CYCLE_FRAMES),
      (unsigned long)(semi_rate(n - 1U) / CYCLE_FRAMES),
      (unsigned long)mix_rate(&s));
  out("  Listen: a rising scale from the left speaker to the right, every "
      "pitch distinct, no clicks.\n");
  if (!wait_ms(opt.seconds * 1000UL)) {
    rc = why(R_FAIL, "aborted");
    goto done;
  }
  if (info_err == AHIE_OK) {
    if (!g_ctx.info_calls) {
      rc = why(R_FAIL, "AHIET_CHANNELINFO hook was never called");
      goto done;
    }
    out("  channel info: %lu calls, channel 0 at frame %lu, channel %u at "
        "frame %lu\n", (unsigned long)g_ctx.info_calls,
        (unsigned long)g_ctx.snap[0], (unsigned int)(n - 1U),
        (unsigned long)g_ctx.snap[n - 1U]);
  } else if (opt.voice) {
    rc = why(R_FAIL, "AHIET_CHANNELINFO returned %s", err_name(info_err));
    goto done;
  } else {
    out("  channel info not available (%s)\n", err_name(info_err));
  }
  why(R_PASS, "%u tones played for %lu s", (unsigned int)n,
      (unsigned long)opt.seconds);
done:
  if (s.playing) stop_all(&s);
  session_close(&s);
  FreeVec(cycle);
  return rc;
}

/* ------------------------------------------------------------------ */
/* S2 chord at the master volume                                       */
/* ------------------------------------------------------------------ */

static int scen_chord(void)
{
  static const UWORD chord[3] = { 0, 4, 7 };
  struct session s;
  UWORD n = pick_channels(32, 1), ch;
  WORD *cycle = (WORD *)amalloc(CYCLE_FRAMES * 2UL);
  ULONG err, phase_ms = opt.seconds * 1000UL / 3UL;
  int rc = R_PASS;

  if (!cycle) return why(R_FAIL, "out of memory");
  if (phase_ms < 700UL) phase_ms = 700UL;
  gen16(cycle, CYCLE_FRAMES, CYCLE_STEP, 0);
  memset(&g_ctx, 0, sizeof(g_ctx));
  if (!session_open(&s, n, 1, NULL, 0, 0, 0, NULL)) {
    alloc_hint(n, 1);
    FreeVec(cycle);
    return R_NOALLOC;
  }
  err = load_sound(&s, 0, AHIST_M16S, cycle, CYCLE_FRAMES);
  if (err != AHIE_OK) {
    rc = why(R_FAIL, "AHI_LoadSound M16S returned %s", err_name(err));
    goto done;
  }
  err = play(&s, TRUE);
  if (err != AHIE_OK) {
    rc = why(R_FAIL, "AHI_ControlAudio play returned %lu", (unsigned long)err);
    goto done;
  }
  /* Every voice at full volume. ahi.device divides each channel by the
   * channel count, so the default master volume 1.0 cannot clip. */
  for (ch = 0; ch < n; ch++) {
    ULONG semis = chord[ch % 3U] + 12UL * ((ch / 3U) % 3U);

    voice_start(&s, ch, 0, 0, 0, semi_rate(semis), VOL_FULL,
                (ch & 1U) ? 0xC000L : 0x4000L);
  }
  out("  %u voices in a major chord at equal level\n", (unsigned int)n);
  out("  Listen: a steady chord with no distortion or crackle.\n");
  if (!wait_ms(phase_ms)) {
    rc = why(R_FAIL, "aborted");
    goto done;
  }
  err = master_set(&s, 0x10000L);
  if (err != AHIE_OK && opt.voice) {
    rc = why(R_FAIL, "AHIET_MASTERVOLUME 1.0 returned %s", err_name(err));
    goto done;
  }
  out("  master volume 1.0: %s\n", err_name(err));
  if (!wait_ms(phase_ms)) {
    rc = why(R_FAIL, "aborted");
    goto done;
  }
  if (err == AHIE_OK) {
    err = master_set(&s, 0x8000L);
    if (err != AHIE_OK) {
      rc = why(R_FAIL, "AHIET_MASTERVOLUME 0.5 returned %s", err_name(err));
      goto done;
    }
    out("  master volume 0.5: the chord is quieter by 6 dB\n");
    if (!wait_ms(phase_ms)) {
      rc = why(R_FAIL, "aborted");
      goto done;
    }
  }
  why(R_PASS, "%u-voice chord played at master volume 1.0 and 0.5",
      (unsigned int)n);
done:
  if (s.playing) stop_all(&s);
  session_close(&s);
  FreeVec(cycle);
  return rc;
}

/* ------------------------------------------------------------------ */
/* S3 SoundFunc multi-segment loop                                     */
/* ------------------------------------------------------------------ */

static const ULONG seg_lengths[8] = { 48, 96, 240, 480, 2400, 4800, 12000,
                                       24000 };

static int scen_segments(void)
{
  struct session s;
  struct Hook sound_hook;
  struct seg_plan plans[8];
  UWORD n = pick_channels(1, 1), ch;
  ULONG total = 0, i, err, rate, window_ms;
  WORD *data;
  int rc = R_PASS, bad = 0;

  if (!timer_open()) return why(R_FAIL, "timer.device is not available");
  for (i = 0; i < 8; i++) {
    plans[i].a_off = total;
    plans[i].b_off = total + seg_lengths[i];
    plans[i].len = (LONG)seg_lengths[i];
    total += 2UL * seg_lengths[i];
  }
  data = (WORD *)amalloc(total * 2UL);
  if (!data) return why(R_FAIL, "out of memory");
  /* Region A is 1500 Hz, region B 2250 Hz at a 48 kHz mix. */
  for (i = 0; i < 8; i++) {
    gen16(data + plans[i].a_off, seg_lengths[i], 2048UL, 0);
    gen16(data + plans[i].b_off, seg_lengths[i], 3072UL, 0);
  }
  memset(&g_ctx, 0, sizeof(g_ctx));
  hook_init(&sound_hook, HOOK_ENTRY(sound_segment_hook), &g_ctx);
  if (!session_open(&s, n, 1, NULL, 0, 0, 0, &sound_hook)) {
    alloc_hint(n, 1);
    FreeVec(data);
    return R_NOALLOC;
  }
  err = load_sound(&s, 0, AHIST_M16S, data, total);
  if (err != AHIE_OK) {
    rc = why(R_FAIL, "AHI_LoadSound M16S returned %s", err_name(err));
    goto done;
  }
  err = play(&s, TRUE);
  if (err != AHIE_OK) {
    rc = why(R_FAIL, "AHI_ControlAudio play returned %lu", (unsigned long)err);
    goto done;
  }
  rate = mix_rate(&s);
  out("  %u channel(s), one sound of %lu frames, passes of 1 ms to 500 ms\n",
      (unsigned int)n, (unsigned long)total);
  for (i = 0; i < 8 && !g_break; i++) {
    struct measure m;
    unsigned long long want;
    ULONG pass_ms = seg_lengths[i] * 1000UL / rate;
    ULONG want100, got100;

    if (pass_ms == 0) pass_ms = 1;
    for (ch = 0; ch < n; ch++) voice_stop(&s, ch);
    wait_ms(40);
    g_ctx.plan = &plans[i];
    g_ctx.next_b = ~0UL;
    for (ch = 0; ch < n; ch++)
      voice_start(&s, ch, 0, plans[i].a_off, plans[i].len, rate, VOL_FULL,
                  PAN_CENTER);
    wait_ms(2UL * pass_ms + 50UL);
    window_ms = opt.seconds * 1000UL / 8UL;
    if (window_ms < 4UL * pass_ms + 250UL) window_ms = 4UL * pass_ms + 250UL;
    if (!measure_calls(&g_ctx.sound_calls, window_ms, &m)) break;
    want = (unsigned long long)m.ticks * rate * 100ULL * n /
           ((unsigned long long)g_efreq * seg_lengths[i]);
    want100 = (ULONG)want;
    got100 = m.count * 100UL;
    if (within(got100, want100, 200UL)) {
      out("  pass %5lu ms: %lu calls in %lu ms, expected %lu.%02lu  ok\n",
          (unsigned long)pass_ms, (unsigned long)m.count,
          (unsigned long)(m.ticks / (g_efreq / 1000UL)),
          (unsigned long)(want100 / 100UL), (unsigned long)(want100 % 100UL));
    } else {
      out("  pass %5lu ms: %lu calls in %lu ms, expected %lu.%02lu  BAD\n",
          (unsigned long)pass_ms, (unsigned long)m.count,
          (unsigned long)(m.ticks / (g_efreq / 1000UL)),
          (unsigned long)(want100 / 100UL), (unsigned long)(want100 % 100UL));
      bad++;
    }
  }
  for (ch = 0; ch < n; ch++) voice_stop(&s, ch);
  if (g_break) rc = why(R_FAIL, "aborted");
  else if (bad) rc = why(R_FAIL, "%d of 8 pass lengths missed the expected "
                         "SoundFunc rate by more than 5 percent", bad);
  else why(R_PASS, "SoundFunc rate matched at 8 pass lengths from 1 to 500 ms");
done:
  session_close(&s);
  FreeVec(data);
  return rc;
}

/* ------------------------------------------------------------------ */
/* S4 PlayerFreq sweep                                                 */
/* ------------------------------------------------------------------ */

static int scen_playerfreq(void)
{
  static const Fixed steps[7] = {
    25L << 16, (Fixed)((100UL << 16) / 3UL), 50L << 16, 60L << 16, 75L << 16,
    100L << 16, 125L << 16
  };
  struct session s;
  struct Hook player_hook;
  UWORD n = pick_channels(1, 1), ch;
  WORD *cycle = (WORD *)amalloc(CYCLE_FRAMES * 2UL);
  ULONG err, window_ms, i;
  int rc = R_PASS, bad = 0;

  if (!cycle) return why(R_FAIL, "out of memory");
  if (!timer_open()) {
    FreeVec(cycle);
    return why(R_FAIL, "timer.device is not available");
  }
  gen16(cycle, CYCLE_FRAMES, CYCLE_STEP, 0);
  memset(&g_ctx, 0, sizeof(g_ctx));
  hook_init(&player_hook, HOOK_ENTRY(player_count_hook), &g_ctx);
  if (!session_open(&s, n, 1, &player_hook, steps[0], 25L << 16,
                    125L << 16, NULL)) {
    alloc_hint(n, 1);
    FreeVec(cycle);
    return R_NOALLOC;
  }
  err = load_sound(&s, 0, AHIST_M16S, cycle, CYCLE_FRAMES);
  if (err != AHIE_OK) {
    rc = why(R_FAIL, "AHI_LoadSound M16S returned %s", err_name(err));
    goto done;
  }
  /* PlayerFunc runs only between AHIC_Play TRUE and FALSE. */
  if (!wait_ms(300)) {
    rc = why(R_FAIL, "aborted");
    goto done;
  }
  if (g_ctx.player_calls) {
    rc = why(R_FAIL, "PlayerFunc was called %lu times before AHIC_Play",
             (unsigned long)g_ctx.player_calls);
    goto done;
  }
  err = play(&s, TRUE);
  if (err != AHIE_OK) {
    rc = why(R_FAIL, "AHI_ControlAudio play returned %lu", (unsigned long)err);
    goto done;
  }
  for (ch = 0; ch < n; ch++)
    voice_start(&s, ch, 0, 0, 0, semi_rate(ch), VOL_FULL, PAN_CENTER);
  window_ms = opt.seconds * 1000UL / 7UL;
  if (window_ms < 2000UL) window_ms = 2000UL;
  for (i = 0; i < 7 && !g_break; i++) {
    struct measure m;
    ULONG want100 = ((ULONG)steps[i] * 100UL) >> 16;
    ULONG got100;

    err = set_player_freq(&s, steps[i]);
    if (err != AHIE_OK) {
      rc = why(R_FAIL, "AHIA_PlayerFreq change returned %lu",
               (unsigned long)err);
      goto done;
    }
    if (!wait_ms(300)) break;
    if (!measure_calls(&g_ctx.player_calls, window_ms, &m)) break;
    got100 = rate100(m.count, m.ticks);
    if (within(got100, want100, 0)) {
      out("  PlayerFreq %3lu.%02lu Hz: measured %lu.%02lu Hz  ok\n",
          (unsigned long)(want100 / 100UL), (unsigned long)(want100 % 100UL),
          (unsigned long)(got100 / 100UL), (unsigned long)(got100 % 100UL));
    } else {
      out("  PlayerFreq %3lu.%02lu Hz: measured %lu.%02lu Hz  BAD\n",
          (unsigned long)(want100 / 100UL), (unsigned long)(want100 % 100UL),
          (unsigned long)(got100 / 100UL), (unsigned long)(got100 % 100UL));
      bad++;
    }
  }
  if (g_break) {
    rc = why(R_FAIL, "aborted");
    goto done;
  }
  stop_all(&s);
  err = play(&s, FALSE);
  if (err != AHIE_OK) {
    rc = why(R_FAIL, "AHI_ControlAudio stop returned %lu", (unsigned long)err);
    goto done;
  }
  {
    ULONG before;

    wait_ms(300);
    before = g_ctx.player_calls;
    wait_ms(300);
    if (g_ctx.player_calls != before) {
      rc = why(R_FAIL, "PlayerFunc still called after AHIC_Play FALSE");
      goto done;
    }
  }
  if (bad)
    rc = why(R_FAIL, "%d of 7 PlayerFunc rates were off by more than 5 "
             "percent", bad);
  else
    why(R_PASS, "PlayerFunc rate followed AHIA_PlayerFreq from 25 to 125 Hz");
done:
  if (s.playing) stop_all(&s);
  session_close(&s);
  FreeVec(cycle);
  return rc;
}

/* ------------------------------------------------------------------ */
/* S5 IMM and queued changes                                           */
/* ------------------------------------------------------------------ */

#define S5_A_OFF   0UL
#define S5_A_LEN   19200L
#define S5_B_OFF   19200UL
#define S5_B_LEN   19200L
#define S5_C_OFF   38400UL
#define S5_C_LEN   4800L
#define S5_TOTAL   43200UL

#define S5_T_START 1UL
#define S5_T_QUEUE 5UL
#define S5_T_IMM   30UL
#define S5_T_QSTOP 42UL
#define S5_T_END   75UL

/* The PlayerFunc script runs at 50 Hz. It starts sound A with IMM, queues
 * sound B without IMM (B must wait for the end of A's pass), switches to a
 * short sound C with IMM (C must start at once) and finally queues a stop. */
HOOK_FN(player_script_hook)
{
  struct vctx *c = (struct vctx *)hook->h_Data;
  ULONG t = c->player_calls + 1;

  c->player_calls = t;
  switch (t) {
  case S5_T_START:
    AHI_SetVol(0, VOL_FULL, PAN_CENTER, actrl, AHISF_IMM);
    AHI_SetSound(0, 0, S5_A_OFF, S5_A_LEN, actrl, AHISF_IMM);
    AHI_SetFreq(0, c->hz, actrl, AHISF_IMM);
    break;
  case S5_T_QUEUE:
    AHI_SetVol(0, VOL_HALF, PAN_LEFT, actrl, AHISF_NONE);
    AHI_SetSound(0, 0, S5_B_OFF, S5_B_LEN, actrl, AHISF_NONE);
    AHI_SetFreq(0, c->hz_b, actrl, AHISF_NONE);
    break;
  case S5_T_IMM:
    AHI_SetVol(0, VOL_FULL, PAN_RIGHT, actrl, AHISF_IMM);
    AHI_SetSound(0, 0, S5_C_OFF, S5_C_LEN, actrl, AHISF_IMM);
    AHI_SetFreq(0, c->hz, actrl, AHISF_IMM);
    break;
  case S5_T_QSTOP:
    AHI_SetSound(0, AHI_NOSOUND, 0, 0, actrl, AHISF_NONE);
    break;
  default:
    break;
  }
  (void)msg;
  return 0;
}

static ULONG pass_ticks(ULONG frames, ULONG hz)
{
  return (frames * PLAYER_HZ + hz / 2UL) / hz;
}

static int scen_queued(void)
{
  struct session s;
  struct Hook player_hook, sound_hook;
  WORD *data = (WORD *)amalloc(S5_TOTAL * 2UL);
  ULONG err, ta, tc, i, first_after_queue = 0, imm_hit = 0, late_calls = 0;
  ULONG waited;
  int rc = R_PASS;

  if (!data) return why(R_FAIL, "out of memory");
  /* A 440 Hz, B 330 Hz, C 660 Hz at the 48 kHz mix. */
  gen16(data + S5_A_OFF, (ULONG)S5_A_LEN, 600UL, 0);
  gen16(data + S5_B_OFF, (ULONG)S5_B_LEN, 450UL, 0);
  gen16(data + S5_C_OFF, (ULONG)S5_C_LEN, 900UL, 0);
  memset(&g_ctx, 0, sizeof(g_ctx));
  g_ctx.hz = MIX_FREQ;
  g_ctx.hz_b = MIX_FREQ * 2UL / 3UL;
  hook_init(&player_hook, HOOK_ENTRY(player_script_hook), &g_ctx);
  hook_init(&sound_hook, HOOK_ENTRY(sound_count_hook), &g_ctx);
  if (!session_open(&s, 1, 1, &player_hook, (Fixed)(PLAYER_HZ << 16),
                    25L << 16, 125L << 16, &sound_hook)) {
    alloc_hint(1, 1);
    FreeVec(data);
    return R_NOALLOC;
  }
  err = load_sound(&s, 0, AHIST_M16S, data, S5_TOTAL);
  if (err != AHIE_OK) {
    rc = why(R_FAIL, "AHI_LoadSound M16S returned %s", err_name(err));
    goto done;
  }
  /* The script starts at the first PlayerFunc tick. */
  err = play(&s, TRUE);
  if (err != AHIE_OK) {
    rc = why(R_FAIL, "AHI_ControlAudio play returned %lu", (unsigned long)err);
    goto done;
  }
  for (waited = 0; g_ctx.player_calls < S5_T_END && waited < 5000UL;
       waited += 20UL)
    if (!wait_ms(20)) break;
  wait_ms(400);
  if (g_break) {
    rc = why(R_FAIL, "aborted");
    goto done;
  }
  if (g_ctx.player_calls < S5_T_END) {
    rc = why(R_FAIL, "PlayerFunc reached tick %lu of %lu",
             (unsigned long)g_ctx.player_calls, (unsigned long)S5_T_END);
    goto done;
  }
  ta = pass_ticks((ULONG)S5_A_LEN, g_ctx.hz);
  tc = pass_ticks((ULONG)S5_C_LEN, g_ctx.hz);
  out("  SoundFunc calls at PlayerFunc ticks:");
  for (i = 0; i < g_ctx.snd_log_n && i < SND_LOG_MAX; i++) {
    ULONG t = g_ctx.snd_log[i];

    out(" %lu", (unsigned long)t);
    if (!first_after_queue && t > S5_T_QUEUE) first_after_queue = t;
    if (t >= S5_T_IMM + tc - 2UL && t <= S5_T_IMM + tc + 3UL) imm_hit = 1;
    if (t > S5_T_QSTOP + tc + 6UL) late_calls++;
  }
  out("\n");
  if (!first_after_queue || first_after_queue + 3UL < S5_T_START + ta ||
      first_after_queue > S5_T_START + ta + 3UL) {
    rc = why(R_FAIL, "the queued sound B (queued at tick %lu) began at "
             "tick %lu, expected the end of pass A near tick %lu",
             (unsigned long)S5_T_QUEUE, (unsigned long)first_after_queue,
             (unsigned long)(S5_T_START + ta));
    goto done;
  }
  if (!imm_hit) {
    rc = why(R_FAIL, "the IMM sound C (set at tick %lu) did not end its "
             "first pass near tick %lu", (unsigned long)S5_T_IMM,
             (unsigned long)(S5_T_IMM + tc));
    goto done;
  }
  if (late_calls) {
    rc = why(R_FAIL, "the queued stop (tick %lu) did not silence the "
             "channel: %lu later SoundFunc calls", (unsigned long)S5_T_QSTOP,
             (unsigned long)late_calls);
    goto done;
  }
  out("  Listen: A centred, then B lower, quieter, left; then C higher, "
      "right; then silence.\n");
  why(R_PASS, "queued changes waited for the pass end, IMM changes did not");
done:
  if (s.playing) stop_all(&s);
  session_close(&s);
  FreeVec(data);
  return rc;
}

/* ------------------------------------------------------------------ */
/* S6 backward sounds                                                  */
/* ------------------------------------------------------------------ */

#define S6_FRAMES 4800UL

static int scen_backward(void)
{
  struct session s;
  struct Hook sound_hook;
  BYTE *glide8 = (BYTE *)amalloc(S6_FRAMES);
  WORD *glide16 = (WORD *)amalloc(S6_FRAMES * 2UL);
  ULONG err, rate, window_ms, id, dir;
  int rc = R_PASS, bad = 0;

  if (!glide8 || !glide16) {
    if (glide8) FreeVec(glide8);
    if (glide16) FreeVec(glide16);
    return why(R_FAIL, "out of memory");
  }
  if (!timer_open()) {
    FreeVec(glide8);
    FreeVec(glide16);
    return why(R_FAIL, "timer.device is not available");
  }
  /* A rising sweep from 1 kHz to 4 kHz; backwards it falls. */
  gen8(glide8, S6_FRAMES, 1365UL, 4096L);
  gen16(glide16, S6_FRAMES, 1365UL, 4096L);
  memset(&g_ctx, 0, sizeof(g_ctx));
  hook_init(&sound_hook, HOOK_ENTRY(sound_count_hook), &g_ctx);
  if (!session_open(&s, 1, 2, NULL, 0, 0, 0, &sound_hook)) {
    alloc_hint(1, 2);
    FreeVec(glide8);
    FreeVec(glide16);
    return R_NOALLOC;
  }
  err = load_sound(&s, 0, AHIST_M8S, glide8, S6_FRAMES);
  if (err != AHIE_OK) {
    rc = why(R_FAIL, "AHI_LoadSound M8S returned %s", err_name(err));
    goto done;
  }
  err = load_sound(&s, 1, AHIST_M16S, glide16, S6_FRAMES);
  if (err != AHIE_OK) {
    rc = why(R_FAIL, "AHI_LoadSound M16S returned %s", err_name(err));
    goto done;
  }
  err = play(&s, TRUE);
  if (err != AHIE_OK) {
    rc = why(R_FAIL, "AHI_ControlAudio play returned %lu", (unsigned long)err);
    goto done;
  }
  rate = mix_rate(&s);
  window_ms = opt.seconds * 1000UL / 4UL;
  if (window_ms < 1000UL) window_ms = 1000UL;
  out("  Listen: each sweep rises when played forward and falls when played "
      "backward.\n");
  for (id = 0; id < 2 && !g_break; id++) {
    for (dir = 0; dir < 2 && !g_break; dir++) {
      struct measure m;
      ULONG want100, got100;

      voice_stop(&s, 0);
      wait_ms(100);
      /* Offset is the first sample played in both directions: a backward
       * pass starts at the last frame. */
      if (dir)
        voice_start(&s, 0, (UWORD)id, S6_FRAMES - 1UL, -(LONG)S6_FRAMES,
                    rate, VOL_FULL, PAN_CENTER);
      else
        voice_start(&s, 0, (UWORD)id, 0, (LONG)S6_FRAMES, rate, VOL_FULL,
                    PAN_CENTER);
      wait_ms(250);
      if (!measure_calls(&g_ctx.sound_calls, window_ms, &m)) break;
      want100 = (ULONG)((unsigned long long)m.ticks * rate * 100ULL /
                        ((unsigned long long)g_efreq * S6_FRAMES));
      got100 = m.count * 100UL;
      out("  %s %s: %lu passes, expected %lu.%02lu  %s\n",
          id ? "16-bit" : " 8-bit", dir ? "backward" : "forward ",
          (unsigned long)m.count, (unsigned long)(want100 / 100UL),
          (unsigned long)(want100 % 100UL),
          within(got100, want100, 200UL) ? "ok" : "BAD");
      if (!within(got100, want100, 200UL)) bad++;
    }
  }
  voice_stop(&s, 0);
  if (g_break) rc = why(R_FAIL, "aborted");
  else if (bad) rc = why(R_FAIL, "%d of 4 sweeps passed at the wrong rate", bad);
  else why(R_PASS, "8-bit and 16-bit sounds played forward and backward");
done:
  session_close(&s);
  FreeVec(glide8);
  FreeVec(glide16);
  return rc;
}

/* ------------------------------------------------------------------ */
/* S7 sample types                                                     */
/* ------------------------------------------------------------------ */

#define S7_FRAMES 128UL

static int scen_types(void)
{
  static const struct {
    const char *name;
    ULONG type;
    BOOL stereo;
  } kinds[4] = {
    { "M8S", AHIST_M8S, FALSE }, { "M16S", AHIST_M16S, FALSE },
    { "S8S", AHIST_S8S, TRUE }, { "S16S", AHIST_S16S, TRUE }
  };
  struct session s;
  struct Hook sound_hook;
  void *mem[4] = { NULL, NULL, NULL, NULL };
  ULONG err, id, play_ms, rate;
  int rc = R_PASS, skipped = 0, played = 0;

  mem[0] = amalloc(S7_FRAMES);
  mem[1] = amalloc(S7_FRAMES * 2UL);
  mem[2] = amalloc(S7_FRAMES * 2UL);
  mem[3] = amalloc(S7_FRAMES * 4UL);
  if (!mem[0] || !mem[1] || !mem[2] || !mem[3]) {
    rc = why(R_FAIL, "out of memory");
    goto free_mem;
  }
  /* Mono: 2 cycles in 128 frames. Stereo: 2 cycles left, 3 right. */
  gen8((BYTE *)mem[0], S7_FRAMES, 1024UL, 0);
  gen16((WORD *)mem[1], S7_FRAMES, 1024UL, 0);
  gen8s((BYTE *)mem[2], S7_FRAMES, 1024UL, 1536UL);
  gen16s((WORD *)mem[3], S7_FRAMES, 1024UL, 1536UL);
  memset(&g_ctx, 0, sizeof(g_ctx));
  hook_init(&sound_hook, HOOK_ENTRY(sound_count_hook), &g_ctx);
  if (!session_open(&s, 1, 4, NULL, 0, 0, 0, &sound_hook)) {
    alloc_hint(1, 4);
    rc = R_NOALLOC;
    goto free_mem;
  }
  for (id = 0; id < 4; id++) {
    err = load_sound(&s, (UWORD)id, kinds[id].type, mem[id], S7_FRAMES);
    if (err == AHIE_OK) {
      out("  %s load: AHIE_OK\n", kinds[id].name);
    } else if (err == AHIE_BADSAMPLETYPE && kinds[id].stereo && opt.voice) {
      out("  %s load: AHIE_BADSAMPLETYPE (the firmware does not advertise "
          "stereo voice samples; reported, not a failure)\n",
          kinds[id].name);
      skipped++;
    } else {
      rc = why(R_FAIL, "AHI_LoadSound %s returned %s", kinds[id].name,
               err_name(err));
      goto done;
    }
  }
  err = play(&s, TRUE);
  if (err != AHIE_OK) {
    rc = why(R_FAIL, "AHI_ControlAudio play returned %lu", (unsigned long)err);
    goto done;
  }
  rate = mix_rate(&s) / 2UL;
  play_ms = opt.seconds * 1000UL / 4UL;
  if (play_ms < 700UL) play_ms = 700UL;
  out("  Listen: mono tones in the centre; stereo tones low on the left and "
      "a fifth higher on the right.\n");
  for (id = 0; id < 4 && !g_break; id++) {
    ULONG before;

    if (!(s.loaded & (1UL << id))) continue;
    before = g_ctx.sound_calls;
    voice_start(&s, 0, (UWORD)id, 0, 0, rate, VOL_FULL, PAN_CENTER);
    if (!wait_ms(play_ms)) break;
    voice_stop(&s, 0);
    wait_ms(100);
    if (g_ctx.sound_calls == before) {
      rc = why(R_FAIL, "%s sound was loaded but never reached its end "
               "(no SoundFunc calls)", kinds[id].name);
      goto done;
    }
    played++;
  }
  if (g_break) rc = why(R_FAIL, "aborted");
  else why(R_PASS, "%d sample types played, %d stereo types refused by the "
           "firmware", played, skipped);
done:
  session_close(&s);
free_mem:
  for (id = 0; id < 4; id++)
    if (mem[id]) FreeVec(mem[id]);
  return rc;
}

/* ------------------------------------------------------------------ */
/* S8 stop and start                                                   */
/* ------------------------------------------------------------------ */

static int scen_stopstart(void)
{
  static const UWORD tones[4] = { 0, 4, 7, 12 };
  struct session s;
  struct Hook player_hook;
  UWORD n = pick_channels(4, 2), ch;
  WORD *cycle = (WORD *)amalloc(CYCLE_FRAMES * 2UL);
  ULONG err, info_err, phase_ms = opt.seconds * 1000UL / 5UL, before;
  int rc = R_PASS;

  if (!cycle) return why(R_FAIL, "out of memory");
  if (phase_ms < 800UL) phase_ms = 800UL;
  gen16(cycle, CYCLE_FRAMES, CYCLE_STEP, 0);
  memset(&g_ctx, 0, sizeof(g_ctx));
  hook_init(&player_hook, HOOK_ENTRY(player_count_hook), &g_ctx);
  if (!session_open(&s, n, 1, &player_hook, (Fixed)(PLAYER_HZ << 16),
                    25L << 16, 125L << 16, NULL)) {
    alloc_hint(n, 1);
    FreeVec(cycle);
    return R_NOALLOC;
  }
  info_err = info_on(&s);
  err = load_sound(&s, 0, AHIST_M16S, cycle, CYCLE_FRAMES);
  if (err != AHIE_OK) {
    rc = why(R_FAIL, "AHI_LoadSound M16S returned %s", err_name(err));
    goto done;
  }
  err = play(&s, TRUE);
  if (err != AHIE_OK) {
    rc = why(R_FAIL, "AHI_ControlAudio play returned %lu", (unsigned long)err);
    goto done;
  }
  for (ch = 0; ch < n; ch++)
    voice_start(&s, ch, 0, 0, 0, semi_rate(tones[ch & 3U] + 12U * ((ch >> 2) % 3U)),
                VOL_FULL, (ch & 1U) ? 0xC000L : 0x4000L);
  out("  1. playing: %u tones\n", (unsigned int)n);
  if (!wait_ms(phase_ms)) goto aborted;
  if (!g_ctx.player_calls) {
    rc = why(R_FAIL, "PlayerFunc was not called while playing");
    goto done;
  }

  out("  2. AHIC_Play FALSE: expect silence\n");
  err = play(&s, FALSE);
  if (err != AHIE_OK) {
    rc = why(R_FAIL, "AHI_ControlAudio stop returned %lu", (unsigned long)err);
    goto done;
  }
  wait_ms(200);
  before = g_ctx.player_calls;
  if (!wait_ms(phase_ms)) goto aborted;
  if (g_ctx.player_calls != before) {
    rc = why(R_FAIL, "PlayerFunc ran while playback was stopped");
    goto done;
  }
  if (info_err == AHIE_OK && offset_moves(0)) {
    rc = why(R_FAIL, "channel 0 kept moving while playback was stopped");
    goto done;
  }

  out("  3. AHIC_Play TRUE: expect the tones to continue\n");
  err = play(&s, TRUE);
  if (err != AHIE_OK) {
    rc = why(R_FAIL, "AHI_ControlAudio restart returned %lu",
             (unsigned long)err);
    goto done;
  }
  before = g_ctx.player_calls;
  if (!wait_ms(phase_ms)) goto aborted;
  if (g_ctx.player_calls == before) {
    rc = why(R_FAIL, "PlayerFunc did not resume after AHIC_Play TRUE");
    goto done;
  }
  if (info_err == AHIE_OK && !offset_moves(1)) {
    rc = why(R_FAIL, "channel 1 did not move after AHIC_Play TRUE");
    goto done;
  }

  out("  4. frequency 0 on channel 0: expect that tone to pause\n");
  AHI_SetFreq(0, 0, s.ctrl, AHISF_IMM);
  wait_ms(200);
  if (info_err == AHIE_OK) {
    if (offset_moves(0)) {
      rc = why(R_FAIL, "channel 0 moved with frequency 0");
      goto done;
    }
    if (!offset_moves(1)) {
      rc = why(R_FAIL, "channel 1 stopped when channel 0 was paused");
      goto done;
    }
  }
  if (!wait_ms(phase_ms)) goto aborted;

  out("  5. frequency restored: expect the tone to resume\n");
  AHI_SetFreq(0, semi_rate(tones[0]), s.ctrl, AHISF_IMM);
  wait_ms(200);
  if (info_err == AHIE_OK && !offset_moves(0)) {
    rc = why(R_FAIL, "channel 0 did not resume after frequency 0");
    goto done;
  }
  if (!wait_ms(phase_ms)) goto aborted;
  if (info_err != AHIE_OK)
    out("  channel info not available (%s): checked the PlayerFunc only\n",
        err_name(info_err));
  out("  Listen: silence in step 2, the same tones again in step 3, one "
      "tone missing in step 4 and back in step 5.\n");
  why(R_PASS, "stop, start and frequency 0 behaved as documented");
  goto done;
aborted:
  rc = why(R_FAIL, "aborted");
done:
  if (s.playing) stop_all(&s);
  session_close(&s);
  FreeVec(cycle);
  return rc;
}

/* ------------------------------------------------------------------ */
/* S9 unload while playing                                             */
/* ------------------------------------------------------------------ */

static int scen_unload(void)
{
  struct session s;
  struct Hook sound_hook;
  UWORD n = pick_channels(2, 2);
  WORD *cycle = (WORD *)amalloc(CYCLE_FRAMES * 2UL);
  ULONG err, rate0 = semi_rate(0), rate1 = semi_rate(7), before;
  int rc = R_PASS;

  if (!cycle) return why(R_FAIL, "out of memory");
  gen16(cycle, CYCLE_FRAMES, CYCLE_STEP, 0);
  memset(&g_ctx, 0, sizeof(g_ctx));
  hook_init(&sound_hook, HOOK_ENTRY(sound_count_hook), &g_ctx);
  if (!session_open(&s, n, 2, NULL, 0, 0, 0, &sound_hook)) {
    alloc_hint(n, 2);
    FreeVec(cycle);
    return R_NOALLOC;
  }
  if ((err = load_sound(&s, 0, AHIST_M16S, cycle, CYCLE_FRAMES)) != AHIE_OK ||
      (err = load_sound(&s, 1, AHIST_M16S, cycle, CYCLE_FRAMES)) != AHIE_OK) {
    rc = why(R_FAIL, "AHI_LoadSound M16S returned %s", err_name(err));
    goto done;
  }
  err = play(&s, TRUE);
  if (err != AHIE_OK) {
    rc = why(R_FAIL, "AHI_ControlAudio play returned %lu", (unsigned long)err);
    goto done;
  }
  voice_start(&s, 0, 0, 0, 0, rate0, VOL_FULL, PAN_LEFT);
  voice_start(&s, 1, 1, 0, 0, rate1, VOL_FULL, PAN_RIGHT);
  out("  Listen: a low tone on the left and a higher one on the right.\n");
  if (!wait_ms(800)) goto aborted;

  /* AHI does not say what a channel plays after its sound is unloaded.
   * The requirement is that nothing hangs and the other channel carries
   * on, so only channel 1 is judged here. */
  out("  unloading sound 0 while channel 0 plays it\n");
  unload_sound(&s, 0);
  before = g_ctx.ch_calls[1];
  if (!wait_ms(600)) goto aborted;
  if (g_ctx.ch_calls[1] == before) {
    rc = why(R_FAIL, "channel 1 stopped after sound 0 was unloaded");
    goto done;
  }
  voice_stop(&s, 0);

  out("  loading sound 0 again and restarting channel 0\n");
  err = load_sound(&s, 0, AHIST_M16S, cycle, CYCLE_FRAMES);
  if (err != AHIE_OK) {
    rc = why(R_FAIL, "reloading sound 0 returned %s", err_name(err));
    goto done;
  }
  before = g_ctx.ch_calls[0];
  voice_start(&s, 0, 0, 0, 0, rate0, VOL_FULL, PAN_LEFT);
  if (!wait_ms(600)) goto aborted;
  if (g_ctx.ch_calls[0] == before) {
    rc = why(R_FAIL, "channel 0 did not play the reloaded sound 0");
    goto done;
  }

  out("  unloading sound 1 while channel 1 plays it\n");
  unload_sound(&s, 1);
  before = g_ctx.ch_calls[0];
  if (!wait_ms(600)) goto aborted;
  if (g_ctx.ch_calls[0] == before) {
    rc = why(R_FAIL, "channel 0 stopped after sound 1 was unloaded");
    goto done;
  }
  voice_stop(&s, 1);
  why(R_PASS, "unloading a playing sound left the other channel playing");
  goto done;
aborted:
  rc = why(R_FAIL, "aborted");
done:
  if (s.playing) stop_all(&s);
  session_close(&s);
  FreeVec(cycle);
  return rc;
}

/* ------------------------------------------------------------------ */
/* S10 repeated allocation cycles                                      */
/* ------------------------------------------------------------------ */

static int scen_cycles(void)
{
  struct session s;
  WORD *cycle = (WORD *)amalloc(CYCLE_FRAMES * 2UL);
  UWORD n = pick_channels(2, 1);
  ULONG i, err, bad_before;
  int rc = R_PASS;

  if (!cycle) return why(R_FAIL, "out of memory");
  gen16(cycle, CYCLE_FRAMES, CYCLE_STEP, 0);
  memset(&g_ctx, 0, sizeof(g_ctx));
  for (i = 0; i < opt.cycles && !g_break; i++) {
    UWORD ch;

    bad_before = g_stats_bad;
    if (!session_open(&s, n, 1, NULL, 0, 0, 0, NULL)) {
      alloc_hint(n, 1);
      out("  allocation failed in cycle %lu of %lu\n",
          (unsigned long)(i + 1UL), (unsigned long)opt.cycles);
      FreeVec(cycle);
      return R_NOALLOC;
    }
    err = load_sound(&s, 0, AHIST_M16S, cycle, CYCLE_FRAMES);
    if (err == AHIE_OK) err = play(&s, TRUE);
    if (err != AHIE_OK) {
      rc = why(R_FAIL, "cycle %lu: load or play returned %lu",
               (unsigned long)(i + 1UL), (unsigned long)err);
      session_close(&s);
      break;
    }
    for (ch = 0; ch < n; ch++)
      voice_start(&s, ch, 0, 0, 0, semi_rate((ch % 9U) * 4U), VOL_FULL, PAN_CENTER);
    wait_ms(100);
    stop_all(&s);
    session_close(&s);
    if (g_stats_bad != bad_before)
      out("  cycle %lu: the stats above are nonzero\n",
          (unsigned long)(i + 1UL));
    if (((i + 1UL) % 10UL) == 0)
      out("  %lu of %lu cycles done\n", (unsigned long)(i + 1UL),
          (unsigned long)opt.cycles);
  }
  if (g_break) rc = why(R_FAIL, "aborted");
  else if (rc == R_PASS)
    why(R_PASS, "%lu AllocAudio and FreeAudio cycles", (unsigned long)opt.cycles);
  FreeVec(cycle);
  return rc;
}

/* ------------------------------------------------------------------ */
/* Driver                                                              */
/* ------------------------------------------------------------------ */

struct scenario {
  const char *name;
  int (*run)(void);
};

static const struct scenario scenarios[SCENARIOS] = {
  { "tone per channel", scen_tones },
  { "chord at the master volume", scen_chord },
  { "SoundFunc multi-segment loop", scen_segments },
  { "PlayerFunc rate sweep", scen_playerfreq },
  { "IMM and queued changes", scen_queued },
  { "backward sounds", scen_backward },
  { "sample types", scen_types },
  { "stop and start", scen_stopstart },
  { "unload while playing", scen_unload },
  { "allocation cycles", scen_cycles },
};

static void usage(void)
{
  out("Usage: ZZAXVoiceTest [MODE <audio id>] [SCENARIO S1..S10|ALL]\n"
      "                    [CHANNELS 1-32] [SECONDS 1-%lu] [CYCLES 1-%lu]\n"
      "Template: %s\n"
      "MODE is a hexadecimal audio ID, default $CAFE0002 (hardware mixing);\n"
      "$CAFE0003 is the interpolated hardware mode and $CAFE0004 the cubic\n"
      "hardware mode.\n",
      (unsigned long)MAX_SECONDS, (unsigned long)MAX_CYCLES, TEMPLATE);
}

static BOOL eq_nocase(const char *a, const char *b)
{
  while (*a && *b) {
    char x = *a++, y = *b++;

    if (x >= 'a' && x <= 'z') x = (char)(x - 'a' + 'A');
    if (y >= 'a' && y <= 'z') y = (char)(y - 'a' + 'A');
    if (x != y) return FALSE;
  }
  return !*a && !*b;
}

static BOOL parse_mode(const char *text, ULONG *mode)
{
  char *end;
  ULONG v;

  if (*text == '$') text++;
  else if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) text += 2;
  if (!*text) return FALSE;
  v = strtoul(text, &end, 16);
  if (*end) return FALSE;
  *mode = v;
  return TRUE;
}

static BOOL parse_scenario(const char *text, int *first, int *last)
{
  char *end;
  ULONG v;

  if (eq_nocase(text, "ALL")) {
    *first = 1;
    *last = SCENARIOS;
    return TRUE;
  }
  if (*text == 'S' || *text == 's') text++;
  v = strtoul(text, &end, 10);
  if (!*text || *end || v < 1 || v > SCENARIOS) return FALSE;
  *first = *last = (int)v;
  return TRUE;
}

int main(void)
{
  LONG args[5] = { 0, 0, 0, 0, 0 };
  struct RDArgs *rda;
  int first = 1, last = SCENARIOS, i;
  int passed = 0, failed = 0, result = RETURN_OK;
  struct MsgPort *port = NULL;
  struct AHIRequest *request = NULL;
  BYTE device_open = -1;

  opt.mode = DEFAULT_MODE;
  opt.seconds = DEFAULT_SECONDS;
  opt.cycles = DEFAULT_CYCLES;

  rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
  if (!rda) {
    PrintFault(IoErr(), (CONST_STRPTR)"ZZAXVoiceTest");
    usage();
    return EXIT_USAGE;
  }
  if (args[0] && !parse_mode((const char *)args[0], &opt.mode)) {
    out("ERROR: MODE must be a hexadecimal audio ID such as $CAFE0002\n");
    FreeArgs(rda);
    return EXIT_USAGE;
  }
  if (args[1] && !parse_scenario((const char *)args[1], &first, &last)) {
    out("ERROR: SCENARIO must be S1 to S10 or ALL\n");
    FreeArgs(rda);
    return EXIT_USAGE;
  }
  if (args[2]) {
    LONG v = *(LONG *)args[2];

    if (v < 1 || v > (LONG)MAX_CH) {
      out("ERROR: CHANNELS must be 1 to %u\n", (unsigned int)MAX_CH);
      FreeArgs(rda);
      return EXIT_USAGE;
    }
    opt.channels = (ULONG)v;
  }
  if (args[3]) {
    LONG v = *(LONG *)args[3];

    if (v < 1 || v > (LONG)MAX_SECONDS) {
      out("ERROR: SECONDS must be 1 to %lu\n", (unsigned long)MAX_SECONDS);
      FreeArgs(rda);
      return EXIT_USAGE;
    }
    opt.seconds = (ULONG)v;
  }
  if (args[4]) {
    LONG v = *(LONG *)args[4];

    if (v < 1 || v > (LONG)MAX_CYCLES) {
      out("ERROR: CYCLES must be 1 to %lu\n", (unsigned long)MAX_CYCLES);
      FreeArgs(rda);
      return EXIT_USAGE;
    }
    opt.cycles = (ULONG)v;
  }
  FreeArgs(rda);
  opt.voice = (opt.mode == MODE_HW_NEAREST || opt.mode == MODE_HW_LINEAR ||
               opt.mode == MODE_HW_CUBIC);

  port = CreateMsgPort();
  if (port)
    request = (struct AHIRequest *)
        CreateIORequest(port, sizeof(struct AHIRequest));
  if (!request) {
    out("ERROR: unable to create the AHI request\n");
    result = EXIT_ALLOC;
    goto cleanup;
  }
  request->ahir_Version = 4;
  device_open = OpenDevice((CONST_STRPTR)AHINAME, AHI_NO_UNIT,
                           (struct IORequest *)request, 0);
  if (device_open) {
    out("ERROR: unable to open %s version 4 (%d)\n", AHINAME,
        (int)device_open);
    result = EXIT_ALLOC;
    goto cleanup;
  }
  AHIBase = (struct Library *)request->ahir_Std.io_Device;

  out("ZZAXVoiceTest: mode $%08lX (%s), %lu s per scenario\n",
      (unsigned long)opt.mode,
      opt.voice ? "hardware mixing" : "software mixing",
      (unsigned long)opt.seconds);

  for (i = first; i <= last && !g_break; i++) {
    int rc;

    stats_reset();
    out("S%d %s\n", i, scenarios[i - 1].name);
    g_why[0] = '\0';
    rc = scenarios[i - 1].run();
    if (rc == R_NOALLOC) {
      out("S%d FAIL: could not allocate the audio hardware\n", i);
      result = EXIT_ALLOC;
      goto close;
    }
    if (rc == R_PASS && g_stats_bad) {
      rc = why(R_FAIL, "voice stats were missing, malformed, or counted "
               "late, rejected, underrun or dropped records");
    }
    if (rc == R_PASS) {
      out("S%d PASS: %s\n", i, g_why);
      passed++;
    } else {
      out("S%d FAIL: %s\n", i, g_why);
      failed++;
    }
  }
  if (g_break) {
    out("Aborted.\n");
    failed++;
  }
  out("ZZAXVoiceTest: %d passed, %d failed\n", passed, failed);
  if (failed) result = EXIT_FAILED;

close:
  CloseDevice((struct IORequest *)request);
cleanup:
  timer_close();
  if (request) DeleteIORequest((struct IORequest *)request);
  if (port) DeleteMsgPort(port);
  return result;
}
