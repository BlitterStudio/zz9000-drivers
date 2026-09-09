/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Target-side regression: compile the real driver with controlled scheduling,
 * direct hook dispatch and coherent RAM instead of hardware. Hooks model
 * AHI 4.180's timer state; real fabric/legacy paths must resume ordered PCM.
 * Run on AmigaOS or with vamos --cpu=68020 --stack-size=64.
 */
#include <stdio.h>
#include <stdlib.h>
#include <exec/exec.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/utility.h>
#include <utility/hooks.h>

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
/* The resident owns DOSBase, unlike an executable's libc startup. */
#define DOSBase test_driver_dos_base
#define _start timer_test_driver_start
#include "zz9000ax-ahi.c"
#undef _start

/* Unused resident initialization must not auto-open hardware libraries. */
struct ExpansionBase *ExpansionBase = NULL;
struct IntuitionBase *IntuitionBase = NULL;

#define TEST_ROUNDS 3U
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
  BYTE next_signal;
  int failures;
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
}

static BYTE test_alloc_signal(LONG number)
{
  (void)number;
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

static ULONG test_wait(ULONG signals)
{
  (void)signals;
  if (fixture.rounds == 1U)
    fixture.bytes_after_skip = published_bytes();
  if (fixture.rounds == TEST_ROUNDS)
    return SIGBREAKF_CTRL_C;
  fixture.rounds++;
  fixture.now = TEST_ENTRY + fixture.rounds * TEST_PERIOD;
  return 1UL << fixture.driver.enable_signal;
}

static ULONG test_player(struct Hook *hook asm("a0"),
                          struct AHIAudioCtrlDrv *audio asm("a2"),
                          APTR message asm("a1"))
{
  (void)hook;
  (void)message;
  check(audio == &fixture.audio, "player receives its audio control");
  fixture.players++;
  return 0;
}

static ULONG test_mixer(struct Hook *hook asm("a0"),
                         struct AHIAudioCtrlDrv *audio asm("a2"),
                         APTR buffer asm("a1"))
{
  UWORD *samples = buffer;
  ULONG frame;
  (void)hook;
  check(audio == &fixture.audio, "mixer receives its audio control");
  fixture.mixes++;
  if (audio->ahiac_BuffSamples > BOUNCE_MAX_FRAMES) {
    check(0, "oversized period never reaches the mixer");
    return 0;
  }
  for (frame = 0; frame < audio->ahiac_BuffSamples; frame++) {
    samples[2U * frame] = 0x1234U;
    samples[2U * frame + 1U] = 0xa1b2U;
  }
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

static void prepare(int fabric, int overload)
{
  ZZ9KAudioRingSession *session;
  memset(&fixture, 0, sizeof(fixture));
  fixture.entry = TEST_ENTRY;
  fixture.exit = TEST_ENTRY + (overload ? 13500UL : 100UL);
  fixture.player.h_Entry = (ULONG (*)())test_player;
  fixture.mixer.h_Entry = (ULONG (*)())test_mixer;
  fixture.audio.ahiac_PlayerFunc = &fixture.player;
  fixture.audio.ahiac_MixerFunc = &fixture.mixer;
  fixture.audio.ahiac_PreTimer = test_pre_timer;
  fixture.audio.ahiac_PostTimer = test_post_timer;
  fixture.audio.ahiac_MixFreq = 48000UL;
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
    WorkerProcess();
    /* Production teardown leaves Forbid held until the worker exits. */
    Permit();
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

static int oversize_case(void)
{
  prepare(0, 0);
  fixture.audio.ahiac_BuffSamples = BOUNCE_MAX_FRAMES + 1U;
  WorkerProcess();
  Permit();
  check(fixture.pres == TEST_ROUNDS && fixture.posts == TEST_ROUNDS,
        "oversized legacy periods close each timer exactly once");
  check(fixture.mixes == 0 && published_bytes() == 0,
        "oversized legacy periods cannot reach mixer or hardware");
  if (!fixture.failures)
    puts("PASS: oversized legacy periods close timing without delivery");
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
  failures += oversize_case();
  CloseLibrary(UtilityBase);
  return failures ? 20 : 0;
}
