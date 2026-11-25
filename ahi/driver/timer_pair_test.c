/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Target-side regression: compile the real driver with controlled scheduling,
 * direct hook dispatch and coherent RAM instead of hardware. Hooks model
 * AHI 4.180's timer state; real fabric/legacy paths must resume ordered PCM.
 * Run on AmigaOS or with vamos --cpu=68020 -m 8000 --stack-size=64. In the
 * amigadev/crosstools:m68k-amigaos image, from ahi/driver after build.sh
 * staged zz9k-headers (the driver links the voice model, so it is part of
 * the build):
 *   m68k-amigaos-gcc timer_pair_test.c voice_model.c asmfuncs.s -O1 -m68020 \
 *       -I../../include -Izz9k-headers -Wl,--allow-multiple-definition \
 *       -o timer_pair_test -ldebug
 *   vamos --cpu=68020 -m 8000 --stack-size=64 timer_pair_test
 * (--allow-multiple-definition lets the driver's SysBase coexist with the
 * startup file's.)
 */
#include <stdio.h>
#include <stdlib.h>
#include <exec/exec.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/utility.h>
#include <utility/hooks.h>

static int forbid_depth;
static LONG test_open_device(void);
static struct Task *test_find_task(CONST_STRPTR name);
static ULONG test_wait(ULONG signals);
static void test_signal(struct Task *task, ULONG signals);
static BYTE test_alloc_signal(LONG number);
static ULONG test_call_hook(struct Hook *hook, APTR object, APTR message);

#undef FindTask
#undef Wait
#undef Signal
#undef AllocSignal
#undef FreeSignal
#undef CacheClearE
#undef CallHookPkt
#define FindTask(name) test_find_task(name)
#define Wait(signals) test_wait(signals)
#define Signal(task, signals) test_signal(task, signals)
#define AllocSignal(number) test_alloc_signal(number)
#define FreeSignal(number) ((void)(number))
#define CacheClearE(address, length, caches) \
  ((void)(address), (void)(length), (void)(caches))
#define CallHookPkt(hook, object, message) test_call_hook(hook, object, message)
/* Forbid is counted, not run: the cases check that a worker publishes its
 * exit under Forbid and returns with it still held. */
#undef Forbid
#undef Permit
#undef OpenDevice
#define Forbid() (forbid_depth++)
#define Permit() (forbid_depth--)
#define OpenDevice(name, unit, request, flags) test_open_device()
/* The resident owns DOSBase, unlike an executable's libc startup. */
#define DOSBase test_driver_dos_base
#define _start timer_test_driver_start
#include "zz9000ax-ahi.c"
#undef _start

#define TEST_ROUNDS 3U
#define TEST_RATE 48000UL
#define TEST_PERIOD_FRAMES (TEST_RATE / 50UL)
#define TEST_MAX_TICKS 700U
#define TEST_PERIOD 14158UL
#define TEST_ENTRY 1000000UL

static struct {
  struct z9ax driver;
  struct AHIAudioCtrlDrv audio;
  struct Process process;
  struct Hook player;
  struct Hook mixer;
  ZZ9KAudioRingProducerLine producer;
  ZZ9KAudioRingFirmwareLine firmware;
  ULONG bounce[ZZ_AX_BYTES_PER_PERIOD / 4U];
  ULONG accum[ZZ_AX_BYTES_PER_PERIOD / 4U];
  ULONG ring[ZZ_AX_AUDIO_BUFSZ / 4U];
  UWORD registers[4096];
  ULONG now, entry, exit;
  ULONG players, mixes, pres, posts, rounds;
  ULONG bytes_after_skip;
  /* PlayerFreq cases: per-tick positions on the frame timeline, frames
   * handed to the mixer, run length and a mid-run tempo change. */
  ULONG rounds_limit, switch_round, pf_after;
  ULONG frames_mixed, max_mix_frames, player_pos[TEST_MAX_TICKS];
  ULONG players_at_switch, frames_at_switch;
  int ramp;
  BYTE next_signal;
  int failures;
  int fail_signals;
  ULONG signals;
  int last_signal_forbid;
} fixture;

static void check(int condition, const char *message)
{
  if (!condition) {
    printf("FAIL (%s): %s\n", fixture.driver.fabric_mode ? "fabric" : "legacy", message);
    fixture.failures++;
  }
}

static struct Task *test_find_task(CONST_STRPTR name)
{
  (void)name;
  return &fixture.process.pr_Task;
}

static void test_signal(struct Task *task, ULONG signals)
{
  (void)task;
  (void)signals;
  fixture.signals++;
  fixture.last_signal_forbid = forbid_depth;
}

static BYTE test_alloc_signal(LONG number)
{
  (void)number;
  if (fixture.fail_signals)
    return -1;
  return fixture.next_signal++;
}

static ULONG test_call_hook(struct Hook *hook, APTR object, APTR message)
{
  typedef ULONG (*Entry)(struct Hook *h asm("a0"), APTR o asm("a2"),
                        APTR m asm("a1"));
  return ((Entry)hook->h_Entry)(hook, object, message);
}

static ULONG published_bytes(void)
{
  if (fixture.driver.fabric_mode)
    return (ULONG)fixture.driver.lease_session.write_cursor;
  return fixture.driver.buf_offset;
}

static void begin_round(void)
{
  fixture.rounds++;
  fixture.now = TEST_ENTRY + fixture.rounds * TEST_PERIOD;
  if (fixture.rounds == fixture.switch_round) {
    fixture.players_at_switch = fixture.players;
    fixture.frames_at_switch = fixture.frames_mixed;
    fixture.audio.ahiac_PlayerFreq = fixture.pf_after;
    intAHIsub_Update(0, &fixture.audio);
  }
}

static ULONG test_wait(ULONG signals)
{
  (void)signals;
  if (fixture.rounds == 1U)
    fixture.bytes_after_skip = published_bytes();
  if (fixture.rounds == fixture.rounds_limit)
    return SIGBREAKF_CTRL_C;
  begin_round();
  return 1UL << fixture.driver.enable_signal;
}

static ULONG test_player(struct Hook *hook asm("a0"),
                          struct AHIAudioCtrlDrv *audio asm("a2"),
                          APTR message asm("a1"))
{
  (void)hook;
  (void)message;
  check(audio == &fixture.audio, "player receives its audio control");
  /* Position of this call on the mixed-frame timeline (no skips in the
   * cases that compare positions). */
  if (fixture.players < TEST_MAX_TICKS)
    fixture.player_pos[fixture.players] = fixture.frames_mixed;
  fixture.players++;
  return 0;
}

/* Models ahi.device's Mix(): it clears the driver's whole mixing buffer
 * (ahiac_BuffSizeNow, one full 20 ms period) at the pointer it is given and
 * then writes ahiac_BuffSamples frames from its start. */
static ULONG test_mixer(struct Hook *hook asm("a0"),
                         struct AHIAudioCtrlDrv *audio asm("a2"),
                         APTR buffer asm("a1"))
{
  UWORD *samples = buffer;
  ULONG frame, id;
  (void)hook;
  check(audio == &fixture.audio, "mixer receives its audio control");
  check(buffer == (APTR)fixture.bounce,
        "mixer gets the start of the full-size bounce buffer");
  fixture.mixes++;
  if (audio->ahiac_BuffSamples > BOUNCE_MAX_FRAMES ||
      audio->ahiac_BuffSamples == 0U) {
    check(0, "mixer is only called for 1..960 frames");
    return 0;
  }
  memset(buffer, 0, sizeof(fixture.bounce));
  if (audio->ahiac_BuffSamples > fixture.max_mix_frames)
    fixture.max_mix_frames = audio->ahiac_BuffSamples;
  for (frame = 0; frame < audio->ahiac_BuffSamples; frame++) {
    id = fixture.frames_mixed + frame;
    samples[2U * frame] = fixture.ramp ? (UWORD)(id >> 16) : 0x1234U;
    samples[2U * frame + 1U] = fixture.ramp ? (UWORD)id : 0xa1b2U;
  }
  fixture.frames_mixed += audio->ahiac_BuffSamples;
  return 0;
}

/* Exact unsigned/low-byte accounting of the released 4.180 timer routine.
 * The first round sees a prior overload. A skipped PostTimer leaves exit
 * behind entry, and the next round falsely skips again for this fixture.
 */
static BOOL test_pre_timer(void)
{
  ULONG cost = fixture.exit - fixture.entry;
  ULONG period = fixture.now - fixture.entry;
  ULONG used;
  fixture.pres++;
  fixture.entry = fixture.now;
  if (cost == 0)
    return FALSE;
  used = (cost << 8) / period;
  return (used & 255UL) > 230UL;
}

static void test_post_timer(void)
{
  fixture.posts++;
  fixture.exit = fixture.now + 100UL;
}

/* The worker as the process runs it: it signals its exit under Forbid and
 * returns with Forbid held (dos drops it when it removes the process). */
static void run_worker(void)
{
  fixture.driver.worker_process = &fixture.process;
  WorkerProcess();
  check(fixture.signals >= 1U, "the worker signals its parent");
  check(fixture.last_signal_forbid > 0, "the exit is signalled under Forbid");
  check(forbid_depth == 1, "the worker returns with Forbid held once");
  check(fixture.driver.worker_process == NULL,
        "the worker publishes its exit");
  Permit();
}

static void prepare(int fabric, int overload)
{
  ZZ9KAudioRingSession *session;
  memset(&fixture, 0, sizeof(fixture));
  forbid_depth = 0;
  fixture.entry = TEST_ENTRY;
  fixture.exit = TEST_ENTRY + (overload ? 13500UL : 100UL);
  fixture.rounds_limit = TEST_ROUNDS;
  fixture.player.h_Entry = (ULONG (*)())test_player;
  fixture.mixer.h_Entry = (ULONG (*)())test_mixer;
  fixture.audio.ahiac_PlayerFunc = &fixture.player;
  fixture.audio.ahiac_MixerFunc = &fixture.mixer;
  fixture.audio.ahiac_PreTimer = test_pre_timer;
  fixture.audio.ahiac_PostTimer = test_post_timer;
  fixture.audio.ahiac_MixFreq = 48000UL;
  fixture.audio.ahiac_PlayerFreq = 50UL << 16;
  fixture.audio.ahiac_MinPlayerFreq = 25UL << 16;
  fixture.audio.ahiac_MaxPlayerFreq = 60UL << 16;
  fixture.audio.ahiac_BuffSamples = BOUNCE_MAX_FRAMES;
  fixture.driver.audioctrl = &fixture.audio;
  fixture.driver.audio_buf_addr = (ULONG)fixture.bounce;
  fixture.driver.audio_hw_buf_addr = (ULONG)fixture.ring;
  fixture.driver.hw_addr = (ULONG)fixture.registers;
  fixture.driver.record_stop = TRUE;
  fixture.driver.fabric_mode = fabric;
  fixture.driver.lease_held = TRUE;
  fixture.driver.lease_accum = (UBYTE *)fixture.accum;
  fixture.driver.t_mainproc = &fixture.process.pr_Task;
  fixture.process.pr_Task.tc_UserData = &fixture.driver;
  session = &fixture.driver.lease_session;
  session->mapped = TRUE;
  session->grant.generation = 1U;
  session->grant.source_rate = 48000U;
  session->grant.ring_capacity = sizeof(fixture.ring);
  session->ring = (UBYTE *)fixture.ring;
  session->producer_line = &fixture.producer;
  session->firmware_line = &fixture.firmware;
  session->flags = ZZ9K_AUDIO_RING_PRODUCER_FLAG_PAUSED;
  zz9k_audio_ring_firmware_publish(&fixture.firmware, 1U, 0,
                                   ZZ9K_AUDIO_RING_STATUS_OK);
}

static int recovery_case(int fabric)
{
  const UBYTE *pcm;
  ULONG byte;
  prepare(fabric, 1);
  if (fabric) {
    /* Exercise the real pump without a timer.device dependency. */
    for (fixture.rounds = 1; fixture.rounds <= TEST_ROUNDS;
         fixture.rounds++) {
      fixture.now = TEST_ENTRY + fixture.rounds * TEST_PERIOD;
      fabric_lease_pump(&fixture.driver, &fixture.audio);
      if (fixture.rounds == 1U)
        fixture.bytes_after_skip = published_bytes();
    }
  } else {
    /* The legacy loop uses the substituted Wait, real period delivery and
     * fake register/RAM addresses; it never accesses a physical card. */
    run_worker();
  }
  check(fixture.bytes_after_skip == 0,
        "overload skip does not publish stale PCM");
  check(fixture.players == TEST_ROUNDS && fixture.pres == TEST_ROUNDS &&
        fixture.posts == TEST_ROUNDS,
        "every active timer entry is closed, including an overload skip");
  check(fixture.mixes == TEST_ROUNDS - 1U &&
        published_bytes() == (TEST_ROUNDS - 1U) * ZZ_AX_BYTES_PER_PERIOD,
        "normal PCM delivery resumes immediately after overload skip");
  pcm = (const UBYTE *)fixture.ring;
  for (byte = 0; byte < (TEST_ROUNDS - 1U) * ZZ_AX_BYTES_PER_PERIOD;
       byte += 4U) {
    if (fabric ? (pcm[byte] != 0x34 || pcm[byte + 1] != 0x12 ||
                  pcm[byte + 2] != 0xb2 || pcm[byte + 3] != 0xa1)
               : (pcm[byte] != 0x12 || pcm[byte + 1] != 0x34 ||
                  pcm[byte + 2] != 0xa1 || pcm[byte + 3] != 0xb2)) {
      check(0, "recovered periods retain complete correctly ordered PCM");
      break;
    }
  }
  if (fabric)
    check(!(fixture.driver.lease_session.flags &
            ZZ9K_AUDIO_RING_PRODUCER_FLAG_PAUSED),
          "recovered complete periods satisfy the unchanged startup runway");
  if (!fixture.failures)
    printf("PASS: %s skip closes timing and resumes complete PCM\n",
           fabric ? "fabric" : "legacy");
  return fixture.failures;
}

static LONG test_open_device(void)
{
  return 1; /* timer.device refuses */
}

/* Exit order of the worker, normal and failed. Every exit publishes the
 * worker as gone under Forbid and returns with that Forbid still held, so
 * the waiting task cannot run (and unload the driver) until the process is
 * removed. 0: no signal bits, 1: fabric mode without a pacing timer, 2: a
 * normal run to CTRL-C. */
static int worker_exit_case(int mode)
{
  prepare(mode == 1, 0);
  fixture.fail_signals = mode == 0;
  run_worker();
  if (mode == 2) {
    check(fixture.signals == 2U,
          "a normal run signals the handshake, then its exit");
  } else {
    check(fixture.signals == 1U && fixture.mixes == 0U,
          "a failed start signals once, before the mix loop");
  }
  if (mode == 1)
    check(fixture.driver.lease_timer_req == NULL &&
              fixture.driver.lease_timer_port == NULL,
          "the failed pacing timer is released before the exit");
  if (!fixture.failures)
    printf("PASS: worker exit order (%s)\n",
           mode == 0 ? "no signals" : mode == 1 ? "no pacing timer" : "normal");
  return fixture.failures;
}

static int oversize_case(void)
{
  prepare(0, 0);
  fixture.audio.ahiac_BuffSamples = BOUNCE_MAX_FRAMES + 1U;
  run_worker();
  check(fixture.pres == TEST_ROUNDS && fixture.posts == TEST_ROUNDS,
        "oversized legacy periods close each timer exactly once");
  check(fixture.mixes == 0 && published_bytes() == 0,
        "oversized legacy periods cannot reach mixer or hardware");
  if (!fixture.failures)
    puts("PASS: oversized legacy periods close timing without delivery");
  return fixture.failures;
}

static int playerfreq_skip_case(int fabric)
{
  prepare(fabric, 1);
  fixture.audio.ahiac_PlayerFreq = 60UL << 16;
  if (fabric) {
    while (fixture.rounds < fixture.rounds_limit) {
      begin_round();
      fabric_lease_pump(&fixture.driver, &fixture.audio);
    }
  } else {
    run_worker();
  }
  check(fixture.players == 4U,
        "60 Hz player clock advances through the skipped transport period");
  check(fixture.pres == TEST_ROUNDS && fixture.posts == TEST_ROUNDS,
        "split periods and skip preserve one timer pair per wake");
  check(fixture.frames_mixed == (TEST_ROUNDS - 1U) * TEST_PERIOD_FRAMES &&
        published_bytes() == (TEST_ROUNDS - 1U) * ZZ_AX_BYTES_PER_PERIOD,
        "split mixing resumes complete transport periods after a skip");
  if (!fixture.failures)
    printf("PASS: %s 60 Hz skip preserves player cadence and timing\n",
           fabric ? "fabric" : "legacy");
  return fixture.failures;
}

static int playerfreq_case(int fabric, int fractional)
{
  ULONG tick, expected, period, frame, offset, id;
  const UBYTE *pcm;
  prepare(fabric, 0);
  fixture.ramp = 1;
  fixture.rounds_limit = fractional ? 500U : 100U;
  fixture.audio.ahiac_PlayerFreq =
      fractional ? (59UL << 16) + 32768UL : 60UL << 16;
  if (!fractional) {
    fixture.switch_round = 51U;
    fixture.pf_after = 25UL << 16;
  }
  if (fabric) {
    while (fixture.rounds < fixture.rounds_limit) {
      begin_round();
      zz9k_audio_ring_firmware_publish(&fixture.firmware, 1U,
          fixture.driver.lease_session.write_cursor,
          ZZ9K_AUDIO_RING_STATUS_OK);
      fabric_lease_pump(&fixture.driver, &fixture.audio);
    }
  } else {
    run_worker();
  }
  expected = fractional ? 595UL : 85UL;
  check(fixture.players == expected, "PlayerFreq determines tick count");
  check(fixture.frames_mixed == fixture.rounds_limit * TEST_PERIOD_FRAMES,
        "exactly one second of source frames mixed per 50 transport periods");
  check(fixture.pres == fixture.rounds_limit &&
        fixture.posts == fixture.rounds_limit,
        "one timer pair per transport wake, not per back-to-back mix chunk");
  if (!fractional) {
    check(fixture.players_at_switch == 60U &&
          fixture.frames_at_switch == TEST_RATE,
          "60 ticks and 48000 frames in first second");
    check(fixture.players - fixture.players_at_switch == 25U &&
          fixture.frames_mixed - fixture.frames_at_switch == TEST_RATE,
          "Update switches to 25 ticks and 48000 frames in next second");
  }
  for (tick = 0; tick < expected && tick < fixture.players; tick++) {
    ULONG position = fractional ? tick * 96000UL / 119UL :
        tick < 60U ? tick * 800UL :
        TEST_RATE + (tick - 60U) * 1920UL;
    if (fixture.player_pos[tick] != position) {
      check(0, "PlayerFunc boundaries carry fractional frames without drift");
      break;
    }
  }
  /* Inspect the last eight complete ring slots after wrap. AHI Mix clears
   * its whole output buffer each chunk, so this also detects mixing into
   * interior pointers or losing a prefix/suffix at a tick boundary. */
  pcm = (const UBYTE *)fixture.ring;
  for (period = fixture.rounds_limit - ZZ_AX_AUDIO_PERIODS;
       period < fixture.rounds_limit; period++) {
    offset = (period % ZZ_AX_AUDIO_PERIODS) * ZZ_AX_BYTES_PER_PERIOD;
    for (frame = 0; frame < TEST_PERIOD_FRAMES; frame++, offset += 4U) {
      if (fabric)
        id = ((ULONG)pcm[offset + 1U] << 24) |
             ((ULONG)pcm[offset] << 16) |
             ((ULONG)pcm[offset + 3U] << 8) | pcm[offset + 2U];
      else
        id = ((ULONG)pcm[offset] << 24) |
             ((ULONG)pcm[offset + 1U] << 16) |
             ((ULONG)pcm[offset + 2U] << 8) | pcm[offset + 3U];
      if (id != period * TEST_PERIOD_FRAMES + frame) {
        check(0, "transport ring contains continuous whole periods");
        period = fixture.rounds_limit;
        break;
      }
    }
  }
  if (!fixture.failures)
    printf("PASS: %s %s PlayerFreq, frames, timer pairs and ring wrap\n",
           fabric ? "fabric" : "legacy",
           fractional ? "59.5 Hz fractional" : "60 to 25 Hz");
  return fixture.failures;
}

int main(void)
{
  int failures = 0;
  SysBase = *(struct ExecBase **)4;
  UtilityBase = OpenLibrary((STRPTR)"utility.library", 37);
  if (!UtilityBase) {
    puts("FAIL: utility.library unavailable");
    return 20;
  }
  failures += recovery_case(1);
  failures += recovery_case(0);
  failures += worker_exit_case(0);
  failures += worker_exit_case(1);
  failures += worker_exit_case(2);
  failures += oversize_case();
  failures += playerfreq_case(0, 0);
  failures += playerfreq_case(1, 0);
  failures += playerfreq_case(0, 1);
  failures += playerfreq_case(1, 1);
  failures += playerfreq_skip_case(0);
  failures += playerfreq_skip_case(1);
  CloseLibrary(UtilityBase);
  return failures ? 20 : 0;
}
