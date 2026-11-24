/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Target-side regression for the hardware-mixing modes: compile the real
 * driver with a fake voice session and controlled scheduling instead of a
 * card. The fake session answers the six zz9k.library voice calls and the
 * mailbox, the worker's init, service and end steps run by hand, and the
 * test checks what the driver sends and what it refuses. Like the firmware,
 * the fake models the library's client ids (a task gets one by opening
 * zz9k.library, 0 otherwise; ZZ9KCall carries the caller's id, the Voice
 * API always 0), ties the voice session to the id it was opened under and
 * answers a request under any other id with BAD_HANDLE.
 *
 * Run on AmigaOS or with vamos --cpu=68020 -m 8000 --stack-size=64 (the
 * default vamos memory is too small for the fixture). In the
 * amigadev/crosstools:m68k-amigaos image, from ahi/driver after build.sh
 * staged zz9k-headers:
 *   m68k-amigaos-gcc voice_worker_test.c voice_model.c asmfuncs.s -O1 -m68020-60 \
 *       -I../../include -Izz9k-headers -Wl,--allow-multiple-definition \
 *       -o voice_worker_test -ldebug
 *   vamos --cpu=68020 -m 8000 --stack-size=64 voice_worker_test
 * (--allow-multiple-definition lets the driver's SysBase coexist with the
 * startup file's.)
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <exec/exec.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/utility.h>
#include <utility/hooks.h>
#include <zz9k/library_vectors.h>
#include <zz9k/request.h>
#include <proto/zz9k.h>
#include <clib/debug_protos.h>

static struct Task *test_find_task(CONST_STRPTR name);
static ULONG test_wait(ULONG signals);
static void test_signal(struct Task *task, ULONG signals);
static BYTE test_alloc_signal(LONG number);
static ULONG test_call_hook(struct Hook *hook, APTR object, APTR message);
static struct Node *test_find_name(struct List *list, CONST_STRPTR name);
static void *test_alloc_vec(ULONG size, ULONG flags);
static void test_free_vec(void *memory);
static struct Process *test_create_process(Tag first_tag, ...);
static struct Library *test_open_library(CONST_STRPTR name, ULONG version);
static void test_close_library(struct Library *library);
static void test_add_int_server(ULONG int_number, struct Interrupt *interrupt);
static void test_rem_int_server(ULONG int_number, struct Interrupt *interrupt);
static LONG test_get_var(const char *name, char *buffer, LONG size,
                         ULONG flags);
static BOOL test_set_var(const char *name, const char *buffer, LONG size,
                         ULONG flags);
static int test_zz9k_call(ZZ9KRequest *request, ZZ9KMailboxEntry *reply,
                          ULONG timeout);
static int test_query_caps(ZZ9KCaps *caps);
static int zz_ax_voice_open(const ZZ9KVoiceOpenDesc *desc,
                            ZZ9KVoiceSession *session);
static int zz_ax_voice_close(ZZ9KVoiceSession *session);
static int zz_ax_voice_reset(ZZ9KVoiceSession *session);
static uint32_t zz_ax_voice_write(ZZ9KVoiceSession *session,
                                  const ZZ9KVoiceRecord *records,
                                  uint32_t count);
static int zz_ax_voice_get_status(ZZ9KVoiceSession *session,
                                  ZZ9KVoiceStatus *status);

#undef FindTask
#undef FindName
#undef Wait
#undef Signal
#undef AllocSignal
#undef FreeSignal
#undef CacheClearE
#undef CallHookPkt
#undef CreateNewProcTags
#undef OpenLibrary
#undef CloseLibrary
#undef AddIntServer
#undef RemIntServer
#undef GetVar
#undef SetVar
#undef ZZ9KCall
#undef ZZ9KQueryCaps
#define FindTask(name) test_find_task(name)
#define Wait(signals) test_wait(signals)
#define Signal(task, signals) test_signal(task, signals)
#define AllocSignal(number) test_alloc_signal(number)
#define FreeSignal(number) ((void)(number))
#undef AllocVec
#undef FreeVec
#define AllocVec(size, flags) test_alloc_vec(size, flags)
#define FreeVec(memory) test_free_vec(memory)
#undef FindName
#define FindName(list, name) test_find_name(list, name)
/* The driver's debug lines go to the emulator's debug channel, which this
 * image's debug library cannot reach; the cases check state, not text. */
#undef KPrintF
#define KPrintF(...) ((void)0)
#define CacheClearE(address, length, caches) \
  ((void)(address), (void)(length), (void)(caches))
#define CallHookPkt(hook, object, message) test_call_hook(hook, object, message)
#define CreateNewProcTags(first, ...) test_create_process(first, __VA_ARGS__)
#define OpenLibrary(name, version) test_open_library(name, version)
#define CloseLibrary(library) test_close_library(library)
#define AddIntServer(number, interrupt) test_add_int_server(number, interrupt)
#define RemIntServer(number, interrupt) test_rem_int_server(number, interrupt)
#define GetVar(name, buffer, size, flags) \
  test_get_var((const char *)(name), (char *)(buffer), size, flags)
#define SetVar(name, buffer, size, flags) \
  test_set_var((const char *)(name), (const char *)(buffer), size, flags)
#define ZZ9KCall(request, reply, timeout) \
  test_zz9k_call(request, reply, timeout)
#define ZZ9KQueryCaps(caps) test_query_caps(caps)
/* The executable's startup owns the DOS base the stubs use. */
#define DOSBase test_driver_dos_base
#define _start voice_test_driver_start
#define ZZ_AX_VOICE_TEST 1
#include "zz9000ax-ahi.c"
#undef _start

/* The worker's steps run as the worker task: the fake firmware, like the
 * real one, ties a voice session to the task that opened it. */
static void worker_task_enter(struct Task **saved);
static void worker_task_leave(struct Task *saved);

static int worker_run_init(struct z9ax *ahi_data)
{
  struct Task *saved;
  int ready;

  worker_task_enter(&saved);
  ready = voice_worker_init(ahi_data);
  worker_task_leave(saved);
  return ready;
}

static void worker_run_end(struct z9ax *ahi_data, int live)
{
  struct Task *saved;

  worker_task_enter(&saved);
  voice_worker_end(ahi_data, live);
  worker_task_leave(saved);
}

static void worker_run_service(struct z9ax *ahi_data)
{
  struct Task *saved;

  worker_task_enter(&saved);
  voice_service(ahi_data);
  worker_task_leave(saved);
}

/* Every case and test_wait below reach the worker through these. */
#define voice_worker_init(ahi_data) worker_run_init(ahi_data)
#define voice_worker_end(ahi_data, live) worker_run_end(ahi_data, live)
#define voice_service(ahi_data) worker_run_service(ahi_data)

/* zz9k.library's Voice API (ZZ9KVoiceOpen, Close, Reset) sends its requests
 * through the host's zz9k_call, which never stamps the client id of the
 * context into the request (only zz9k_submit does, and so ZZ9KCall and
 * ZZ9KCallAsync): they always carry client 0. */
#define FAKE_VOICE_API_ID 0U

#define FAKE_OPENERS 4U
#define FAKE_BANK_BYTES (2U * 1024U * 1024U)
#define FAKE_LEAD 7680U
#define FAKE_RESET_JUMP 1920U    /* periods the TX ring held, staged but unplayed */
#define FAKE_SERVICE_FLAGS \
  (ZZ9K_SERVICE_FLAG_AUDIO_VOICE | ZZ9K_SERVICE_FLAG_AUDIO_VOICE_STEREO)

/* The fake firmware, library and card. */
static struct {
  struct Library library;      /* zz9k.library */
  struct z9ax_base base;       /* the driver base */
  uint8_t registers[512];
  uint8_t bank[FAKE_BANK_BYTES];
  uint8_t ring[ZZ9K_VOICE_RING_MAX_BYTES];
  uint8_t status_block[ZZ9K_VOICE_STATUS_BYTES];
  uint8_t staging[65536];
  uint32_t service_flags;
  uint16_t revision;
  uint8_t library_present;
  uint8_t zorro;
  int open_status;
  int call_fail_after;         /* mailbox call count that fails, 0 = never */
  uint32_t calls;
  uint32_t opens, closes, resets, writes, uploads, defines;
  uint32_t library_opens, library_closes;
  uint32_t int_servers_added, int_servers_removed;
  uint32_t playing_frame, horizon;
  uint32_t state;
  uint32_t late;
  uint32_t late_records;          /* records written stamped before the horizon */
  uint8_t slot_live;              /* the first records reached the engine */
  uint16_t read_cursor;
  LONG write_budget;           /* records the ring still takes, -1 = no limit */
  uint32_t writes_outside_worker;
  uint16_t owner_id;              /* client id the voice session belongs to */
  uint8_t session_live;
  struct {
    struct Task *task;
    uint16_t id;
    uint32_t refs;
  } openers[FAKE_OPENERS];        /* zz9k.library's opener records */
  uint16_t next_client_id;
  uint32_t refused;               /* voice requests from a task but the owner */
  uint32_t query_caps;
  uint32_t features;              /* the features word the status block shows */
  uint32_t open_flags;            /* flags of the last open request */
  uint8_t mhi_mode;            /* 0 none, 1 legacy MHI, 2 fabric MHI */
  struct Node mhi_node;
} fw;

static struct {
  struct z9ax driver;
  struct z9ax_voice voice;
  struct AHIAudioCtrlDrv audio;
  struct Process process;
  struct Task main_task;
  struct Task other_task;         /* a second client task */
  struct Task *current;           /* the running task, NULL = main_task */
  struct Hook player;
  struct Hook sound;
  uint32_t players, sounds;
  uint32_t player_calls_pushed;   /* PlayerFunc scenarios that make calls */
  uint8_t in_hook;
  uint8_t in_interrupt;
  uint8_t worker_live;
  uint8_t stop_sent;
  BYTE next_signal;
  uint32_t last_signal;
  uint32_t stats_len;
  char stats[256];
  uint32_t lead_env;             /* ENV:ZZ9K_AHI_LEAD, 0 = absent */
  uint32_t bank_env;             /* ENV:ZZ9K_AHI_BANK (KiB), 0 = absent */
  int failures;
} fx;

static int failures_total;

static struct TagItem voice_tags[] = {
    {AHIDB_ZZVoice, 0xCAFE0002UL}, {AHIDB_Volume, TRUE}, {TAG_DONE, 0}};
static struct TagItem linear_tags[] = {
    {AHIDB_ZZVoiceLinear, 0xCAFE0003UL}, {TAG_DONE, 0}};
static struct TagItem cubic_tags[] = {
    {AHIDB_ZZVoiceCubic, 0xCAFE0004UL}, {TAG_DONE, 0}};
static struct TagItem soft_tags[] = {{AHIDB_AudioID, 0xCAFE0000UL},
                                     {TAG_DONE, 0}};

static void check(int condition, const char *message, ...)
{
  va_list args;

  if (!condition) {
    va_start(args, message);
    printf("FAIL: ");
    vprintf(message, args);
    printf("\n");
    va_end(args);
    fx.failures++;
    failures_total++;
  }
}

/* ---- exec, dos and library replacements ---- */

static struct Task *test_find_task(CONST_STRPTR name)
{
  (void)name;
  return fx.current ? fx.current : &fx.main_task;
}

static void worker_task_enter(struct Task **saved)
{
  *saved = fx.current;
  fx.current = &fx.process.pr_Task;
}

static void worker_task_leave(struct Task *saved)
{
  fx.current = saved;
}

/* The worker runs whenever a task blocks on it: a Start or Stop waits for
 * the worker's answer, and FreeAudio waits for the worker to exit. The
 * AllocAudio handshake is where the worker's init step runs. */
static ULONG test_wait(ULONG signals)
{
  struct z9ax *driver = &fx.driver;

  (void)signals;
  if (fx.stop_sent) {
    fx.stop_sent = 0;
    if (fx.worker_live) {
      voice_worker_end(driver, 1);
      fx.worker_live = 0;
    }
    driver->worker_process = NULL;
    return signals;
  }
  if (!fx.worker_live) {
    /* The handshake of AllocAudio: the worker publishes its result first. */
    if (voice_worker_init(driver))
      fx.worker_live = 1;
    else {
      voice_worker_end(driver, 0);
      driver->worker_process = NULL;
    }
    return signals;
  }
  if (driver->worker_process)
    voice_service(driver);
  return signals;
}

static void test_signal(struct Task *task, ULONG signals)
{
  (void)task;
  if (signals & SIGBREAKF_CTRL_C)
    fx.stop_sent = 1;
}

/* The ownership check lists the interrupt servers of the real Amiga. The
 * emulator has no such lists (SysBase->IntVects is empty), so the harness
 * answers for them: no MHI server, unless the case installs one. */
static struct Node *test_find_name(struct List *list, CONST_STRPTR name)
{
  (void)list;
  if (fw.mhi_mode == 0U)
    return NULL;
  /* The node answers only to its own name, as a real server list would. */
  if (strcmp((const char *)name,
             fw.mhi_mode == 1U ? ZZ_AX_IRQ_NAME_MHI
                               : ZZ_AX_IRQ_NAME_MHI_FABRIC) != 0)
    return NULL;
  return &fw.mhi_node;
}

/* The driver's two allocations come from the fixture, so the cases can
 * inspect them: the driver data and the voice state. */
static void *test_alloc_vec(ULONG size, ULONG flags)
{
  (void)flags;
  if (size == sizeof(struct z9ax)) {
    memset(&fx.driver, 0, sizeof(fx.driver));
    return &fx.driver;
  }
  if (size == sizeof(struct z9ax_voice)) {
    memset(&fx.voice, 0, sizeof(fx.voice));
    return &fx.voice;
  }
  check(0, "the hardware-mixing path allocates only its two structures (%lu)",
        (unsigned long)size);
  return NULL;
}

static void test_free_vec(void *memory)
{
  check(memory == &fx.driver || memory == &fx.voice,
        "only the driver's own structures are freed");
}

static BYTE test_alloc_signal(LONG number)
{
  (void)number;
  fx.last_signal = (uint32_t)fx.next_signal;
  return fx.next_signal++;
}

static ULONG test_call_hook(struct Hook *hook, APTR object, APTR message)
{
  typedef ULONG (*Entry)(struct Hook *h asm("a0"), APTR o asm("a2"),
                        APTR m asm("a1"));
  return ((Entry)hook->h_Entry)(hook, object, message);
}

static struct Process *test_create_process(Tag first_tag, ...)
{
  (void)first_tag;
  return &fx.process;
}

/* zz9k.library hands every task that opened it a client id of its own, and
 * a task that never opened it calls with id 0, as the library does. */
static uint16_t fake_client_id_of(const struct Task *task)
{
  uint32_t i;

  for (i = 0U; i < FAKE_OPENERS; i++) {
    if (fw.openers[i].refs != 0U && fw.openers[i].task == task)
      return fw.openers[i].id;
  }
  return 0U;
}

static uint16_t fake_client_id(void)
{
  return fake_client_id_of(test_find_task(NULL));
}

/* The firmware's voice_session_owned_by: the request's client id must be the
 * one the session was opened under. */
static int fake_refuses(uint16_t client_id)
{
  return fw.session_live && client_id != fw.owner_id;
}

static struct Library *test_open_library(CONST_STRPTR name, ULONG version)
{
  struct Task *task = test_find_task(NULL);
  uint32_t i;

  if (strcmp((const char *)name, ZZ9K_LIBRARY_NAME) == 0) {
    fw.library_opens++;
    if (!fw.library_present)
      return NULL;
    fw.library.lib_Revision = fw.revision;
    for (i = 0U; i < FAKE_OPENERS; i++) {
      if (fw.openers[i].refs != 0U && fw.openers[i].task == task)
        break;
    }
    if (i == FAKE_OPENERS) {
      for (i = 0U; i < FAKE_OPENERS && fw.openers[i].refs != 0U; i++)
        ;
      if (i == FAKE_OPENERS) {
        check(0, "the fake library has room for the openers");
        return NULL;
      }
      fw.openers[i].task = task;
      fw.openers[i].id = ++fw.next_client_id;
    }
    fw.openers[i].refs++;
    return &fw.library;
  }
  /* The driver opens no other library outside init(), which is not run. */
  check(0, "the driver opened a library through the fake");
  return NULL;
}

/* The last close of a task's opener releases its client id; the firmware
 * then shuts down a voice session that id still owns. */
static void test_close_library(struct Library *library)
{
  struct Task *task = test_find_task(NULL);
  uint32_t i;

  if (library != &fw.library) {
    check(0, "the driver closed a library it did not open through the fake");
    return;
  }
  fw.library_closes++;
  for (i = 0U; i < FAKE_OPENERS; i++) {
    if (fw.openers[i].refs != 0U && fw.openers[i].task == task)
      break;
  }
  if (i == FAKE_OPENERS) {
    check(0, "a task closed zz9k.library without having opened it");
    return;
  }
  fw.openers[i].refs--;
}

static void test_add_int_server(ULONG int_number, struct Interrupt *interrupt)
{
  (void)int_number;
  /* Exec calls is_Code for every interrupt in the chain. */
  check(interrupt->is_Code != NULL && interrupt->is_Data != NULL &&
            interrupt->is_Node.ln_Type == NT_INTERRUPT,
        "AddIntServer gets a prepared node (%s)",
        interrupt->is_Node.ln_Name ? interrupt->is_Node.ln_Name : "no name");
  fw.int_servers_added++;
}

static void test_rem_int_server(ULONG int_number, struct Interrupt *interrupt)
{
  (void)int_number;
  (void)interrupt;
  fw.int_servers_removed++;
}

/* A decimal number without printf's floating-point support, which the
 * 68020 emulator cannot run. */
static LONG put_number(char *buffer, LONG size, uint32_t value)
{
  char digits[12];
  LONG n = 0;
  LONG i;

  do {
    digits[n++] = (char)('0' + (LONG)(value % 10U));
    value /= 10U;
  } while (value != 0U);
  if (n >= size)
    return -1;
  for (i = 0; i < n; i++)
    buffer[i] = digits[n - 1 - i];
  buffer[n] = '\0';
  return n;
}

static LONG test_get_var(const char *name, char *buffer, LONG size,
                         ULONG flags)
{
  (void)flags;
  if (strcmp((const char *)name, "ZZ9K_AHI_LEAD") == 0 && fx.lead_env) {
    return put_number(buffer, size, fx.lead_env);
  }
  if (strcmp((const char *)name, "ZZ9K_AHI_BANK") == 0 && fx.bank_env) {
    return put_number(buffer, size, fx.bank_env);
  }
  if (strcmp((const char *)name, "ZZ9K_AHI_VOICE_STATS") == 0) {
    strncpy(buffer, fx.stats, (size_t)size - 1);
    buffer[size - 1] = '\0';
    return (LONG)strlen(buffer);
  }
  return -1;
}

static BOOL test_set_var(const char *name, const char *buffer, LONG size,
                         ULONG flags)
{
  (void)size;
  (void)flags;
  check(strcmp((const char *)name, "ZZ9K_AHI_VOICE_STATS") == 0,
        "the worker writes only ENV:ZZ9K_AHI_VOICE_STATS");
  strncpy(fx.stats, buffer, sizeof(fx.stats) - 1);
  fx.stats[sizeof(fx.stats) - 1] = '\0';
  fx.stats_len = (uint32_t)strlen(fx.stats);
  return TRUE;
}

/* ---- the mailbox ---- */

static int test_query_caps(ZZ9KCaps *caps)
{
  fw.query_caps++;
  memset(caps, 0, sizeof(*caps));
  return ZZ9K_STATUS_OK;
}

static int test_zz9k_call(ZZ9KRequest *request, ZZ9KMailboxEntry *reply,
                          ULONG timeout)
{
  ZZ9KServiceInfoPayload *info;
  const ZZ9KAudioVoiceUploadPayload *upload;
  uint32_t flags;
  uint32_t bank_offset;
  uint32_t length;
  uint32_t sample_id;
  uint32_t sample_length;
  uint32_t format;
  uint32_t staging_bytes;

  (void)timeout;
  fw.calls++;
  if (fw.call_fail_after && fw.calls >= fw.call_fail_after)
    return ZZ9K_STATUS_IO_ERROR;
  memset(reply, 0, sizeof(*reply));
  if (request->entry.opcode == ZZ9K_OP_QUERY_SERVICE) {
    info = (ZZ9KServiceInfoPayload *)reply->payload.inline_data;
    zz9k_put_be32(info->flags, fw.service_flags);
    return ZZ9K_STATUS_OK;
  }
  if (request->entry.opcode != ZZ9K_OP_AUDIO_VOICE_UPLOAD)
    return ZZ9K_STATUS_UNSUPPORTED;
  /* A raw voice request (ZZ9KCall) carries the client id of the calling
   * task's opener, and the firmware's voice_session_owned_by check wants the
   * id the session was opened under. */
  if (fake_refuses(fake_client_id())) {
    fw.refused++;
    return ZZ9K_STATUS_BAD_HANDLE;
  }
  upload = (const ZZ9KAudioVoiceUploadPayload *)request->entry.payload.inline_data;
  flags = zz9k_get_be32(upload->flags);
  bank_offset = zz9k_get_be32(upload->bank_offset);
  length = zz9k_get_be32(upload->length);
  sample_id = zz9k_get_be32(upload->sample_id);
  sample_length = zz9k_get_be32(upload->sample_length);
  format = zz9k_get_be32(upload->format);
  staging_bytes = fx.driver.voice ? fx.driver.voice->session.staging_bytes : 0U;
  if ((flags & ZZ9K_VOICE_UPLOAD_FLAG_COPY) != 0U) {
    if (length == 0U || length > staging_bytes ||
        bank_offset > FAKE_BANK_BYTES ||
        length > FAKE_BANK_BYTES - bank_offset)
      return ZZ9K_STATUS_BAD_REQUEST;
    memcpy(fw.bank + bank_offset, fw.staging, length);
    fw.uploads++;
  }
  if ((flags & ZZ9K_VOICE_UPLOAD_FLAG_DEFINE) != 0U) {
    if (sample_id >= ZZ9K_VOICE_MAX_SAMPLES || sample_length == 0U ||
        sample_length > FAKE_BANK_BYTES || !zz9k_voice_format_valid(format))
      return ZZ9K_STATUS_BAD_REQUEST;
    fw.defines++;
  }
  return ZZ9K_STATUS_OK;
}

/* ---- the fake voice session ---- */

static int zz_ax_voice_open(const ZZ9KVoiceOpenDesc *desc,
                            ZZ9KVoiceSession *session)
{
  uint32_t ring_bytes = desc->ring_bytes ? desc->ring_bytes
                                         : ZZ9K_VOICE_RING_DEFAULT_BYTES;
  uint32_t staging_bytes = desc->staging_bytes
                               ? desc->staging_bytes
                               : ZZ9K_VOICE_STAGING_DEFAULT_BYTES;

  fw.opens++;
  fw.open_flags = desc->flags;
  memset(session, 0, sizeof(*session));
  if (fw.open_status != ZZ9K_STATUS_OK)
    return fw.open_status;
  if (staging_bytes > sizeof(fw.staging))
    return ZZ9K_STATUS_NO_MEMORY;
  session->grant.session = 7U;
  session->grant.voices = desc->voices;
  /* The fake card has less memory than the driver asks for. */
  session->grant.bank_bytes = desc->bank_bytes < FAKE_BANK_BYTES
                                  ? desc->bank_bytes
                                  : FAKE_BANK_BYTES;
  session->grant.lead_frames = FAKE_LEAD;
  session->grant.lead_floor_frames = FAKE_LEAD;
  session->grant.lead_max_frames = 24000U;
  session->grant.ring_records = zz9k_voice_ring_records_for_bytes(ring_bytes);
  session->grant.output_rate = ZZ9K_VOICE_OUTPUT_RATE;
  session->grant.max_samples = ZZ9K_VOICE_MAX_SAMPLES;
  session->ring = fw.ring;
  session->status = fw.status_block;
  zz9k_put_be32(fw.status_block + ZZ9K_VOICE_STATUS_FEATURES_OFFSET,
                fw.features);
  session->staging = fw.staging;
  session->staging_bytes = staging_bytes;
  session->write_cursor = 0U;
  session->heartbeat = 1U;
  session->mapped = 1U;
  fw.read_cursor = 0U;
  /* The Voice API's request carries FAKE_VOICE_API_ID, whoever calls. */
  fw.owner_id = FAKE_VOICE_API_ID;
  fw.session_live = 1U;
  return ZZ9K_STATUS_OK;
}

static int zz_ax_voice_close(ZZ9KVoiceSession *session)
{
  if (fake_refuses(FAKE_VOICE_API_ID)) {
    fw.refused++;
    return ZZ9K_STATUS_BAD_HANDLE;
  }
  fw.closes++;
  fw.session_live = 0U;
  session->mapped = 0U;
  return ZZ9K_STATUS_OK;
}

static int zz_ax_voice_reset(ZZ9KVoiceSession *session)
{
  if (fake_refuses(FAKE_VOICE_API_ID)) {
    fw.refused++;
    return ZZ9K_STATUS_BAD_HANDLE;
  }
  fw.resets++;
  fw.read_cursor = session->write_cursor;
  if (fw.slot_live) {
    /* audio_voice_session_reset: the playing frame resumes at the staged
     * cursor and the engine renders silence up to the lead again. */
    fw.playing_frame += FAKE_RESET_JUMP;
    fw.horizon = fw.playing_frame + FAKE_LEAD;
  }
  return ZZ9K_STATUS_OK;
}

/* What the library does: stage into the ring, publish, return the count. */
static uint32_t zz_ax_voice_write(ZZ9KVoiceSession *session,
                                  const ZZ9KVoiceRecord *records,
                                  uint32_t count)
{
  uint32_t staged = zz9k_voice_ring_stage(session, fw.read_cursor, records,
                                          count);
  uint32_t i;

  if (fx.in_hook || fx.in_interrupt)
    fw.writes_outside_worker++;
  if (fw.write_budget >= 0) {
    if (staged > (uint32_t)fw.write_budget)
      staged = (uint32_t)fw.write_budget;
    fw.write_budget -= (LONG)staged;
  }
  if (staged == 0U)
    return 0U;
  /* A record stamped before the engine frame is late (audio_voice_render). */
  for (i = 0U; i < staged; i++) {
    if ((int32_t)(records[i].stamp - fw.horizon) < 0)
      fw.late_records++;
  }
  fw.slot_live = 1U;
  fw.writes++;
  zz9k_voice_ring_publish(session);
  return staged;
}

static int zz_ax_voice_get_status(ZZ9KVoiceSession *session,
                                  ZZ9KVoiceStatus *status)
{
  /* The library's GetStatus reads shared memory: no mailbox request. */
  if (!fw.state && fx.worker_live == 2U)
    return ZZ9K_STATUS_IO_ERROR;
  memset(status, 0, sizeof(*status));
  status->playing_frame = fw.playing_frame;
  status->horizon = fw.horizon;
  status->read_cursor = fw.read_cursor;
  status->state = fw.state;
  status->late = fw.late;
  status->session = 7U;
  session->heartbeat++;
  return ZZ9K_STATUS_OK;
}

/* ---- client hooks ---- */

static ULONG test_player(struct Hook *hook asm("a0"),
                          struct AHIAudioCtrlDrv *audio asm("a2"),
                          APTR message asm("a1"))
{
  (void)hook;
  (void)message;
  check(audio == &fx.audio, "PlayerFunc receives its audio control");
  fx.players++;
  if (fx.player_calls_pushed) {
    /* A tracker's PlayerFunc: new note, immediate. */
    fx.in_hook = 1U;
    intAHIsub_SetSound(1U, 0U, 0U, 0, audio, AHISF_IMM);
    intAHIsub_SetFreq(1U, 20000U, audio, AHISF_IMM);
    fx.in_hook = 0U;
  }
  return 0;
}

static ULONG test_sound(struct Hook *hook asm("a0"),
                         struct AHIAudioCtrlDrv *audio asm("a2"),
                         struct AHISoundMessage *message asm("a1"))
{
  (void)hook;
  fx.sounds++;
  check(message->ahism_Channel == 0U, "SoundFunc names its channel");
  /* The repeat of the note, queued for the pass after this one. */
  fx.in_hook = 1U;
  intAHIsub_SetSound(0U, 0U, 200U, 300, audio, 0U);
  fx.in_hook = 0U;
  return 0;
}

/* ---- helpers ---- */

static uint8_t sample[4096];

static void prepare(void)
{
  memset(&fw, 0, sizeof(fw));
  memset(&fx, 0, sizeof(fx));
  fw.library_present = 1U;
  fw.revision = ZZ9K_LIBRARY_MIN_REVISION_AUDIO_VOICE;
  fw.service_flags = FAKE_SERVICE_FLAGS;
  fw.write_budget = -1;
  fw.zorro = 3U;
  fw.state = ZZ9K_VOICE_STATE_LIVE;
  fw.horizon = FAKE_LEAD;
  fx.next_signal = 1;
  fx.player.h_Entry = (ULONG (*)())test_player;
  fx.sound.h_Entry = (ULONG (*)())test_sound;
  fx.audio.ahiac_PlayerFunc = &fx.player;
  fx.audio.ahiac_SoundFunc = &fx.sound;
  fx.audio.ahiac_PlayerFreq = 50UL << 16;
  fx.audio.ahiac_Channels = 4U;
  fx.audio.ahiac_Sounds = 8U;
  fx.audio.ahiac_MixFreq = 44100U;
  Z9AXBase = &fw.base;
  Z9AXBase->hw_addr = (uint32_t)fw.registers;
  Z9AXBase->zorro_version = fw.zorro;
  *(volatile uint16_t *)(void *)(fw.registers + ZZ_REG_AUDIO_CONFIG) = 1U;
  fx.process.pr_Task.tc_UserData = &fx.driver;
}

static void advance_card(uint32_t frames)
{
  fw.playing_frame += frames;
  fw.horizon = fw.playing_frame + FAKE_LEAD;
}

static uint32_t count_op(uint8_t op)
{
  const ZZ9KVoiceRecord *records;
  uint32_t count;
  uint32_t i;
  uint32_t n = 0U;

  records = vm_records(&fx.driver.voice->model, &count);
  for (i = 0U; i < count; i++)
    n += records[i].op == op;
  return n;
}

static const ZZ9KVoiceRecord *ring_record(uint32_t index)
{
  /* Records the fake ring holds, from the fake write cursor. */
  uint32_t slot = index & (fx.driver.voice->session.grant.ring_records - 1U);
  ZZ9KVoiceRecordWire *wire =
      (ZZ9KVoiceRecordWire *)(void *)(fw.ring + ZZ9K_VOICE_RING_HEADER_BYTES +
                                      slot * ZZ9K_VOICE_RECORD_BYTES);
  static ZZ9KVoiceRecord record;

  zz9k_voice_record_decode(&record, wire);
  return &record;
}

static uint32_t ring_records(void)
{
  return fx.driver.voice->session.write_cursor;
}

static uint32_t alloc_voice(void)
{
  return intAHIsub_AllocAudio(voice_tags, &fx.audio);
}

/* ---- cases ---- */

static int case_alloc_and_free(void)
{
  uint32_t rc;

  prepare();
  rc = alloc_voice();
  check(rc == 0U, "a hardware mode returns no AHISF flag");
  check(fw.opens == 1U, "the worker opened the voice session");
  check(fw.closes == 0U && fw.library_opens == 1U, "nothing closed yet");
  check(fw.defines == 4U, "the worker defines four whole-bank views");
  check(Z9AXBase->owner == &fx.driver, "the driver owns the card");
  check(Z9AXBase->allocating == NULL, "the reservation is released");
  check(fw.int_servers_added == 2U, "legacy node and fabric token installed");
  check(fx.driver.voice != NULL && fx.driver.voice->session_open,
        "the session is open");
  check(fx.audio.ahiac_MixFreq == 48000U, "the mode runs at the engine rate");
  check(fx.driver.voice->lead == FAKE_LEAD, "the granted lead is kept");
  check(fx.driver.voice->session.grant.ring_records == 1024U,
        "Zorro III asks for a 1024-record ring");
  check(fx.driver.voice->session.staging_bytes == 65536U,
        "Zorro III asks for a 64 KiB staging buffer");
  check(vm_bank_free_bytes(&fx.driver.voice->model) == FAKE_BANK_BYTES,
        "the model owns the granted bank");

  intAHIsub_FreeAudio(&fx.audio);
  check(fw.closes == 1U, "FreeAudio closes the session once");
  check(fw.library_closes == 1U, "FreeAudio closes zz9k.library once");
  check(Z9AXBase->owner == NULL, "FreeAudio releases the card");
  check(fw.int_servers_removed == 2U, "both ownership nodes removed");
  check(fx.audio.ahiac_DriverData == NULL, "the driver data is gone");
  check(strstr(fx.stats, "late=0 ") == fx.stats,
        "the stats variable starts with the engine counters");
  check(strstr(fx.stats, "ticks=") != NULL, "the stats variable names ticks");

  /* The interpolated mode opens with the linear flag: check through the
   * worker state the mode left behind. */
  prepare();
  rc = intAHIsub_AllocAudio(linear_tags, &fx.audio);
  check(rc == 0U && fx.driver.voice_mode == Z9AX_VOICE_LINEAR,
        "the interpolated mode is recognised");
  intAHIsub_FreeAudio(&fx.audio);
  return fx.failures;
}

static int refusal(const char *name, uint8_t zorro, uint8_t library,
                   uint16_t revision, uint32_t flags, int open_status,
                   uint32_t channels, uint8_t expect_reason)
{
  uint32_t rc;

  prepare();
  fw.zorro = zorro;
  Z9AXBase->zorro_version = zorro;
  fw.library_present = library;
  fw.revision = revision;
  fw.service_flags = flags;
  fw.open_status = open_status;
  fx.audio.ahiac_Channels = (UWORD)channels;
  rc = alloc_voice();
  check(rc == AHISF_ERROR, name);
  check(Z9AXBase->owner == NULL && Z9AXBase->allocating == NULL,
        "a refusal leaves the owner alone");
  check(ZZ9KBase == NULL, "a refusal leaves no library base open");
  check(fw.library_closes == (library ? fw.library_opens : 0U),
        "a refusal closes the library it opened");
  check(fw.closes == fw.opens || open_status != ZZ9K_STATUS_OK,
        "a refused allocation closes the session it opened");
  check(fw.int_servers_added == 0U, "a refusal installs no ownership node");
  check(fx.audio.ahiac_DriverData == NULL, "a refusal leaves no driver data");
  /* A refusal before anything is allocated records no reason. */
  check(fx.voice.reason == expect_reason,
        "the reason is recorded (%u, expected %u)", (unsigned)fx.voice.reason,
        (unsigned)expect_reason);
  return fx.failures;
}

static int case_refusals(void)
{
  refusal("Zorro II is refused", 2U, 1U,
          ZZ9K_LIBRARY_MIN_REVISION_AUDIO_VOICE, FAKE_SERVICE_FLAGS,
          ZZ9K_STATUS_OK, 4U, 0U);
  refusal("a missing zz9k.library is refused", 3U, 0U, 0U, 0U,
          ZZ9K_STATUS_OK, 4U, Z9AX_VOICE_REASON_LIBRARY);
  refusal("zz9k.library below revision 42 is refused", 3U, 1U, 41U, 0U,
          ZZ9K_STATUS_OK, 4U, Z9AX_VOICE_REASON_REVISION);
  refusal("firmware without the voice flag is refused", 3U, 1U,
          ZZ9K_LIBRARY_MIN_REVISION_AUDIO_VOICE,
          ZZ9K_SERVICE_FLAG_AUDIO_FABRIC_RATE, ZZ9K_STATUS_OK, 4U,
          Z9AX_VOICE_REASON_NO_ENGINE);
  refusal("a busy voice engine is refused", 3U, 1U,
          ZZ9K_LIBRARY_MIN_REVISION_AUDIO_VOICE, FAKE_SERVICE_FLAGS,
          ZZ9K_STATUS_BUSY, 4U, Z9AX_VOICE_REASON_OPEN);
  refusal("a card without bank memory is refused", 3U, 1U,
          ZZ9K_LIBRARY_MIN_REVISION_AUDIO_VOICE, FAKE_SERVICE_FLAGS,
          ZZ9K_STATUS_NO_MEMORY, 4U, Z9AX_VOICE_REASON_MEMORY);
  refusal("firmware without the voice engine is refused", 3U, 1U,
          ZZ9K_LIBRARY_MIN_REVISION_AUDIO_VOICE, FAKE_SERVICE_FLAGS,
          ZZ9K_STATUS_UNSUPPORTED, 4U, Z9AX_VOICE_REASON_OPEN);
  refusal("33 channels are refused", 3U, 1U,
          ZZ9K_LIBRARY_MIN_REVISION_AUDIO_VOICE, FAKE_SERVICE_FLAGS,
          ZZ9K_STATUS_OK, 33U, 0U);
  return fx.failures;
}

static int case_second_alloc(void)
{
  uint32_t rc;
  struct z9ax *first;

  prepare();
  check(alloc_voice() == 0U, "first allocation");
  first = Z9AXBase->owner;
  rc = intAHIsub_AllocAudio(linear_tags, &fx.audio);
  check(rc == AHISF_ERROR, "a second hardware-mode allocation fails");
  rc = intAHIsub_AllocAudio(soft_tags, &fx.audio);
  check(rc == AHISF_ERROR, "a software-mode allocation beside it fails");
  check(Z9AXBase->owner == first && Z9AXBase->allocating == NULL,
        "the active owner is untouched");
  check(fw.opens == 1U && fw.library_opens == 1U,
        "the second allocation opens nothing");
  check(fx.audio.ahiac_DriverData == &fx.driver,
        "the active driver data is untouched");
  intAHIsub_FreeAudio(&fx.audio);
  return fx.failures;
}

static int case_bank_halves(void)
{
  uint32_t rc;

  prepare();
  fw.open_status = ZZ9K_STATUS_NO_MEMORY;
  rc = alloc_voice();
  check(rc == AHISF_ERROR, "no memory at any size is a refusal");
  check(fw.opens == 10U, "the bank halves to its floor, then the buffers: %lu",
        (unsigned long)fw.opens);
  intAHIsub_FreeAudio(&fx.audio);

  /* The card has 4 MiB: the fifth open, at 1 MiB... no: the request halves
   * 16, 8, 4 and the card takes 4 MiB. */
  prepare();
  fw.open_status = 0;
  fx.bank_env = 16384U;
  check(alloc_voice() == 0U, "allocation with ENV:ZZ9K_AHI_BANK");
  check(fx.driver.voice->bank_request == 16U * 1024U * 1024U,
        "ENV:ZZ9K_AHI_BANK is read in KiB");
  intAHIsub_FreeAudio(&fx.audio);
  return fx.failures;
}

static int case_start_and_ticks(void)
{
  const ZZ9KVoiceRecord *record;
  uint32_t before;
  uint32_t i;
  uint32_t sounds_before;
  uint32_t heartbeat_before;

  struct AHISampleInfo *info = (struct AHISampleInfo *)sample;

  prepare();
  check(alloc_voice() == 0U, "allocation");
  info->ahisi_Type = AHIST_M8S;
  info->ahisi_Address = sample + 64U;
  info->ahisi_Length = 4000U;
  check(intAHIsub_LoadSound(0U, AHIST_SAMPLE, info, &fx.audio) == AHIE_OK,
        "LoadSound");
  check(intAHIsub_Start(AHISF_PLAY, &fx.audio) == AHIE_OK, "Start");
  check(fw.resets == 1U, "Start resets the engine timeline");
  check(fx.driver.voice->playing, "Start marks the session playing");
  record = ring_record(0U);
  check(record->op == ZZ9K_VOICE_OP_MASTER &&
            record->stamp == FAKE_LEAD + 960U,
        "the first record is the MASTER gain, one period clear of the horizon");
  /* The model runs up to two leads ahead of the sound, four ticks a pass. */
  check(fx.players == 4U, "Start runs the first four ticks ahead of the sound");

  /* Calls from an interrupt context and from the client's task. */
  fx.in_interrupt = 1U;
  intAHIsub_SetSound(0U, 0U, 0U, 0, &fx.audio, AHISF_IMM);
  intAHIsub_SetFreq(0U, 8000U, &fx.audio, AHISF_IMM);
  fx.in_interrupt = 0U;
  intAHIsub_SetVol(0U, 0x10000UL, 0x8000UL, &fx.audio, AHISF_IMM);
  check(vm_fifo_pending(&fx.driver.voice->model) == 3U,
        "the calls wait in the FIFO");
  check(ring_records() == 1U, "nothing reaches the ring before the worker");
  advance_card(960U);
  voice_service(&fx.driver);
  check(fx.players == 8U,
        "the model runs two leads ahead of the sound, four ticks per pass");
  check(fx.sounds == 1U, "SoundFunc runs at the first pass start");
  check(ring_records() == 5U, "SET, START, LOOP and the repeat of the hook");
  check(vm_fifo_pending(&fx.driver.voice->model) == 0U,
        "a call made inside SoundFunc is applied in the same pass");
  check(fw.writes_outside_worker == 0U,
        "the worker is the only caller of the ring write");

  /* 50 Hz: a tick per 960 frames; 25 Hz: one per 1920. */
  before = fx.players;
  for (i = 0U; i < 5U; i++) {
    advance_card(960U);
    voice_service(&fx.driver);
  }
  check(fx.players - before == 5U, "PlayerFunc runs 50 times a second");
  fx.audio.ahiac_PlayerFreq = 25UL << 16;
  before = fx.players;
  for (i = 0U; i < 5U; i++) {
    advance_card(960U);
    voice_service(&fx.driver);
  }
  check(fx.players - before == 2U || fx.players - before == 3U,
        "a changed PlayerFreq changes the tick rate");
  check(fx.driver.voice->model.ticks == fx.players, "each tick calls the player");

  /* AHIsub_Disable nests and is only a counter, so the worker keeps
   * running: it must take nothing from the FIFO and call no client hook,
   * while the status reads keep the session alive. */
  cintAHIsub_Disable(&fx.audio);
  cintAHIsub_Disable(&fx.audio);
  intAHIsub_SetFreq(0U, 10000U, &fx.audio, AHISF_IMM);
  before = fx.players;
  sounds_before = fx.sounds;
  heartbeat_before = fx.driver.voice->session.heartbeat;
  for (i = 0U; i < 3U; i++) {
    advance_card(960U);
    voice_service(&fx.driver);
  }
  check(vm_fifo_pending(&fx.driver.voice->model) == 1U,
        "the FIFO is not drained while Disable is nested");
  check(fx.players == before && fx.sounds == sounds_before,
        "no PlayerFunc or SoundFunc while Disable is held (%u, %u)",
        (unsigned)(fx.players - before),
        (unsigned)(fx.sounds - sounds_before));
  check(fx.driver.voice->session.heartbeat != heartbeat_before,
        "the heartbeat goes on while Disable is held");
  cintAHIsub_Enable(&fx.audio);
  advance_card(960U);
  voice_service(&fx.driver);
  check(fx.players == before, "one Enable of two keeps the hooks held");
  cintAHIsub_Enable(&fx.audio);
  advance_card(960U);
  voice_service(&fx.driver);
  check(vm_fifo_pending(&fx.driver.voice->model) == 0U,
        "Enable lets the worker drain the FIFO");
  check(fx.players > before, "Enable lets PlayerFunc run again");
  check(fw.writes_outside_worker == 0U,
        "still only the worker writes the ring");

  intAHIsub_Stop(AHISF_PLAY, &fx.audio);
  check(!fx.driver.voice->playing, "Stop ends playback");
  before = fx.players;
  advance_card(9600U);
  voice_service(&fx.driver);
  check(fx.players == before, "no PlayerFunc after Stop");
  before = ring_records();
  check(intAHIsub_Start(AHISF_PLAY, &fx.audio) == AHIE_OK, "Start again");
  check(ring_records() > before, "Start again restarts every channel");
  intAHIsub_FreeAudio(&fx.audio);
  check(strstr(fx.stats, "resyncs=0 ") != NULL, "no resync in a clean run");
  return fx.failures;
}

/* Reset moves the engine on: the firmware (audio_voice_session_reset) sets
 * the playing frame to the staged cursor and renders silence up to the lead
 * again, so the status the worker read before the reset is stale. Start and
 * a resync must anchor on a status read after it. */
static int case_restart_anchor(void)
{
  uint32_t i;

  prepare();
  check(alloc_voice() == 0U, "allocation");
  check(intAHIsub_Start(AHISF_PLAY, &fx.audio) == AHIE_OK, "Start");
  for (i = 0U; i < 3U; i++) {
    advance_card(960U);
    voice_service(&fx.driver);
  }
  check(fw.slot_live, "the first records took the slot live");
  check(fw.late_records == 0U, "a first Start has no late record (%lu)",
        (unsigned long)fw.late_records);

  /* Stop, then Start with the slot live: the engine jumps at both resets. */
  intAHIsub_Stop(AHISF_PLAY, &fx.audio);
  advance_card(960U);
  voice_service(&fx.driver);
  check(intAHIsub_Start(AHISF_PLAY, &fx.audio) == AHIE_OK, "Start again");
  check(fw.late_records == 0U,
        "Start after Stop anchors past the horizon the reset made (%lu late)",
        (unsigned long)fw.late_records);
  for (i = 0U; i < 3U; i++) {
    advance_card(960U);
    voice_service(&fx.driver);
  }
  check(fw.late_records == 0U, "and the records after it are on time (%lu)",
        (unsigned long)fw.late_records);
  intAHIsub_FreeAudio(&fx.audio);
  return fx.failures;
}

static int case_resync(void)
{
  prepare();
  check(alloc_voice() == 0U, "allocation");
  check(intAHIsub_Start(AHISF_PLAY, &fx.audio) == AHIE_OK, "Start");
  /* The machine stalls for half a second: the model is far behind. */
  advance_card(24000U);
  voice_service(&fx.driver);
  check(fx.driver.voice->resyncs == 1U, "a stalled worker re-anchors the model");
  check(fw.resets == 2U, "the resync drops the stale records");
  check(fw.late_records == 0U,
        "the re-anchored records are not late (%lu)",
        (unsigned long)fw.late_records);
  intAHIsub_FreeAudio(&fx.audio);
  check(strstr(fx.stats, "resyncs=1 ") != NULL, "the stats count the resync");
  return fx.failures;
}

static int case_ring_full(void)
{
  struct AHISampleInfo *info = (struct AHISampleInfo *)sample;

  prepare();
  check(alloc_voice() == 0U, "allocation");
  info->ahisi_Type = AHIST_M8S;
  info->ahisi_Address = sample + 64U;
  info->ahisi_Length = 4000U;
  check(intAHIsub_LoadSound(0U, AHIST_SAMPLE, info, &fx.audio) == AHIE_OK,
        "LoadSound");
  check(intAHIsub_Start(AHISF_PLAY, &fx.audio) == AHIE_OK, "Start");
  /* The ring takes two more records, then nothing until the next pass. */
  fw.write_budget = 2;
  intAHIsub_SetSound(0U, 0U, 0U, 0, &fx.audio, AHISF_IMM);
  intAHIsub_SetFreq(0U, 8000U, &fx.audio, AHISF_IMM);
  advance_card(960U);
  voice_service(&fx.driver);
  check(fx.driver.voice->ring_full >= 1U, "a short write is counted");
  check(count_op(ZZ9K_VOICE_OP_LOOP) >= 1U,
        "the unwritten tail stays for the next pass");
  fw.write_budget = -1;
  voice_service(&fx.driver);
  check(count_op(ZZ9K_VOICE_OP_LOOP) == 0U, "the tail went out");
  intAHIsub_FreeAudio(&fx.audio);
  check(strstr(fx.stats, "ringfull=") != NULL, "the stats name ring-full waits");
  return fx.failures;
}

static int case_session_lost(void)
{
  prepare();
  check(alloc_voice() == 0U, "allocation");
  check(intAHIsub_Start(AHISF_PLAY, &fx.audio) == AHIE_OK, "Start");
  fw.state = ZZ9K_VOICE_STATE_CLOSED;
  voice_service(&fx.driver);
  check(fx.driver.voice->session_lost, "a closed status marks the session lost");
  check(!fx.driver.voice->playing, "playback stops with the session");
  check(intAHIsub_Start(AHISF_PLAY, &fx.audio) == AHIE_UNKNOWN,
        "Start after the loss is refused");
  intAHIsub_FreeAudio(&fx.audio);
  check(fw.closes == 1U, "FreeAudio still closes the session once");
  return fx.failures;
}

static int case_load_unload(void)
{
  struct AHISampleInfo *info = (struct AHISampleInfo *)sample;
  uint32_t calls;
  uint32_t rc;
  uint32_t i;

  prepare();
  for (i = 0U; i < 3000U; i++)
    sample[i] = (uint8_t)i;
  check(alloc_voice() == 0U, "allocation");
  check(intAHIsub_Start(AHISF_PLAY, &fx.audio) == AHIE_OK, "Start");
  fx.driver.voice->session.staging_bytes = 1024U;
  info->ahisi_Type = AHIST_M8S;
  info->ahisi_Address = sample + 64U;
  info->ahisi_Length = 3000U;
  /* A bank that holds an older sound: the guards must be written. */
  memset(fw.bank, 0xAA, 4096U);
  calls = fw.calls;
  rc = intAHIsub_LoadSound(0U, AHIST_SAMPLE, info, &fx.audio);
  check(rc == AHIE_OK, "an 8-bit sample loads");
  check(fw.uploads == 3U && fw.calls - calls == 3U,
        "3000 bytes go in three 1024-byte chunks");
  check(fw.defines == 4U, "a sound defines no engine sample");
  check(memcmp(fw.bank + VM_GUARD_BYTES, sample + 64U, 3000U) == 0,
        "the bank holds the sample, a guard above the allocation start");
  check(memcmp(fw.bank, "\0\0\0\0\0\0\0\0", VM_GUARD_BYTES) == 0 &&
            memcmp(fw.bank + VM_GUARD_BYTES + 3000U,
                   "\0\0\0\0\0\0\0\0", VM_GUARD_BYTES) == 0,
        "both guards are zero though the bank held other bytes");
  check(fw.bank[2U * VM_GUARD_BYTES + 3000U] == 0xAAU,
        "nothing is written past the allocation");
  check(vm_bank_free_bytes(&fx.driver.voice->model) ==
            FAKE_BANK_BYTES - (3000U + 2U * VM_GUARD_BYTES),
        "the sound takes its allocation, guards included, from the bank");

  rc = intAHIsub_LoadSound(1U, AHIST_DYNAMICSAMPLE, info, &fx.audio);
  check(rc == AHIE_BADSOUNDTYPE, "AHIST_DYNAMICSAMPLE is refused");
  info->ahisi_Type = AHIST_S16S;
  info->ahisi_Length = 100U;
  rc = intAHIsub_LoadSound(1U, AHIST_SAMPLE, info, &fx.audio);
  check(rc == AHIE_OK, "a stereo sample loads with the stereo flag");
  info->ahisi_Type = 0x40U;
  rc = intAHIsub_LoadSound(2U, AHIST_SAMPLE, info, &fx.audio);
  check(rc == AHIE_BADSAMPLETYPE, "an unknown sample type is refused");

  /* UnloadSound never calls the mailbox; the worker frees the sound. */
  intAHIsub_SetSound(0U, 0U, 0U, 0, &fx.audio, AHISF_IMM);
  intAHIsub_SetFreq(0U, 8000U, &fx.audio, AHISF_IMM);
  advance_card(960U);
  voice_service(&fx.driver);
  calls = fw.calls;
  intAHIsub_UnloadSound(0U, &fx.audio);
  intAHIsub_UnloadSound(1U, &fx.audio);
  check(fw.calls == calls, "UnloadSound does not call the mailbox");
  check(vm_bank_free_bytes(&fx.driver.voice->model) ==
            FAKE_BANK_BYTES - (3000U + 400U + 4U * VM_GUARD_BYTES),
        "the bank waits for the worker");
  voice_service(&fx.driver);
  check(fw.calls == calls, "the worker frees sounds without the mailbox");
  /* Sound 1 was silent: free at once. Sound 0 is playing: its range waits
   * until the engine horizon passes the stop that silenced it. */
  check(vm_bank_free_bytes(&fx.driver.voice->model) ==
            FAKE_BANK_BYTES - (3000U + 400U + 4U * VM_GUARD_BYTES),
        "freed ranges wait for the engine horizon");
  voice_service(&fx.driver);
  check(vm_bank_free_bytes(&fx.driver.voice->model) ==
            FAKE_BANK_BYTES - (3000U + 400U + 4U * VM_GUARD_BYTES),
        "the ranges wait for the horizon");
  advance_card(FAKE_LEAD + 960U);
  voice_service(&fx.driver);
  check(vm_bank_free_bytes(&fx.driver.voice->model) == FAKE_BANK_BYTES,
        "the horizon passing the stop frees the range");
  check(fx.driver.voice->model.sounds[0].used == 0U, "sound 0 is gone");

  /* A sound whose size is not a multiple of 4: the rounding bytes and both
   * guards are zero, in a range that held other bytes. */
  memset(fw.bank, 0xAA, 4096U);
  info->ahisi_Type = AHIST_M8S;
  info->ahisi_Address = sample + 64U;
  info->ahisi_Length = 17U;
  rc = intAHIsub_LoadSound(3U, AHIST_SAMPLE, info, &fx.audio);
  check(rc == AHIE_OK, "a 17 byte sample loads");
  {
    uint32_t bank = fx.driver.voice->model.sounds[3].bank;
    uint32_t i;
    int zero = 1;

    check(memcmp(fw.bank + bank, sample + 64U, 17U) == 0,
          "the 17 bytes are in the bank");
    for (i = bank - VM_GUARD_BYTES; i < bank; i++)
      zero &= fw.bank[i] == 0U;
    for (i = bank + 17U; i < bank + 20U + VM_GUARD_BYTES; i++)
      zero &= fw.bank[i] == 0U;
    check(zero, "the guards and the 3 rounding bytes are zero");
    check(fw.bank[bank + 20U + VM_GUARD_BYTES] == 0xAAU,
          "nothing is written past the allocation");
  }
  intAHIsub_FreeAudio(&fx.audio);
  return fx.failures;
}

static int case_mid_load_failure(void)
{
  struct AHISampleInfo *info = (struct AHISampleInfo *)sample;
  uint32_t rc;

  prepare();
  check(alloc_voice() == 0U, "allocation");
  fx.driver.voice->session.staging_bytes = 1024U;
  fw.call_fail_after = 3;   /* the third upload call fails */
  info->ahisi_Type = AHIST_M8S;
  info->ahisi_Address = sample + 64U;
  info->ahisi_Length = 3000U;
  rc = intAHIsub_LoadSound(0U, AHIST_SAMPLE, info, &fx.audio);
  check(rc == AHIE_UNKNOWN, "a failed upload is an error");
  check(vm_bank_free_bytes(&fx.driver.voice->model) == FAKE_BANK_BYTES,
        "a failed upload gives its bank range back");
  check(fx.driver.voice->model.sounds[0].used == 0U, "nothing was defined");
  fw.calls = 0;
  intAHIsub_FreeAudio(&fx.audio);
  check(fw.closes == 1U && fw.library_closes == 1U,
        "FreeAudio closes the session and the library once");
  return fx.failures;
}

/* The mailbox copy of a LoadSound belongs to the worker, the task that
 * opened the session, whichever task AHI calls LoadSound from. */
static int case_load_other_task(void)
{
  struct AHISampleInfo *info = (struct AHISampleInfo *)sample;
  uint32_t heartbeat;
  uint32_t rc;
  uint32_t i;

  prepare();
  for (i = 0U; i < 3000U; i++)
    sample[i] = (uint8_t)(i * 7U);
  check(alloc_voice() == 0U, "allocation");
  check(fw.session_live && fw.owner_id == 0U,
        "the Voice API opens the session as client 0 (%u)",
        (unsigned)fw.owner_id);
  check(fake_client_id_of(&fx.process.pr_Task) == 0U,
        "the worker never opened the library, so its ZZ9KCall requests are"
        " client 0 too, like the Voice API's (%u)",
        (unsigned)fake_client_id_of(&fx.process.pr_Task));
  check(fake_client_id_of(&fx.main_task) != 0U,
        "the AllocAudio task has a client id of its own (%u)",
        (unsigned)fake_client_id_of(&fx.main_task));
  fx.driver.voice->session.staging_bytes = 1024U;
  info->ahisi_Type = AHIST_M8S;
  info->ahisi_Address = sample + 64U;
  info->ahisi_Length = 2900U;

  /* The client's own task, which opened zz9k.library as a client does,
   * loads a sound while the card plays. */
  fx.current = &fx.other_task;
  test_open_library((CONST_STRPTR)ZZ9K_LIBRARY_NAME, 0);
  fx.current = NULL;
  check(fake_client_id_of(&fx.other_task) != 0U,
        "the client's other task has a client id of its own");
  check(intAHIsub_Start(AHISF_PLAY, &fx.audio) == AHIE_OK, "Start");
  fx.current = &fx.other_task;
  heartbeat = fx.driver.voice->session.heartbeat;
  rc = intAHIsub_LoadSound(0U, AHIST_SAMPLE, info, &fx.audio);
  fx.current = NULL;
  check(rc == AHIE_OK, "LoadSound from a task that is not the worker (%lu)",
        (unsigned long)rc);
  check(fw.refused == 0U, "the card refused %lu voice requests from a task"
        " that does not own the session", (unsigned long)fw.refused);
  check(fw.uploads == 3U, "2900 bytes go in three chunks (%lu)",
        (unsigned long)fw.uploads);
  check(memcmp(fw.bank + VM_GUARD_BYTES, sample + 64U, 2900U) == 0,
        "the bank holds the sample");
  check(fx.driver.voice->session.heartbeat - heartbeat >= 3U,
        "the worker keeps the heartbeat during the upload (%lu)",
        (unsigned long)(fx.driver.voice->session.heartbeat - heartbeat));
  check(fx.driver.voice->model.sounds[0].used == VM_SOUND_LOADED,
        "the sound is defined");

  /* Replacing it from the AllocAudio task, then a stereo sound from the
   * other task, with the first still in the bank. */
  info->ahisi_Length = 100U;
  rc = intAHIsub_LoadSound(0U, AHIST_SAMPLE, info, &fx.audio);
  check(rc == AHIE_OK, "LoadSound from the AllocAudio task");
  info->ahisi_Type = AHIST_S16S;
  fx.current = &fx.other_task;
  rc = intAHIsub_LoadSound(1U, AHIST_SAMPLE, info, &fx.audio);
  fx.current = NULL;
  check(rc == AHIE_OK, "a stereo LoadSound from a second task");
  check(fw.refused == 0U, "still no refused voice request");
  /* A client hook on the worker that loads a sound is the owner itself: it
   * copies without waiting on itself. */
  fx.current = &fx.process.pr_Task;
  rc = intAHIsub_LoadSound(2U, AHIST_SAMPLE, info, &fx.audio);
  fx.current = NULL;
  check(rc == AHIE_OK, "LoadSound called on the worker task");
  check(fw.refused == 0U, "the worker's own load is not refused");
  fx.current = &fx.other_task;
  intAHIsub_UnloadSound(1U, &fx.audio);
  intAHIsub_Stop(AHISF_PLAY, &fx.audio);
  fx.current = &fx.other_task;
  test_close_library(&fw.library);
  fx.current = NULL;
  intAHIsub_FreeAudio(&fx.audio);
  check(fw.refused == 0U, "the whole run made no refused voice request");
  check(fw.closes == 1U, "FreeAudio closes the session once");
  return fx.failures;
}

/* A load that cannot reach a live session ends instead of waiting. */
static int case_load_after_loss(void)
{
  struct AHISampleInfo *info = (struct AHISampleInfo *)sample;
  uint32_t rc;

  prepare();
  check(alloc_voice() == 0U, "allocation");
  fw.state = ZZ9K_VOICE_STATE_CLOSED;
  voice_service(&fx.driver);
  check(fx.driver.voice->session_lost, "the card closed the session");
  info->ahisi_Type = AHIST_M8S;
  info->ahisi_Address = sample + 64U;
  info->ahisi_Length = 100U;
  rc = intAHIsub_LoadSound(0U, AHIST_SAMPLE, info, &fx.audio);
  check(rc == AHIE_UNKNOWN, "LoadSound after the loss is an error");
  check(vm_bank_free_bytes(&fx.driver.voice->model) == FAKE_BANK_BYTES,
        "its bank range is given back");
  intAHIsub_FreeAudio(&fx.audio);
  return fx.failures;
}

static int case_getattr(void)
{
  check(intAHIsub_GetAttr(AHIDB_MaxChannels, 0, 0, voice_tags, &fx.audio) ==
            32,
        "a hardware mode offers 32 channels");
  check(intAHIsub_GetAttr(AHIDB_MaxChannels, 0, 0, soft_tags, &fx.audio) == 8,
        "a software mode still offers 8 channels");
  check(intAHIsub_GetAttr(AHIDB_Frequencies, 0, 0, voice_tags, &fx.audio) == 1,
        "a hardware mode has one rate");
  check(intAHIsub_GetAttr(AHIDB_Frequency, 0, 0, voice_tags, &fx.audio) ==
            48000,
        "the engine rate");
  check(intAHIsub_GetAttr(AHIDB_Frequency, 1, 77, voice_tags, &fx.audio) ==
            77,
        "no second rate");
  check(intAHIsub_GetAttr(AHIDB_Index, 48000, 0, voice_tags, &fx.audio) == 0,
        "the engine rate is index 0");
  check(intAHIsub_GetAttr(AHIDB_Record, 0, 0, voice_tags, &fx.audio) == FALSE,
        "a hardware mode does not record");
  return fx.failures;
}

static int case_effects(void)
{
  struct AHIEffMasterVolume master;
  struct AHIEffChannelInfo *info;
  uint32_t offsets[4];

  prepare();
  check(alloc_voice() == 0U, "allocation");
  master.ahie_Effect = AHIET_DSPECHO;
  check(intAHIsub_SetEffect((uint8_t *)&master, &fx.audio) == AHIE_UNKNOWN,
        "an unsupported effect is AHIE_UNKNOWN");
  master.ahie_Effect = AHIET_MASTERVOLUME;
  master.ahiemv_Volume = 0x10000L;
  check(intAHIsub_SetEffect((uint8_t *)&master, &fx.audio) == AHIE_OK,
        "AHIET_MASTERVOLUME is accepted");
  check(vm_fifo_pending(&fx.driver.voice->model) == 1U,
        "the master volume goes through the FIFO");
  info = (struct AHIEffChannelInfo *)offsets;
  info->ahie_Effect = AHIET_CHANNELINFO;
  info->ahieci_Func = &fx.player;
  info->ahieci_Channels = 4U;
  check(intAHIsub_SetEffect((uint8_t *)info, &fx.audio) == AHIE_OK,
        "AHIET_CHANNELINFO is accepted");
  check(fx.driver.voice->channelinfo == info, "the channel info hook is kept");
  master.ahie_Effect = AHIET_MASTERVOLUME | AHIET_CANCEL;
  check(intAHIsub_SetEffect((uint8_t *)&master, &fx.audio) == AHIE_OK,
        "a cancel is accepted");
  intAHIsub_FreeAudio(&fx.audio);
  return fx.failures;
}

static int case_mhi(void)
{
  /* A legacy MHI owner keeps the classic exclusion; a fabric-qualified MHI
   * may play beside a voice session. */
  prepare();
  fw.mhi_mode = 1U;
  check(alloc_voice() == AHISF_ERROR, "legacy MHI beside a voice session");
  check(Z9AXBase->owner == NULL && fx.audio.ahiac_DriverData == NULL,
        "the refusal releases everything");
  check(fw.closes == fw.opens && fw.opens == 1U,
        "the session the worker opened is closed again");
  check(ZZ9KBase == NULL && fw.library_closes == 1U,
        "the library is closed again");

  prepare();
  fw.mhi_mode = 2U;
  check(alloc_voice() == 0U, "fabric MHI beside a voice session");
  check(fw.int_servers_added == 2U, "the fabric token is installed");
  intAHIsub_FreeAudio(&fx.audio);
  return fx.failures;
}

/* The records of ops `op` the worker wrote to the fake ring so far. */
static uint32_t ring_ops(uint8_t op, const ZZ9KVoiceRecord **first)
{
  static ZZ9KVoiceRecord found;
  uint32_t n = 0U;
  uint32_t i;

  for (i = 0U; i < ring_records(); i++) {
    const ZZ9KVoiceRecord *r = ring_record(i);

    if (r->op == op) {
      if (n == 0U)
        found = *r;
      n++;
    }
  }
  if (first)
    *first = &found;
  return n;
}

/* Starts a session with the given features and AntiClickSamples and returns
 * the number of CONFIG records the Start wrote; `config` is the first. */
static uint32_t start_with(uint32_t features, uint32_t anticlick,
                           const struct TagItem *tags,
                           const ZZ9KVoiceRecord **config)
{
  uint32_t n;

  prepare();
  fw.features = features;
  fx.audio.ahiac_AntiClickSamples = anticlick;
  if (intAHIsub_AllocAudio((struct TagItem *)tags, &fx.audio) != 0U) {
    check(0, "allocation with features %lx", (unsigned long)features);
    return 0U;
  }
  check(intAHIsub_Start(AHISF_PLAY, &fx.audio) == AHIE_OK, "Start");
  n = ring_ops(ZZ9K_VOICE_OP_CONFIG, config);
  return n;
}

static void end_session(void)
{
  intAHIsub_FreeAudio(&fx.audio);
}

static int case_quality_config(void)
{
  const ZZ9KVoiceRecord *c;
  uint32_t n;
  const uint32_t base = ZZ9K_VOICE_FEATURE_CONFIG;

  /* Firmware without features: no CONFIG record and the old stream. */
  n = start_with(0U, 240U, voice_tags, &c);
  check(n == 0U, "no features: no CONFIG record (%lu)", (unsigned long)n);
  check(ring_record(0U)->op == ZZ9K_VOICE_OP_MASTER, "MASTER stays first");
  check(fx.driver.voice->features == 0U && fx.driver.voice->ramp_frames == 0U,
        "the driver kept no ramp");
  end_session();

  /* The ramp is AntiClickSamples, 48 kHz frames, once RAMP is advertised. */
  n = start_with(base | ZZ9K_VOICE_FEATURE_RAMP, 240U, voice_tags, &c);
  check(n == 1U && c->flags == ZZ9K_VOICE_CONFIG_RAMP && c->arg0 == 240U,
        "a ramp of AntiClickSamples (%lu records)", (unsigned long)n);
  check(ring_record(0U)->op == ZZ9K_VOICE_OP_MASTER &&
            ring_record(1U)->op == ZZ9K_VOICE_OP_CONFIG,
        "CONFIG follows the MASTER record");
  end_session();

  /* Zero is off. */
  n = start_with(base | ZZ9K_VOICE_FEATURE_RAMP, 0U, voice_tags, &c);
  check(n == 0U, "AntiClickSamples 0: no ramp record (%lu)", (unsigned long)n);
  end_session();

  /* More than the engine takes is cut to its maximum. */
  n = start_with(base | ZZ9K_VOICE_FEATURE_RAMP, 100000U, voice_tags, &c);
  check(n == 1U && c->arg0 == ZZ9K_VOICE_RAMP_MAX_FRAMES,
        "the ramp is cut to %u frames", (unsigned)ZZ9K_VOICE_RAMP_MAX_FRAMES);
  end_session();

  /* The limiter goes with the ramp in one record, or alone with arg0 0. */
  n = start_with(base | ZZ9K_VOICE_FEATURE_RAMP | ZZ9K_VOICE_FEATURE_LIMITER,
                 64U, voice_tags, &c);
  check(n == 1U &&
            c->flags == (ZZ9K_VOICE_CONFIG_RAMP | ZZ9K_VOICE_CONFIG_LIMITER) &&
            c->arg0 == 64U && c->arg1 == 1U,
        "ramp and limiter in one record");
  end_session();
  n = start_with(base | ZZ9K_VOICE_FEATURE_LIMITER, 64U, voice_tags, &c);
  check(n == 1U && c->flags == ZZ9K_VOICE_CONFIG_LIMITER && c->arg0 == 0U &&
            c->arg1 == 1U,
        "limiter without RAMP advertised: no ramp, arg0 0");
  end_session();
  n = start_with(base | ZZ9K_VOICE_FEATURE_RAMP | ZZ9K_VOICE_FEATURE_LIMITER,
                 0U, voice_tags, &c);
  check(n == 1U && c->flags == ZZ9K_VOICE_CONFIG_LIMITER && c->arg0 == 0U,
        "zero ramp with the limiter: the limiter alone");
  end_session();

  /* Bits without the CONFIG record itself send nothing. */
  n = start_with(ZZ9K_VOICE_FEATURE_RAMP | ZZ9K_VOICE_FEATURE_LIMITER, 240U,
                 voice_tags, &c);
  check(n == 0U, "RAMP and LIMITER without CONFIG: nothing is sent");
  end_session();

  /* The nearest and linear modes never send a default interpolation. */
  n = start_with(0xFFU, 240U, linear_tags, &c);
  check(ring_ops(ZZ9K_VOICE_OP_CONFIG, &c) == 1U &&
            (c->flags & ZZ9K_VOICE_CONFIG_INTERP) == 0U,
        "the linear mode sends no interpolation record");
  check((fw.open_flags & ZZ9K_VOICE_OPEN_FLAG_LINEAR) != 0U,
        "the linear mode opens with the linear flag");
  end_session();

  /* A restart of the timeline sends it again. */
  n = start_with(base | ZZ9K_VOICE_FEATURE_RAMP, 128U, voice_tags, &c);
  check(n == 1U, "first Start");
  intAHIsub_Stop(AHISF_PLAY, &fx.audio);
  advance_card(960U);
  voice_service(&fx.driver);
  check(intAHIsub_Start(AHISF_PLAY, &fx.audio) == AHIE_OK, "Start again");
  check(ring_ops(ZZ9K_VOICE_OP_CONFIG, &c) == 2U,
        "the second Start sends the ramp again");
  end_session();
  check(strstr(fx.stats, "ramp=128 ") != NULL,
        "the stats name the ramp: %s", fx.stats);
  return fx.failures;
}

static int case_cubic_mode(void)
{
  const ZZ9KVoiceRecord *c;
  uint32_t n;
  uint32_t i;
  const uint32_t need = ZZ9K_VOICE_FEATURE_CONFIG |
                        ZZ9K_VOICE_FEATURE_VOICE_INTERP |
                        ZZ9K_VOICE_FEATURE_CUBIC;
  const uint32_t missing[] = {0U,
                              ZZ9K_VOICE_FEATURE_CONFIG,
                              need & ~ZZ9K_VOICE_FEATURE_CONFIG,
                              need & ~ZZ9K_VOICE_FEATURE_VOICE_INTERP,
                              need & ~ZZ9K_VOICE_FEATURE_CUBIC,
                              ZZ9K_VOICE_FEATURE_RAMP |
                                  ZZ9K_VOICE_FEATURE_LIMITER};

  /* Firmware that lacks any piece refuses the mode: no other interpolation
   * is played in its place. */
  for (i = 0U; i < sizeof(missing) / sizeof(missing[0]); i++) {
    prepare();
    fw.features = missing[i];
    check(intAHIsub_AllocAudio(cubic_tags, &fx.audio) == AHISF_ERROR,
          "features %lx: the cubic mode is refused",
          (unsigned long)missing[i]);
    check(fx.voice.reason == Z9AX_VOICE_REASON_FEATURES,
          "features %lx: the reason is recorded (%u)",
          (unsigned long)missing[i], (unsigned)fx.voice.reason);
    check(Z9AXBase->owner == NULL && Z9AXBase->allocating == NULL &&
              ZZ9KBase == NULL && fx.audio.ahiac_DriverData == NULL,
          "features %lx: a refusal leaves nothing behind",
          (unsigned long)missing[i]);
    check(fw.closes == fw.opens && fw.opens == 1U,
          "features %lx: the session the worker opened is closed",
          (unsigned long)missing[i]);
  }

  /* The pieces together: the mode is recognised, opens without the linear
   * flag and sends the cubic default after the master record. */
  n = start_with(need, 0U, cubic_tags, &c);
  check(n == 1U && c->flags == ZZ9K_VOICE_CONFIG_INTERP &&
            c->arg0 == ZZ9K_VOICE_INTERP_CUBIC && c->arg1 == 0U,
        "the cubic default interpolation record (%lu)", (unsigned long)n);
  check(fx.driver.voice_mode == Z9AX_VOICE_CUBIC && fx.driver.voice->cubic &&
            !fx.driver.voice->linear,
        "the mode is recognised");
  check((fw.open_flags & ZZ9K_VOICE_OPEN_FLAG_LINEAR) == 0U,
        "the cubic mode does not open with the linear flag");
  check(ring_record(0U)->op == ZZ9K_VOICE_OP_MASTER &&
            ring_record(1U)->op == ZZ9K_VOICE_OP_CONFIG,
        "the record follows the MASTER record");
  end_session();

  /* With ramp and limiter advertised: their record, then the interpolation. */
  n = start_with(need | ZZ9K_VOICE_FEATURE_RAMP | ZZ9K_VOICE_FEATURE_LIMITER,
                 96U, cubic_tags, &c);
  check(n == 2U && ring_record(1U)->op == ZZ9K_VOICE_OP_CONFIG &&
            ring_record(1U)->flags ==
                (ZZ9K_VOICE_CONFIG_RAMP | ZZ9K_VOICE_CONFIG_LIMITER) &&
            ring_record(1U)->arg0 == 96U && ring_record(2U)->flags ==
                                                ZZ9K_VOICE_CONFIG_INTERP,
        "ramp and limiter first, then the interpolation (%lu records)",
        (unsigned long)n);
  end_session();

  /* A plain mode on the same firmware keeps its interpolation. */
  prepare();
  fw.features = need;
  check(intAHIsub_AllocAudio(voice_tags, &fx.audio) == 0U,
        "the nearest mode on cubic firmware");
  check(!fx.driver.voice->cubic, "it does not turn cubic");
  end_session();
  return fx.failures;
}

int main(void)
{
  int failures = 0;

  /* Unbuffered: a crash must not swallow what the cases printed. */
  setvbuf(stdout, NULL, _IONBF, 0);
  SysBase = *(struct ExecBase **)4;
  /* The driver's hooks are called directly, so no utility.library is
   * needed; OpenLibrary and CloseLibrary below are the fake ones. */
#define RUN(case_name)                       \
  do {                                       \
    puts("- " #case_name);                   \
    (void)case_name();                         \
  } while (0)
  RUN(case_alloc_and_free);
  RUN(case_quality_config);
  RUN(case_cubic_mode);
  RUN(case_refusals);
  RUN(case_second_alloc);
  RUN(case_bank_halves);
  RUN(case_start_and_ticks);
  RUN(case_resync);
  RUN(case_restart_anchor);
  RUN(case_ring_full);
  RUN(case_session_lost);
  RUN(case_load_unload);
  RUN(case_mid_load_failure);
  RUN(case_load_other_task);
  RUN(case_load_after_loss);
  RUN(case_effects);
  RUN(case_getattr);
  RUN(case_mhi);
  failures = failures_total;
  if (failures) {
    printf("%d failure(s)\n", failures);
    return 20;
  }
  puts("PASS: voice worker (18 cases)");
  return 0;
}
