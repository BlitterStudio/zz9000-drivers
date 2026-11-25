/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Target-side regression for the driver's library lifecycle: compile the real
 * driver, run its Open, Close and Expunge vectors and init() the way exec and
 * ramlib do, and check what a card-less AllocAudio sees afterwards.
 *
 * zz9000ax.audio is an exec library that ahi.device opens with OpenLibrary().
 * The vectors must therefore follow library rules: Open returns the base (and
 * never touches A1, which holds no IORequest), Expunge defers while the
 * library is open, and a removal closes the bases init() opened and returns
 * the seglist, so that the next OpenLibrary loads the file and runs init()
 * again. An Expunge that closed the bases but left the library in the list
 * (avail flush, a failed allocation) made the next AllocAudio call
 * CreateNewProc with a NULL DOSBase.
 *
 * The harness models exec with a private library list: LoadSeg and
 * MakeLibrary become load_driver(), OpenLibrary becomes exec_open(), and
 * RemLibrary becomes exec_expunge(), which hands the returned seglist to
 * "UnLoadSeg" (a counter). Only the library vectors, init() and AllocAudio
 * are the driver's; the card is a zeroed block of memory.
 *
 * Run on AmigaOS or with vamos --cpu=68020 -m 8000 --stack-size=64. In the
 * amigadev/crosstools:m68k-amigaos image, from ahi/driver after build.sh
 * staged zz9k-headers:
 *   m68k-amigaos-gcc library_lifecycle_test.c voice_model.c asmfuncs.s -O1 \
 *       -m68020-60 -I../../include -Izz9k-headers \
 *       -Wl,--allow-multiple-definition -o library_lifecycle_test -ldebug
 *   vamos --cpu=68020 -m 8000 --stack-size=64 library_lifecycle_test
 * (--allow-multiple-definition lets the driver's SysBase coexist with the
 * startup file's.) `make ahi-lifecycle-test` (tools/ahi-lifecycle-test.sh)
 * stages the headers and runs both commands in the container.
 */
#include <stdio.h>
#include <string.h>
#include <exec/exec.h>
#include <dos/dosextens.h>
#include <dos/dostags.h>
#include <libraries/configvars.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/expansion.h>
#include <proto/utility.h>
#include <clib/debug_protos.h>

static void real_free_mem(APTR memory, ULONG size)
{
  FreeMem(memory, size);
}

static struct Node *test_find_name(struct List *list, CONST_STRPTR name);
static struct Task *test_find_task(CONST_STRPTR name);
static BYTE test_alloc_signal(LONG number);
static struct Library *test_open_library(CONST_STRPTR name, ULONG version);
static void test_close_library(struct Library *library);
static struct ConfigDev *test_find_config_dev(struct ConfigDev *after,
                                              LONG manufacturer, LONG product);
static struct Process *test_create_process(void);
static void test_add_int_server(ULONG int_number, struct Interrupt *interrupt);
static void test_rem_int_server(ULONG int_number, struct Interrupt *interrupt);
static void test_free_mem(APTR memory, ULONG size);
static BPTR test_dos_open(CONST_STRPTR name, LONG mode);

#undef FindName
#undef FindTask
#undef AllocSignal
#undef FreeSignal
#undef CreateNewProcTags
#undef OpenLibrary
#undef CloseLibrary
#undef FindConfigDev
#undef AddIntServer
#undef RemIntServer
#undef FreeMem
#undef Open
#undef Close
#undef KPrintF
#define FindName(list, name) test_find_name(list, name)
#define FindTask(name) test_find_task(name)
#define AllocSignal(number) test_alloc_signal(number)
#define FreeSignal(number) ((void)(number))
#define CreateNewProcTags(...) test_create_process()
#define OpenLibrary(name, version) test_open_library(name, version)
#define CloseLibrary(library) test_close_library(library)
#define FindConfigDev(after, manufacturer, product) \
  test_find_config_dev(after, manufacturer, product)
#define AddIntServer(number, interrupt) test_add_int_server(number, interrupt)
#define RemIntServer(number, interrupt) test_rem_int_server(number, interrupt)
#define FreeMem(memory, size) test_free_mem(memory, size)
#define Open(name, mode) test_dos_open(name, mode)
#define Close(file) ((void)(file))
#define KPrintF(...) ((void)0)
/* The resident owns DOSBase, unlike an executable's libc startup. */
#define DOSBase test_driver_dos_base
#define _start lifecycle_test_driver_start
#include "zz9000ax-ahi.c"
#undef _start

/* The version ahi.device asks for (DriverVersion in AHI's header.c). */
#define AHI_DRIVER_VERSION 2UL
#define TEST_SEGLIST ((BPTR)0x1234)
#define CARD_SIZE 0x20000UL
#define TEST_CYCLES 3U

static struct {
  struct Library dos, utility, expansion;
  struct Task task;
  struct Library *open_result;
  struct ConfigDev config_dev;
  struct List libraries;
  UBYTE card[0x10000 + ZZ_AX_AUDIO_BUFSZ];
  ULONG proc_creates;
  struct DosLibrary *dos_at_proc_create;
  ULONG int_servers;
  ULONG unloaded_seglists;
  ULONG freed_blocks;
  APTR freed_memory;
  ULONG freed_size;
  ULONG bad_closes;
  int failures;
} fx;

static void check(int condition, const char *message)
{
  if (!condition) {
    printf("FAIL: %s\n", message);
    fx.failures++;
  }
}

static struct Node *test_find_name(struct List *list, CONST_STRPTR name)
{
  (void)list;
  (void)name;
  return NULL; /* no MHI owner */
}

static struct Task *test_find_task(CONST_STRPTR name)
{
  (void)name;
  return &fx.task;
}

static BYTE test_alloc_signal(LONG number)
{
  (void)number;
  return 5;
}

static struct Library *test_open_library(CONST_STRPTR name, ULONG version)
{
  struct Library *library = NULL;

  (void)version;
  if (!strcmp((const char *)name, "dos.library"))
    library = &fx.dos;
  else if (!strcmp((const char *)name, "utility.library"))
    library = &fx.utility;
  else if (!strcmp((const char *)name, "expansion.library"))
    library = &fx.expansion;
  /* zz9k.library is absent: AllocAudio takes the control-plane-less path. */
  if (library)
    library->lib_OpenCnt++;
  return library;
}

static void test_close_library(struct Library *library)
{
  if (library != &fx.dos && library != &fx.utility &&
      library != &fx.expansion) {
    fx.bad_closes++;
    return;
  }
  if (library->lib_OpenCnt == 0U)
    fx.bad_closes++;
  else
    library->lib_OpenCnt--;
}

static struct ConfigDev *test_find_config_dev(struct ConfigDev *after,
                                              LONG manufacturer, LONG product)
{
  if (after || manufacturer != ZZ9000_MNT_MANUFACTURER ||
      product != ZZ9000_PRODUCT_Z3)
    return NULL;
  fx.config_dev.cd_BoardAddr = fx.card;
  fx.config_dev.cd_BoardSize = CARD_SIZE;
  return &fx.config_dev;
}

/* AllocAudio's worker creation: what the driver's DOSBase is right now. */
static struct Process *test_create_process(void)
{
  fx.proc_creates++;
  fx.dos_at_proc_create = DOSBase;
  return NULL;
}

static void test_add_int_server(ULONG int_number, struct Interrupt *interrupt)
{
  (void)int_number;
  (void)interrupt;
  fx.int_servers++;
}

static void test_rem_int_server(ULONG int_number, struct Interrupt *interrupt)
{
  (void)int_number;
  (void)interrupt;
  fx.int_servers--;
}

static void test_free_mem(APTR memory, ULONG size)
{
  fx.freed_blocks++;
  fx.freed_memory = memory;
  fx.freed_size = size;
  real_free_mem(memory, size);
}

static BPTR test_dos_open(CONST_STRPTR name, LONG mode)
{
  (void)name;
  (void)mode;
  return 0; /* no ENV: variables */
}

/* The A1 argument of the Open vector is garbage for a library (it holds
 * the caller's name pointer at best); the A1 argument of Close is not
 * defined at all. A driver must not write through either. */
typedef struct Library *(*OpenVector)(struct Library *library asm("a6"),
                                      APTR a1 asm("a1"),
                                      ULONG version asm("d0"),
                                      ULONG flags asm("d1"));
typedef BPTR (*CloseVector)(struct Library *library asm("a6"),
                            APTR a1 asm("a1"));
typedef BPTR (*ExpungeVector)(struct Library *library asm("a6"));

#define VECTOR_OPEN ((OpenVector)function_table[0])
#define VECTOR_CLOSE ((CloseVector)function_table[1])
#define VECTOR_EXPUNGE ((ExpungeVector)function_table[2])

static void reset(void)
{
  struct List *list = &fx.libraries;
  int failures = fx.failures;

  memset(&fx, 0, sizeof(fx));
  fx.failures = failures;
  list->lh_Head = (struct Node *)&list->lh_Tail;
  list->lh_Tail = NULL;
  list->lh_TailPred = (struct Node *)&list->lh_Head;
  fx.task.tc_Node.ln_Type = NT_TASK;
}

static struct Library *listed_driver(void)
{
  struct Node *node = fx.libraries.lh_Head;

  return node->ln_Succ ? (struct Library *)node : NULL;
}

/* ramlib: LoadSeg, MakeLibrary, InitResident (init, then AddLibrary). */
static struct Library *load_driver(BPTR seglist)
{
  ULONG vectors = 0, negsize, possize;
  UBYTE *memory;
  struct Library *library;

  while (function_table[vectors] != (uint32_t)-1)
    vectors++;
  negsize = (vectors * 6U + 3U) & ~3UL;
  possize = auto_init_tables[0];
  memory = AllocMem(negsize + possize, MEMF_PUBLIC | MEMF_CLEAR);
  if (!memory)
    return NULL;
  library = (struct Library *)(memory + negsize);
  library->lib_Node.ln_Type = NT_LIBRARY;
  library->lib_Node.ln_Name = (char *)device_name;
  library->lib_Flags = LIBF_SUMUSED | LIBF_CHANGED;
  library->lib_NegSize = (UWORD)negsize;
  library->lib_PosSize = (UWORD)possize;
  library->lib_Version = DEVICE_VERSION;
  library->lib_Revision = DEVICE_REVISION;
  if (!init(seglist, library)) {
    real_free_mem(memory, negsize + possize);
    return NULL;
  }
  AddTail(&fx.libraries, &library->lib_Node);
  return library;
}

/* The test's own cleanup: drop a library that is still loaded. */
static void discard_driver(void)
{
  struct Library *library = listed_driver();

  if (!library)
    return;
  Remove(&library->lib_Node);
  real_free_mem((UBYTE *)library - library->lib_NegSize,
                (ULONG)library->lib_NegSize + library->lib_PosSize);
}

/* OpenLibrary: the library list under Forbid, or a load from disk. The
 * name buffer is what the caller's A1 points at; the test fills it with
 * a pattern so that a stray write shows. */
static struct Library *exec_open(BPTR seglist, UBYTE *name)
{
  struct Library *library = listed_driver();

  if (!library)
    library = load_driver(seglist);
  if (!library)
    return NULL;
  Forbid();
  fx.open_result = VECTOR_OPEN(library, name, AHI_DRIVER_VERSION, 0);
  Permit();
  /* The case that checks the return value reads fx.open_result; the others
   * go on with the listed base, whatever the vector returned. */
  return library;
}

static void unload_if(BPTR seglist)
{
  if (seglist)
    fx.unloaded_seglists++;
}

static void exec_close(struct Library *library)
{
  BPTR seglist;

  Forbid();
  seglist = VECTOR_CLOSE(library, NULL);
  Permit();
  unload_if(seglist);
}

/* RemLibrary, as avail flush and a failed allocation call it. */
static BPTR exec_expunge(struct Library *library)
{
  BPTR seglist;

  Forbid();
  seglist = VECTOR_EXPUNGE(library);
  Permit();
  unload_if(seglist);
  return seglist;
}

#define FAKE_DOS ((struct DosLibrary *)&fx.dos)

static int bases_valid(void)
{
  return DOSBase == FAKE_DOS && UtilityBase == &fx.utility &&
         ExpansionBase == (struct ExpansionBase *)&fx.expansion;
}

static int bases_released(void)
{
  return fx.dos.lib_OpenCnt == 0U && fx.utility.lib_OpenCnt == 0U &&
         fx.expansion.lib_OpenCnt == 0U && fx.bad_closes == 0U;
}

/* One AllocAudio as ahi.device calls it. The fake worker creation fails, so
 * AllocAudio runs its whole setup up to CreateNewProc and takes its fail
 * path; the case checks what it saw. */
static void alloc_audio(void)
{
  struct TagItem tags[] = {{TAG_DONE, 0}};
  static struct AHIAudioCtrlDrv audio;

  memset(&audio, 0, sizeof(audio));
  audio.ahiac_MixFreq = 48000;
  audio.ahiac_Channels = 2;
  audio.ahiac_Sounds = 1;
  /* AllocAudio's detection reads the AX-present strap; its own write of 0
   * to the register would hide it from the next call. */
  *(volatile UWORD *)(fx.card + ZZ_REG_AUDIO_CONFIG) = 1;
  fx.proc_creates = 0;
  fx.dos_at_proc_create = NULL;
  check(intAHIsub_AllocAudio(tags, &audio) == AHISF_ERROR,
        "AllocAudio fails when the worker cannot be created");
  check(audio.ahiac_DriverData == NULL, "a failed AllocAudio frees its data");
  check(fx.int_servers == 0, "a failed AllocAudio removes its interrupt");
}

/* The pattern of a name buffer: 0xAA after the string. */
static void fill_name(UBYTE *name, ULONG size)
{
  memset(name, 0xAA, size);
  memcpy(name, "DEVS:AHI/zz9000ax.audio", 24);
}

/* Open must return the base and leave the caller's A1 buffer alone. */
static void open_vector_case(void)
{
  UBYTE name[64], expected[64];
  struct Library *library, *result;
  ULONG failures = fx.failures;

  reset();
  fill_name(name, sizeof(name));
  fill_name(expected, sizeof(expected));
  library = load_driver(TEST_SEGLIST);
  check(library != NULL, "init() succeeds with the fake card");
  if (!library)
    return;
  library->lib_Flags |= LIBF_DELEXP;
  exec_open(TEST_SEGLIST, name);
  result = fx.open_result;
  check(result == library, "Open returns the library base");
  check(memcmp(name, expected, sizeof(name)) == 0,
        "Open does not write through A1");
  check(library->lib_OpenCnt == 1U, "Open counts one opener");
  check(!(library->lib_Flags & LIBF_DELEXP), "Open clears LIBF_DELEXP");
  exec_close(library);
  check(library->lib_OpenCnt == 0U, "Close drops the opener");
  if (fx.failures == failures)
    puts("PASS: Open returns the base, leaves A1 alone and counts openers");
}

/* An Expunge while the library is open (avail flush during playback) must
 * not touch anything; the last Close unloads. */
static void expunge_while_open_case(void)
{
  UBYTE name[64];
  struct Library *library, *relibrary;
  ULONG failures = fx.failures;
  BPTR seglist;

  reset();
  fill_name(name, sizeof(name));
  library = exec_open(TEST_SEGLIST, name);
  check(library != NULL, "the driver loads and opens");
  if (!library)
    return;
  check(bases_valid(), "init() leaves the bases open");
  check(exec_expunge(library) == 0, "Expunge of an open library returns 0");
  check(listed_driver() == library, "an open library stays in the list");
  check(library->lib_Flags & LIBF_DELEXP, "Expunge flags a delayed expunge");
  check(bases_valid(), "the bases stay valid while the library is open");
  check(fx.freed_blocks == 0U, "an open library is not freed");

  Forbid();
  seglist = VECTOR_CLOSE(library, NULL);
  Permit();
  unload_if(seglist);
  check(seglist == TEST_SEGLIST, "the last Close of a delayed expunge "
        "returns the seglist");
  check(listed_driver() == NULL, "the unloaded library left the list");
  check(fx.freed_blocks == 1U, "the library block is freed once");
  check(fx.freed_memory == (APTR)((UBYTE *)library - library->lib_NegSize) &&
        fx.freed_size == (ULONG)library->lib_NegSize + library->lib_PosSize,
        "the whole library block (vectors and base) is freed");
  check(bases_released(), "the unload closes dos, utility and expansion once");
  check(DOSBase == NULL && UtilityBase == NULL && ExpansionBase == NULL,
        "the unload clears the base pointers");

  /* The next OpenLibrary loads the file again and runs init(). */
  relibrary = exec_open(TEST_SEGLIST, name);
  check(relibrary != NULL, "the driver loads again after the unload");
  check(bases_valid(), "init() opens the bases again");
  if (relibrary)
    exec_close(relibrary);
  if (fx.failures == failures)
    puts("PASS: Expunge waits for the last Close and unloads cleanly");
}

/* The reported freeze: play, close, avail flush, play again. */
static void flush_between_plays_case(void)
{
  UBYTE name[64];
  ULONG failures = fx.failures;
  ULONG cycle;

  reset();
  fill_name(name, sizeof(name));
  for (cycle = 0; cycle < TEST_CYCLES; cycle++) {
    struct Library *library = exec_open(TEST_SEGLIST, name);

    check(library != NULL, "the driver opens");
    if (!library)
      return;
    check(bases_valid(), "the bases are valid when AllocAudio starts");
    alloc_audio();
    check(fx.proc_creates == 1U,
          "AllocAudio reaches the worker creation");
    check(fx.dos_at_proc_create == FAKE_DOS,
          "the worker is created with a valid DOSBase");
    exec_close(library);
    /* Nothing has the driver open: exec flushes it. */
    exec_expunge(library);
    /* Either the library left the list and the bases are closed, or it
     * stayed and the bases are still the ones init() opened. */
    if (listed_driver())
      check(bases_valid(), "a library that stays in the list keeps its bases");
    else
      check(bases_released(), "a removed library closed its bases");
  }
  if (fx.failures == failures)
    puts("PASS: AllocAudio after an expunge has valid library bases");
}

/* A copy without a seglist (ROM, Resident) cannot be loaded again. */
static void resident_case(void)
{
  UBYTE name[64];
  struct Library *library;
  ULONG failures = fx.failures;

  reset();
  fill_name(name, sizeof(name));
  library = exec_open(0, name);
  check(library != NULL, "a resident copy opens");
  if (!library)
    return;
  exec_close(library);
  check(exec_expunge(library) == 0, "a resident copy returns no seglist");
  check(listed_driver() == library, "a resident copy stays in the list");
  check(bases_valid(), "a resident copy keeps its bases");
  check(fx.freed_blocks == 0U, "a resident copy is not freed");
  library = exec_open(0, name);
  check(library != NULL && bases_valid(), "a resident copy opens again");
  alloc_audio();
  check(fx.dos_at_proc_create == FAKE_DOS,
        "AllocAudio of a resident copy has a valid DOSBase");
  if (library)
    exec_close(library);
  if (fx.failures == failures)
    puts("PASS: a copy without a seglist is never expunged");
}

int main(void)
{
  SysBase = *(struct ExecBase **)4;
  open_vector_case();
  discard_driver();
  expunge_while_open_case();
  discard_driver();
  flush_between_plays_case();
  discard_driver();
  resident_case();
  discard_driver();
  return fx.failures ? 20 : 0;
}
