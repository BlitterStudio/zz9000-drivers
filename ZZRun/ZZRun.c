/*
 * ZZRun launches one trusted ARM app on a Zorro III ZZ9000.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/tasks.h>
#include <exec/execbase.h>
#include <devices/timer.h>
#include <devices/inputevent.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/dostags.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <graphics/gfxbase.h>
#include <graphics/modeid.h>
#include <workbench/startup.h>
#include <workbench/workbench.h>
#include <libraries/Picasso96.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/timer.h>
#include <proto/icon.h>
#include <proto/Picasso96.h>
#include <proto/zz9k.h>
#include "zz9k/request.h"
#include "zzapp_proto.h"
#include "zzrun_policy.h"
#include "zzrun_policy.c"
#include "zz9000_hw.h"
#include <devices/ahi.h>
#include <proto/ahi.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>


struct GfxBase *GfxBase;
struct IntuitionBase *IntuitionBase;
struct Library *IconBase;
struct Device *TimerBase;
struct Library *P96Base;
struct Library *ZZ9KBase;
struct Library *AHIBase;

extern struct WBStartup *_WBenchMsg;

#define ZZRUN_VERSION "0.1"
static const char version[] __attribute__((used)) =
    "$VER: ZZRun " ZZRUN_VERSION " (06.11.2026)\r\n";

#define ZZ_OK          0
#define ZZ_EIO        -1
#define ZZ_EINVAL     -2
#define ZZ_ENOMEM     -3
#define ZZ_EBUSY      -4
#define ZZ_ENOENT     -5
#define ZZ_EACCES     -6
#define ZZ_ENOSYS     -7
#define ZZ_ETIMEDOUT  -8
#define ZZ_EFAULT     -9
#define ZZ_ENOSPC    -10
#define ZZ_EEXIST    -11
#define ZZ_EBADF     -12

#define ZZ_FMT_RGB565    1u
#define ZZ_FMT_BGRA8888  2u
#define ZZ_FMT_CLUT8     3u
#define ZZ_LAYOUT_RGB565_BE 1u
#define ZZ_LAYOUT_BGRA8888  5u
#define ZZ_LAYOUT_CLUT8     7u

#define ZZRUN_MIN_STACK     16384UL
#define ZZRUN_IMAGE_LIMIT   ZZAPP_IMAGE_MAX_BYTES
#define ZZRUN_GRACE_MS      500u
#define ZZRUN_POLL_US       1000UL
#define ZZRUN_LONG_OP_MS    120000UL
#define ZZRUN_VRAM_ARM      0x00200000UL
#define ZZRUN_VRAM_END      0x033F0000UL
#define ZZRUN_HEAP_END      0x08200000UL
/* A card-vblank flip acknowledgement normally arrives within one refresh;
 * after this long ZZRun stops waiting for it and uses WaitTOF. */
#define ZZRUN_SCANOUT_ACK_MS 100UL
#define ZZRUN_SCREEN_MAX    4096u
#define ZZRUN_UNIX_DELTA    252460800UL
#define ZZRUN_READ_CHUNK    8192u
_Static_assert(ZZRUN_ST_IDLE == ZZAPP_ST_IDLE, "idle");
_Static_assert(ZZRUN_ST_CLAIMED == ZZAPP_ST_CLAIMED, "claimed");
_Static_assert(ZZRUN_ST_LOADING == ZZAPP_ST_LOADING, "loading");
_Static_assert(ZZRUN_ST_STARTING == ZZAPP_ST_STARTING, "starting");
_Static_assert(ZZRUN_ST_RUNNING == ZZAPP_ST_RUNNING, "running");
_Static_assert(ZZRUN_ST_STOPPING == ZZAPP_ST_STOPPING, "stopping");
_Static_assert(ZZRUN_ST_FAULTED == ZZAPP_ST_FAULTED, "faulted");
_Static_assert(ZZRUN_FAULT_WATCHDOG_RESTART == ZZAPP_FAULT_WATCHDOG_RESTART,
               "watchdog restart");
_Static_assert(ZZRUN_POLICY_GRACE_MS == ZZRUN_GRACE_MS, "grace");
_Static_assert(ZZAPP_AUDIO_EPOCH_OFF == 28u, "audio epoch offset");
_Static_assert(ZZAPP_STOP_REQ_BYTES == ZZRUN_STOP_REQ_BYTES, "stop bytes");
_Static_assert(ZZAPP_STOP_GENERATION == 0u, "stop generation offset");
_Static_assert(ZZRUN_STATUS_NO_MEMORY == ZZ9K_STATUS_NO_MEMORY, "no memory");
_Static_assert(ZZRUN_LOAD_BEGIN_BYTES == ZZAPP_LOAD_HOST_VISIBLE_MIB + 4u,
               "load begin bytes");
_Static_assert(ZZRUN_BLOCK_NONE == ZZAPP_BLOCK_NONE, "block none");
_Static_assert(ZZRUN_BLOCK_FILE == ZZAPP_BLOCK_FILE, "block file");
_Static_assert(ZZRUN_BLOCK_CONSOLE == ZZAPP_BLOCK_CONSOLE, "block console");
_Static_assert(ZZRUN_BLOCK_EVENT == ZZAPP_BLOCK_EVENT, "block event");
_Static_assert(ZZRUN_BLOCK_SLEEP == ZZAPP_BLOCK_SLEEP, "block sleep");
_Static_assert(ZZRUN_BLOCK_VSYNC == ZZAPP_BLOCK_VSYNC, "block vsync");
_Static_assert(ZZRUN_BLOCK_AUDIO == ZZAPP_BLOCK_AUDIO, "block audio");

#define MMU_MAPP_CACHEINHIBIT 0x00000040UL
#define MMU_MAPP_COPYBACK     0x00002000UL
#define MMU_MAPP_IMPRECISE    0x00200000UL
#define MMU_MAPP_NONSERIALIZED 0x20000000UL
#define MMU_CACHE_MASK (MMU_MAPP_CACHEINHIBIT | MMU_MAPP_COPYBACK | \
                        MMU_MAPP_IMPRECISE | MMU_MAPP_NONSERIALIZED)
#define ZZRUN_AHI_BUFFER_COUNT 2u
#define ZZRUN_AHI_BUFFER_BYTES 8192u

struct mmu_context;

static struct ZZ9000Board board;
static uint8_t *window;
static uint32_t window_bytes;
static int abs_paths;
static int stop_asked;
static int user_stop;
static int stop_event_sent;
static uint32_t stop_ms;
static uint32_t flip_acked;
static int have_two;
/* Panning fallback uses a triple-height bitmap when P96 permits it.  The
 * spare third page lets the app start the next frame while the last pan is
 * pending; a two-page bitmap remains a compatible fallback. */
static int have_pan;
static int pan_count;
static int showing;
static char pub_name[128];
static char elf_path[256];
static char arg_store[ZZAPP_ARGV_MAX][ZZAPP_ARGV_STR_MAX];
static int app_argc;
static BPTR root_lock;
static struct Screen *screen;
static struct Window *input_win;
static struct ScreenBuffer *buf_front;
static struct ScreenBuffer *buf_back;
static int joy_port0;
static ULONG card_front;
static ULONG card_back;
static ULONG bound_w, bound_h, bound_pitch, bound_fmt;
static uint16_t mouse_buttons;
static ULONG joy_last[2];
static int joy_valid[2];
static struct MsgPort *timer_port;
static struct timerequest *timer_io;
static int timer_pending;
static struct MsgPort *flip_safe_port;
static uint32_t flip_pending;
static uint32_t flip_safe_pending;
static uint32_t scanout_submitted;
static uint32_t scanout_submit_ms;
static int scanout_fallback;
static uint32_t palette_generation;
static struct Library *MMUBase;
static struct mmu_context *mmu_context;
static uint32_t owned_generation;
static uint32_t owned_slot;
static uint32_t maxmem_mib;
static uint32_t hostmem_mib;
static uint32_t runtime_caps;
static BPTR console_handle;
static int console_failed;
static int audio_ahi;
static struct MsgPort *ahi_port;
static struct AHIRequest *ahi_request[ZZRUN_AHI_BUFFER_COUNT];
static int ahi_device_open = -1;
static uint32_t ahi_rate;
static uint32_t ahi_channels;
static uint32_t ahi_pending_bytes[ZZRUN_AHI_BUFFER_COUNT];
static uint8_t *ahi_buffer[ZZRUN_AHI_BUFFER_COUNT];
static uint8_t ahi_pending[ZZRUN_AHI_BUFFER_COUNT];
static uint32_t ahi_underruns;
static uint32_t ahi_stream_epoch;
static int ahi_stream_epoch_valid;
static int console_output_failed;
static int audio_open_failed;
static int launcher_output_active;
static uint32_t launcher_log_dropped;
static struct zzrun_owned_cursor input_head_cursor;
static struct zzrun_owned_cursor console_output_tail_cursor;
static struct zzrun_owned_cursor console_input_head_cursor;
static struct zzrun_owned_cursor log_tail_cursor;
static struct zzrun_owned_cursor ahi_tail_cursor;


struct file_slot {
    BPTR fh;
    int used;
};

struct list_slot {
    BPTR lock;
    struct FileInfoBlock *fib;
    int used;
    int primed;
};

static struct file_slot files[ZZAPP_FD_COUNT];
static struct list_slot lists[ZZAPP_FD_COUNT];
static uint32_t file_reply_seq;
static uint32_t file_generation;
static uint8_t file_io_buffer[ZZRUN_READ_CHUNK];

static void cleanup(void);
static int tool_is_ahi(const char *value);
static void launcher_status(const char *msg);
static int heartbeat_owned(uint32_t captured_generation);
static int generation_owned(uint32_t captured_generation);
static void request_stop(void);
static int dos_packet_async(struct MsgPort *handler, LONG action, LONG arg1,
                            LONG arg2, LONG arg3, LONG arg4, LONG arg5,
                            uint32_t captured_generation, LONG *result_out,
                            int *ownership_lost, int *abandoned_out);
static LONG file_packet_async(BPTR fh, LONG action, LONG arg2, LONG arg3,
                              LONG arg4, LONG arg5, int *transport_ok);
static int examine_lock_async(BPTR lock, struct FileInfoBlock *fib,
                              int *transport_ok);
static int examine_next_async(BPTR lock, struct FileInfoBlock *fib,
                              int *transport_ok);
static BPTR lock_async(BPTR dir, const char *name, LONG access,
                       int *transport_ok);
static BPTR lock_abs_async(const char *name, int *transport_ok);
static BPTR open_find_async(BPTR dir, const char *name, LONG mode,
                            int *transport_ok);
static int end_async(int slot, BPTR fh, int *transport_ok);
static void end_or_close(BPTR fh);
static int life_put_owned(uint32_t off, uint32_t value);
static void serve_file_request(const struct zzapp_file_wire *req);
static void timer_arm(void);
static void timer_reap(void);


static int stack_ok(void)
{
    struct Task *task = FindTask(NULL);
    if (!task || (uintptr_t)task->tc_SPUpper <= (uintptr_t)task->tc_SPLower ||
        (uintptr_t)task->tc_SPUpper - (uintptr_t)task->tc_SPLower < ZZRUN_MIN_STACK)
        return 0;
    return 1;
}

static struct mmu_context *mmu_default_context(void)
{
    register struct mmu_context *d0 __asm("d0");
    register struct Library *a6 __asm("a6") = MMUBase;
    __asm volatile("jsr -150(a6)"
                   : "=r"(d0)
                   : "r"(a6)
                   : "cc", "memory", "d1", "a0", "a1");
    return d0;
}

static uint32_t mmu_page_size(struct mmu_context *context)
{
    register uint32_t d0 __asm("d0");
    register struct mmu_context *a0 __asm("a0") = context;
    register struct Library *a6 __asm("a6") = MMUBase;
    __asm volatile("jsr -48(a6)"
                   : "=r"(d0), "+r"(a0)
                   : "r"(a6)
                   : "cc", "memory", "d1", "a1");
    return d0;
}

static int mmu_set_properties(struct mmu_context *context, uint32_t flags,
                              uint32_t mask, uint32_t address, uint32_t size)
{
    register uint32_t d0 __asm("d0") = size;
    register uint32_t d1 __asm("d1") = flags;
    register uint32_t d2 __asm("d2") = mask;
    register struct mmu_context *a0 __asm("a0") = context;
    register void *a1 __asm("a1") = (void *)(uintptr_t)address;
    register void *a2 __asm("a2") = NULL;
    register struct Library *a6 __asm("a6") = MMUBase;
    __asm volatile("jsr -84(a6)"
                   : "+r"(d0), "+r"(d1), "+r"(d2), "+r"(a0), "+r"(a1), "+r"(a2)
                   : "r"(a6)
                   : "cc", "memory");
    return d0 != 0;
}

static void mmu_lock_context(struct mmu_context *context)
{
    register struct mmu_context *a0 __asm("a0") = context;
    register struct Library *a6 __asm("a6") = MMUBase;

    __asm volatile("jsr -72(a6)"
                   : "+r"(a0)
                   : "r"(a6)
                   : "cc", "memory", "d0", "d1", "a1", "a2");
}

static void mmu_unlock_context(struct mmu_context *context)
{
    register struct mmu_context *a0 __asm("a0") = context;
    register struct Library *a6 __asm("a6") = MMUBase;

    __asm volatile("jsr -78(a6)"
                   : "+r"(a0)
                   : "r"(a6)
                   : "cc", "memory", "d0", "d1", "a1", "a2");
}

static int mmu_rebuild_tree(struct mmu_context *context)
{
    register uint32_t d0 __asm("d0");
    register struct mmu_context *a0 __asm("a0") = context;
    register struct Library *a6 __asm("a6") = MMUBase;

    __asm volatile("jsr -96(a6)"
                   : "=r"(d0), "+r"(a0)
                   : "r"(a6)
                   : "cc", "memory", "d1", "a1", "a2");
    return d0 != 0;
}

/* The shared window stays cache-inhibited once any page is inhibited.
 * Restoring the saved mode would make another client's live window
 * copyback-cacheable again. There is no snapshot: a failed inhibit must
 * not call SetProperties on pages it did not change. */
static void window_cache_release(void)
{
    mmu_context = NULL;
    if (MMUBase) {
        CloseLibrary(MMUBase);
        MMUBase = NULL;
    }
}

static int window_cache_disable(void)
{
    uint32_t page_bytes;
    uint32_t first;
    uint32_t last;
    uint32_t count;

    MMUBase = OpenLibrary((CONST_STRPTR)"mmu.library", 41);
    if (!MMUBase) {
        /* Without mmu.library the window stays cacheable; that is the
         * long-standing behaviour, so keep running with the warning. */
        puts("ZZRun: mmu.library unavailable; shared window remains cacheable.");
        return 1;
    }
    mmu_context = mmu_default_context();
    page_bytes = mmu_context ? mmu_page_size(mmu_context) : 0;
    if (page_bytes == 0 || (page_bytes & (page_bytes - 1u)) != 0u) {
        puts("ZZRun: mmu.library has no usable page size.");
        window_cache_release();
        return 0;
    }
    first = (uint32_t)(uintptr_t)window & ~(page_bytes - 1u);
    if (window_bytes > UINT32_MAX - (uint32_t)(uintptr_t)window ||
        (uint32_t)(uintptr_t)window + window_bytes > UINT32_MAX - (page_bytes - 1u)) {
        puts("ZZRun: shared window range overflows.");
        window_cache_release();
        return 0;
    }
    last = ((uint32_t)(uintptr_t)window + window_bytes + page_bytes - 1u) &
           ~(page_bytes - 1u);
    count = (last - first) / page_bytes;
    if (count == 0) {
        puts("ZZRun: shared window page count is invalid.");
        window_cache_release();
        return 0;
    }

    /* Write back and invalidate while the old mapping is still active. */
    CacheClearE(window, window_bytes, CACRF_ClearD);
    mmu_lock_context(mmu_context);
    if (!mmu_set_properties(mmu_context, MMU_MAPP_CACHEINHIBIT,
                            MMU_CACHE_MASK, first, last - first)) {
        mmu_unlock_context(mmu_context);
        puts("ZZRun: cannot configure a non-cacheable shared window.");
        window_cache_release();
        return 0;
    }
    if (!mmu_rebuild_tree(mmu_context)) {
        mmu_unlock_context(mmu_context);
        puts("ZZRun: cannot activate a non-cacheable shared window.");
        window_cache_release();
        return 0;
    }
    mmu_unlock_context(mmu_context);
    window_cache_release();
    return 1;
}

static void win_cache(uint32_t off, uint32_t len)
{
    if (window && len)
        CacheClearE(window + off, len, CACRF_ClearD);
}

static uint32_t life_get(uint32_t off)
{
    win_cache(ZZAPP_LIFE_OFF + off, 4);
    return zzapp_get_be32(window + ZZAPP_LIFE_OFF + off);
}

static uint32_t shared_current_generation(void *context)
{
    (void)context;
    return life_get(ZZAPP_LIFE_GENERATION_OFF);
}

static struct zzrun_shared_writer shared_writer(uint32_t generation)
{
    struct zzrun_shared_writer writer;

    writer.context = NULL;
    writer.owned_generation = generation;
    writer.attached = window != NULL;
    writer.read_current_generation = shared_current_generation;
    return writer;
}

static int shared_write_be32(uint8_t *out, uint32_t value, uint32_t generation)
{
    struct zzrun_shared_writer writer = shared_writer(generation);

    return zzrun_shared_write_be32(&writer, out, value);
}

static int shared_write_bytes(uint8_t *out, const uint8_t *in, uint32_t bytes,
                              uint32_t generation)
{
    struct zzrun_shared_writer writer = shared_writer(generation);

    return zzrun_shared_write_bytes(&writer, out, in, bytes);
}

static int life_put(uint32_t off, uint32_t value, uint32_t generation)
{
    if (!window)
        return 0;
    win_cache(ZZAPP_LIFE_OFF + off, 4);
    if (!shared_write_be32(window + ZZAPP_LIFE_OFF + off, value, generation))
        return 0;
    win_cache(ZZAPP_LIFE_OFF + off, 4);
    return 1;
}

static uint32_t now_ms(void)
{
    struct timeval tv;
    GetSysTime(&tv);
    return (uint32_t)tv.tv_secs * 1000u + (uint32_t)(tv.tv_micro / 1000);
}

static void heartbeat(void)
{
    (void)life_put_owned(ZZAPP_LIFE_HEARTBEAT_OFF, now_ms());
}


static int ctrl_c_set(void)
{
    return (SetSignal(0, 0) & SIGBREAKF_CTRL_C) != 0;
}

static void usage(void)
{
    puts("ZZRun <app.elf> [MAXMEM=<MiB>] [HOSTMEM=<MiB>] [AUDIO=AHI] [args]\n"
         "ZZRun STATUS\n"
         "ZZRun RELEASE <slot>\n"
         "Run Stack 32768 in this Shell first.\n"
         "Workbench ToolTypes: APP, ROOT, ABS, MODE, ARGS, JOYPORT0, AUDIO, MAXMEM, HOSTMEM.\n"
         "ROOT defaults to the current directory, or the icon directory.");
}

static void tell(const char *msg)
{
    if (launcher_output_active)
        launcher_status(msg);
    else
        puts(msg);
    if (_WBenchMsg && IntuitionBase) {
        struct EasyStruct es;
        memset(&es, 0, sizeof(es));
        es.es_StructSize = sizeof(es);
        es.es_Title = (UBYTE *)"ZZRun";
        es.es_TextFormat = (UBYTE *)msg;
        es.es_GadgetFormat = (UBYTE *)"OK";
        EasyRequestArgs(NULL, &es, NULL, NULL);
    }
}

static const char *status_name(int status)
{
    switch (status) {
    case ZZ9K_STATUS_OK: return "ok";
    case ZZ9K_STATUS_BUSY: return "busy";
    case ZZ9K_STATUS_UNSUPPORTED: return "unsupported";
    case ZZ9K_STATUS_BAD_REQUEST: return "bad request";
    case ZZ9K_STATUS_NO_MEMORY: return "no memory";
    case ZZ9K_STATUS_TIMEOUT: return "timeout";
    case ZZ9K_STATUS_NOT_FOUND: return "not found";
    case ZZ9K_STATUS_IO_ERROR: return "io error";
    default: return "error";
    }
}

static const char *fault_name(uint32_t code)
{
    switch (code) {
    case ZZAPP_FAULT_NONE: return "none";
    case ZZAPP_FAULT_DATA: return "data abort";
    case ZZAPP_FAULT_PREFETCH: return "prefetch abort";
    case ZZAPP_FAULT_UNDEF: return "undefined instruction";
    case ZZAPP_FAULT_WATCHDOG: return "watchdog";
    case ZZAPP_FAULT_FORCED: return "forced stop";
    case ZZAPP_FAULT_WATCHDOG_RESTART: return "watchdog-plus-restart";
    default: return "unknown";
    }
}

static const char *state_name(uint32_t state)
{
    return zzrun_slot_state_name(state);
}

static int mailbox_selected(uint16_t opcode, const uint8_t *payload, uint16_t len,
                            uint32_t slot, uint32_t generation,
                            ZZ9KMailboxEntry *reply)
{
    ZZ9KRequest request;
    uint8_t extended[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    int status;

    if (!reply)
        return ZZ9K_STATUS_BAD_REQUEST;
    if (zzrun_request_build(extended, payload, len, 1, slot, generation) !=
        ZZAPP_MAILBOX_PAYLOAD_BYTES)
        return ZZ9K_STATUS_BAD_REQUEST;
    zz9k_request_init(&request, opcode);
    request.entry.payload_len = ZZAPP_MAILBOX_PAYLOAD_BYTES;
    memcpy(request.entry.payload.inline_data, extended,
           ZZAPP_MAILBOX_PAYLOAD_BYTES);
    status = ZZ9KCall(&request, reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
    if (status == ZZ9K_STATUS_OK && reply->status != ZZ9K_STATUS_OK)
        status = reply->status;
    return status;
}

static int mailbox(uint16_t opcode, const uint8_t *payload, uint16_t len,
                   ZZ9KMailboxEntry *reply)
{
    return mailbox_selected(opcode, payload, len, owned_slot, owned_generation,
                            reply);
}

static int mailbox_for_slot(uint16_t opcode, uint32_t slot,
                            ZZ9KMailboxEntry *reply)
{
    return mailbox_selected(opcode, NULL, 0, slot, 0, reply);
}

static int mailbox_long(uint16_t opcode, const uint8_t *payload, uint16_t len,
                        ZZ9KMailboxEntry *reply)
{
    ZZ9KRequest request;
    ZZ9KAsyncRequest async;
    uint8_t extended[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    uint32_t retry_started;
    uint32_t lease_generation;
    int status;

    if (!reply)
        return ZZ9K_STATUS_BAD_REQUEST;
    if (zzrun_request_build(extended, payload, len, 1, owned_slot,
                            owned_generation) != ZZAPP_MAILBOX_PAYLOAD_BYTES)
        return ZZ9K_STATUS_BAD_REQUEST;
    zz9k_request_init(&request, opcode);
    request.entry.payload_len = ZZAPP_MAILBOX_PAYLOAD_BYTES;
    memcpy(request.entry.payload.inline_data, extended,
           ZZAPP_MAILBOX_PAYLOAD_BYTES);
    retry_started = now_ms();
    lease_generation = owned_generation;
    if (lease_generation && !heartbeat_owned(lease_generation))
        return ZZ9K_STATUS_BAD_REQUEST;
    for (;;) {
        uint32_t operation_started;

        memset(&async, 0, sizeof(async));
        status = ZZ9KCallAsync(&async, &request, NULL, NULL);
        if (status == ZZ9K_STATUS_QUEUED) {
            operation_started = now_ms();
            while (!async.completed) {
                uint32_t completed = 0;
                if (lease_generation && !heartbeat_owned(lease_generation)) {
                    (void)ZZ9KCancelAsync(&async);
                    return ZZ9K_STATUS_BAD_REQUEST;
                }
                status = ZZ9KPoll(1, &completed);
                if (status != ZZ9K_STATUS_OK)
                    return status;
                if (async.completed)
                    break;
                if (now_ms() - operation_started >= ZZRUN_LONG_OP_MS) {
                    (void)ZZ9KCancelAsync(&async);
                    return ZZ9K_STATUS_TIMEOUT;
                }
                Delay(1);
            }
            *reply = async.reply;
            status = async.status;
        }
        if (!zzrun_deferred_should_retry(status, ZZ9K_STATUS_BUSY,
                                         retry_started, now_ms()))
            return status;
        if (lease_generation && !heartbeat_owned(lease_generation))
            return ZZ9K_STATUS_BAD_REQUEST;
        Delay(1);
    }
}

static int op_empty(uint16_t opcode, ZZ9KMailboxEntry *reply)
{
    return mailbox(opcode, NULL, 0, reply);
}

static int owns_current_generation(void)
{
    uint32_t generation;
    uint32_t state;

    if (!window || owned_generation == 0)
        return 0;
    generation = life_get(ZZAPP_LIFE_GENERATION_OFF);
    state = life_get(ZZAPP_LIFE_STATE_OFF);
    return !zzrun_slot_replaced(1, owned_slot, owned_generation,
                                owned_slot, generation, state);
}

static int life_put_owned(uint32_t off, uint32_t value)
{
    return life_put(off, value, owned_generation);
}

static int heartbeat_owned(uint32_t captured_generation)
{
    uint32_t observed_generation;

    if (!window || owned_generation == 0)
        return 0;
    observed_generation = life_get(ZZAPP_LIFE_GENERATION_OFF);
    if (!zzrun_lease_renew_allowed(captured_generation, owned_generation,
                                   observed_generation))
        return 0;
    heartbeat();
    return 1;
}

static int generation_owned(uint32_t captured_generation)
{
    if (!window || owned_generation == 0)
        return 0;
    return zzrun_lease_renew_allowed(captured_generation, owned_generation,
                                     life_get(ZZAPP_LIFE_GENERATION_OFF));
}
static void stop_owned(void)
{
    ZZ9KMailboxEntry reply;
    uint8_t payload[ZZRUN_STOP_REQ_BYTES];

    if (!owns_current_generation())
        return;
    zzrun_stop_payload(payload, owned_generation);
    (void)mailbox(ZZAPP_OP_STOP, payload, ZZRUN_STOP_REQ_BYTES, &reply);
}

static int print_status(const struct zzrun_slots_query *query)
{
    ZZ9KMailboxEntry reply;
    uint32_t slot;
    int status;

    for (slot = 0; slot < query->slots; slot++) {
        struct zzrun_list_reply item;

        status = mailbox_for_slot(ZZAPP_OP_LIST, slot, &reply);
        if (status != ZZ9K_STATUS_OK ||
            !zzrun_list_reply_decode(reply.payload.inline_data, reply.payload_len,
                                     &item)) {
            printf("ZZRun: APP_LIST slot %lu failed (%s)\n",
                   (unsigned long)slot, status_name(status));
            return 20;
        }
        printf("slot %lu state %s name %s generation %lu committed %lu MiB "
               "limit %lu MiB cpu %lu s block %s reason %s\n",
               (unsigned long)slot, state_name(item.state),
               item.name[0] ? item.name : "-",
               (unsigned long)item.generation,
               (unsigned long)(item.committed_bytes / ZZRUN_MIB_BYTES),
               (unsigned long)(item.limit_bytes / ZZRUN_MIB_BYTES),
               (unsigned long)(item.cpu_ms / 1000u),
               zzrun_block_reason_name(item.block_reason),
               zzrun_terminal_reason_name(item.terminal_reason));
    }
    return 0;
}

static int release_quarantined_slot(uint32_t slot)
{
    ZZ9KMailboxEntry reply;
    struct zzrun_list_reply item;
    int status;

    status = mailbox_for_slot(ZZAPP_OP_LIST, slot, &reply);
    if (status != ZZ9K_STATUS_OK ||
        !zzrun_list_reply_decode(reply.payload.inline_data, reply.payload_len,
                                 &item)) {
        printf("ZZRun: APP_LIST slot %lu failed (%s)\n",
               (unsigned long)slot, status_name(status));
        return 20;
    }
    if (item.state != ZZAPP_ST_STOPPING ||
        item.terminal_reason != ZZAPP_END_QUARANTINED) {
        printf("ZZRun: slot %lu is not quarantined\n", (unsigned long)slot);
        return 20;
    }
    status = mailbox_selected(ZZAPP_OP_RELEASE, NULL, 0, slot,
                              item.generation, &reply);
    if (status != ZZ9K_STATUS_OK) {
        printf("ZZRun: release slot %lu failed (%s)\n",
               (unsigned long)slot, status_name(status));
        return 20;
    }
    printf("ZZRun: released quarantined slot %lu\n", (unsigned long)slot);
    return 0;
}


static int tool_true(const char *value)
{
    if (!value || !value[0])
        return 0;
    if (value[0] == '1' && value[1] == 0)
        return 1;
    if ((value[0] == 'y' || value[0] == 'Y') &&
        (value[1] == 0 || (value[1] == 'e' || value[1] == 'E')))
        return 1;
    if (value[0] == 't' || value[0] == 'T')
        return 1;
    return 0;
}

static int parse_mode(const char *text, ULONG *out)
{
    char *end = NULL;
    unsigned long value;
    if (!text || !text[0])
        return 0;
    if (text[0] == '$')
        value = strtoul(text + 1, &end, 16);
    else if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
        value = strtoul(text, &end, 16);
    else
        value = strtoul(text, &end, 10);
    if (!end || *end)
        return 0;
    *out = (ULONG)value;
    return 1;
}

static int add_arg(const char *text)
{
    size_t n;
    if (!text)
        return 1;
    n = strlen(text);
    if (!zzrun_arg_length_allowed((uint32_t)n) ||
        app_argc >= (int)ZZAPP_ARGV_MAX - 1) {
        tell("Too many arguments, or an argument is longer than 127 bytes.");
        return 0;
    }
    memcpy(arg_store[app_argc], text, n + 1);
    app_argc++;
    return 1;
}

static int add_args_split(const char *text)
{
    char buf[256];
    char *p;
    if (!text || !text[0])
        return 1;
    if (strlen(text) >= sizeof(buf)) {
        tell("ARGS is longer than 255 bytes.");
        return 0;
    }
    memcpy(buf, text, strlen(text) + 1);
    p = buf;
    while (*p) {
        char *start;
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;
        start = p;
        while (*p && *p != ' ' && *p != '\t')
            p++;
        if (*p)
            *p++ = 0;
        if (!add_arg(start))
            return 0;
    }
    return 1;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wincompatible-pointer-types"
static char *tool_value(struct DiskObject *icon, const char *name)
{
    return (char *)FindToolType((STRPTR *)icon->do_ToolTypes, (STRPTR)name);
}
#pragma GCC diagnostic pop

static int read_tooltypes(struct DiskObject *icon, char *app_out, size_t app_n,
                          char *root_out, size_t root_n, ULONG *mode,
                          int *have_mode, uint32_t *maxmem,
                          uint32_t *hostmem)
{
    char *value;
    if (!icon || !icon->do_ToolTypes)
        return 1;
    value = tool_value(icon, "APP");
    if (value && value[0] && app_out && strlen(value) < app_n)
        memcpy(app_out, value, strlen(value) + 1);
    value = tool_value(icon, "ROOT");
    if (value && value[0] && root_out && strlen(value) < root_n)
        memcpy(root_out, value, strlen(value) + 1);
    value = tool_value(icon, "ABS");
    if (value && (value[0] == 0 || tool_true(value)))
        abs_paths = 1;
    value = tool_value(icon, "MODE");
    if (value && value[0] && mode && parse_mode(value, mode))
        *have_mode = 1;
    value = tool_value(icon, "JOYPORT0");
    if (value && (value[0] == 0 || tool_true(value)))
        joy_port0 = 1;
    value = tool_value(icon, "AUDIO");
    if (tool_is_ahi(value))
        audio_ahi = 1;
    value = tool_value(icon, "MAXMEM");
    if (value && !zzrun_parse_maxmem(value, maxmem))
        return 0;
    value = tool_value(icon, "HOSTMEM");
    if (value && !zzrun_parse_hostmem(value, hostmem))
        return 0;
    value = tool_value(icon, "ARGS");
    return !value || add_args_split(value);
}

static int tool_is_ahi(const char *value)
{
    return value && (tolower((unsigned char)value[0]) == 'a') &&
           value[1] && (tolower((unsigned char)value[1]) == 'h') &&
           value[2] && (tolower((unsigned char)value[2]) == 'i') &&
           value[3] == '\0';
}

static int load_program_icon(char *app_out, size_t app_n, char *root_out,
                             size_t root_n, ULONG *mode, int *have_mode,
                             uint32_t *maxmem, uint32_t *hostmem)
{
    struct DiskObject *icon;
    BPTR old = 0;
    int have_old = 0;
    if (!IconBase)
        return 1;
    if (_WBenchMsg && _WBenchMsg->sm_NumArgs >= 1) {
        old = CurrentDir(_WBenchMsg->sm_ArgList[0].wa_Lock);
        have_old = 1;
        icon = GetDiskObject(_WBenchMsg->sm_ArgList[0].wa_Name);
    } else {
        icon = GetDiskObject((STRPTR)"PROGDIR:ZZRun");
    }
    if (icon) {
        int ok = read_tooltypes(icon, app_out, app_n, root_out, root_n, mode,
                                have_mode, maxmem, hostmem);
        FreeDiskObject(icon);
        if (!ok) {
            if (have_old)
                CurrentDir(old);
            return 0;
        }
    }
    if (have_old)
        CurrentDir(old);
    return 1;
}

static int under_root(BPTR lock)
{
    BPTR cur = DupLock(lock);
    while (cur) {
        BPTR parent;
        if (SameLock(cur, root_lock) == LOCK_SAME) {
            UnLock(cur);
            return 1;
        }
        parent = ParentDir(cur);
        UnLock(cur);
        cur = parent;
    }
    return 0;
}

static int step_parent(BPTR *cur, int *err)
{
    BPTR parent = ParentDir(*cur);
    if (!parent || !under_root(parent)) {
        if (parent)
            UnLock(parent);
        *err = ZZ_EACCES;
        return 0;
    }
    UnLock(*cur);
    *cur = parent;
    return 1;
}

static int enter_dir(BPTR *cur, const char *name, int *err)
{
    int transport_ok;
    BPTR child = lock_async(*cur, name, ACCESS_READ, &transport_ok);

    if (!transport_ok) {
        *err = ZZ_EIO;
        return 0;
    }
    if (!child) {
        *err = IoErr() == ERROR_OBJECT_NOT_FOUND ||
               IoErr() == ERROR_DIR_NOT_FOUND ? ZZ_ENOENT : ZZ_EIO;
        return 0;
    }
    if (!under_root(child)) {
        UnLock(child);
        *err = ZZ_EACCES;
        return 0;
    }
    UnLock(*cur);
    *cur = child;
    return 1;
}

/* A slash after a name separates the next component. A slash with no
 * name (a leading slash, or two slashes) is the parent. `..` is parent.
 * A single trailing slash is only the separator. */
static int next_comp(const char **pp, char *out, size_t n, int *parent, int *end)
{
    const char *p = *pp;
    size_t i = 0;
    *parent = 0;
    *end = 0;
    if (*p == 0) {
        *end = 1;
        return 1;
    }
    if (*p == '/') {
        *parent = 1;
        *pp = p + 1;
        return 1;
    }
    while (*p && *p != '/') {
        if (i + 1 >= n)
            return 0;
        out[i++] = *p++;
    }
    out[i] = 0;
    if (*p == '/')
        p++;
    *pp = p;
    return 1;
}

static BPTR resolve_rel(const char *path, char *leaf, size_t leaf_n, int *err)
{
    const char *p = path;
    BPTR cur = DupLock(root_lock);
    char comp[108];
    char hold[108];
    int have_hold = 0;

    hold[0] = 0;
    leaf[0] = 0;
    if (!cur) {
        *err = ZZ_EIO;
        return 0;
    }
    for (;;) {
        int parent = 0, end = 0;
        comp[0] = 0;
        if (!next_comp(&p, comp, sizeof(comp), &parent, &end)) {
            *err = ZZ_EINVAL;
            UnLock(cur);
            return 0;
        }
        if (end)
            break;
        if (parent || strcmp(comp, "..") == 0) {
            if (have_hold) {
                if (!enter_dir(&cur, hold, err)) {
                    UnLock(cur);
                    return 0;
                }
                have_hold = 0;
            }
            if (!step_parent(&cur, err)) {
                if (cur)
                    UnLock(cur);
                return 0;
            }
            continue;
        }
        if (strcmp(comp, ".") == 0)
            continue;
        if (have_hold) {
            if (!enter_dir(&cur, hold, err)) {
                UnLock(cur);
                return 0;
            }
        }
        memcpy(hold, comp, strlen(comp) + 1);
        have_hold = 1;
    }
    if (have_hold) {
        if (strlen(hold) >= leaf_n) {
            *err = ZZ_EINVAL;
            UnLock(cur);
            return 0;
        }
        memcpy(leaf, hold, strlen(hold) + 1);
    }
    return cur;
}

static int32_t zz_from_io(void)
{
    switch (IoErr()) {
    case 0:
        return ZZ_OK;
    case ERROR_OBJECT_NOT_FOUND:
    case ERROR_DIR_NOT_FOUND:
    case ERROR_DEVICE_NOT_MOUNTED:
    case ERROR_NO_MORE_ENTRIES:
        return ZZ_ENOENT;
    case ERROR_DISK_FULL:
        return ZZ_ENOSPC;
    case ERROR_WRITE_PROTECTED:
    case ERROR_READ_PROTECTED:
    case ERROR_DELETE_PROTECTED:
        return ZZ_EACCES;
    case ERROR_OBJECT_EXISTS:
        return ZZ_EEXIST;
    case ERROR_NO_FREE_STORE:
        return ZZ_ENOMEM;
    case ERROR_OBJECT_IN_USE:
        return ZZ_EBUSY;
    case ERROR_SEEK_ERROR:
        return ZZ_EINVAL;
    default:
        return ZZ_EIO;
    }
}

static int slot_index(int32_t id, int *err)
{
    if (id < ZZAPP_FD_BASE || id >= ZZAPP_FD_BASE + ZZAPP_FD_COUNT) {
        *err = ZZ_EBADF;
        return -1;
    }
    return (int)(id - ZZAPP_FD_BASE);
}

static uint32_t amiga_unix(const struct DateStamp *ds)
{
    if (!ds || (ds->ds_Days == 0 && ds->ds_Minute == 0 && ds->ds_Tick == 0))
        return 0;
    return ZZRUN_UNIX_DELTA +
           (uint32_t)ds->ds_Days * 86400u +
           (uint32_t)ds->ds_Minute * 60u +
           (uint32_t)ds->ds_Tick / 50u;
}

static int examine_lock(BPTR lock, struct FileInfoBlock *fib,
                        int *transport_ok)
{
    return examine_lock_async(lock, fib, transport_ok);
}

static int lock_target(BPTR dir, const char *leaf, BPTR *out, int *err)
{
    int transport_ok;
    BPTR locked;
    if (!leaf[0]) {
        *out = DupLock(dir);
        if (!*out) {
            *err = ZZ_EIO;
            return 0;
        }
        return 1;
    }
    locked = lock_async(dir, leaf, ACCESS_READ, &transport_ok);
    if (!transport_ok) {
        *err = ZZ_EIO;
        return 0;
    }
    if (!locked) {
        *err = zz_from_io();
        if (*err == ZZ_OK)
            *err = ZZ_ENOENT;
        return 0;
    }
    if (!abs_paths && !under_root(locked)) {
        UnLock(locked);
        *err = ZZ_EACCES;
        return 0;
    }
    *out = locked;
    return 1;
}

static int final_path_under_root(BPTR dir, const char *leaf, int create,
                                 int *err)
{
    int transport_ok;
    BPTR locked = lock_async(dir, leaf, ACCESS_READ, &transport_ok);

    if (!transport_ok) {
        *err = ZZ_EIO;
        return 0;
    }
    if (!locked) {
        if (create && IoErr() == ERROR_OBJECT_NOT_FOUND)
            return 1;
        *err = zz_from_io();
        if (*err == ZZ_OK)
            *err = ZZ_ENOENT;
        return 0;
    }
    if (!under_root(locked)) {
        UnLock(locked);
        *err = ZZ_EACCES;
        return 0;
    }
    UnLock(locked);
    return 1;
}

static int path_from_wire(const struct zzapp_file_wire *req, char *out, int *err)
{
    uint32_t off = req->path_off;
    const uint8_t *src;
    size_t i;
    if (off < ZZAPP_PATH_OFF || off >= ZZAPP_PATH_OFF + ZZAPP_PATH_BYTES) {
        *err = ZZ_EINVAL;
        return 0;
    }
    win_cache(ZZAPP_PATH_OFF, ZZAPP_PATH_BYTES);
    src = window + off;
    for (i = 0; i < ZZAPP_PATH_MAX && off + i < ZZAPP_PATH_OFF + ZZAPP_PATH_BYTES; i++) {
        out[i] = (char)src[i];
        if (out[i] == 0)
            return 1;
    }
    *err = ZZ_EINVAL;
    return 0;
}

static int open_mode(int32_t flags, LONG *mode, int *err)
{
    if (flags & ~(ZZAPP_FILE_READ | ZZAPP_FILE_WRITE | ZZAPP_FILE_CREATE)) {
        *err = ZZ_EINVAL;
        return 0;
    }
    if (!(flags & (ZZAPP_FILE_READ | ZZAPP_FILE_WRITE | ZZAPP_FILE_CREATE))) {
        *err = ZZ_EINVAL;
        return 0;
    }
    if (flags & ZZAPP_FILE_CREATE)
        *mode = MODE_NEWFILE;
    else if (flags & ZZAPP_FILE_WRITE)
        *mode = MODE_READWRITE;
    else
        *mode = MODE_OLDFILE;
    return 1;
}

static int file_request_is_current(uint32_t request_seq)
{
    uint32_t live_seq;

    if (!window || !generation_owned(file_generation))
        return 0;
    win_cache(ZZAPP_FILE_OFF, 4);
    live_seq = zzapp_get_be32(window + ZZAPP_FILE_OFF);
    return zzrun_file_request_is_current(
        request_seq, file_generation, live_seq,
        life_get(ZZAPP_LIFE_GENERATION_OFF));
}

static int file_copy_data_if_current(uint32_t request_seq, uint32_t at,
                                     const uint8_t *data, uint32_t len)
{
    if (!file_request_is_current(request_seq))
        return 0;
    if (!shared_write_bytes(window + ZZAPP_DATA_OFF + at, data, len,
                            file_generation))
        return 0;
    win_cache(ZZAPP_DATA_OFF + at, len);
    return 1;
}

static void file_reply(uint32_t request_seq, int32_t fd, int32_t err,
                       uint32_t out_n, uint32_t mode, uint64_t out_pos,
                       uint32_t mtime)
{
    uint8_t reply[ZZAPP_FILE_BYTES];
    uint32_t done = request_seq + 2u;

    win_cache(ZZAPP_FILE_OFF, ZZAPP_FILE_BYTES);
    if (!file_request_is_current(request_seq))
        return;
    memset(reply, 0, sizeof(reply));
    zzapp_put_be32(reply + offsetof(struct zzapp_file_wire, seq), request_seq + 1u);
    zzapp_put_be32(reply + offsetof(struct zzapp_file_wire, fd), (uint32_t)fd);
    zzapp_put_be32(reply + offsetof(struct zzapp_file_wire, err), (uint32_t)err);
    zzapp_put_be32(reply + offsetof(struct zzapp_file_wire, out_n), out_n);
    zzapp_put_be32(reply + offsetof(struct zzapp_file_wire, mode), mode);
    zzapp_put_be64(reply + offsetof(struct zzapp_file_wire, out_pos), out_pos);
    zzapp_put_be32(reply + offsetof(struct zzapp_file_wire, mtime), mtime);
    if (!shared_write_bytes(window + ZZAPP_FILE_OFF, reply, sizeof(reply),
                            file_generation))
        return;
    win_cache(ZZAPP_FILE_OFF, ZZAPP_FILE_BYTES);
    if (!shared_write_be32(window + ZZAPP_FILE_OFF +
                           offsetof(struct zzapp_file_wire, seq), done,
                           file_generation))
        return;
    win_cache(ZZAPP_FILE_OFF, 4);
    file_reply_seq = done;
}

static int read_file_req(struct zzapp_file_wire *out)
{
    uint8_t raw[ZZAPP_FILE_BYTES];
    uint32_t s1, s2;
    int tries;
    for (tries = 0; tries < 4; tries++) {
        win_cache(ZZAPP_FILE_OFF, ZZAPP_FILE_BYTES);
        s1 = zzapp_get_be32(window + ZZAPP_FILE_OFF);
        if (s1 & 1u)
            return 0;
        memcpy(raw, window + ZZAPP_FILE_OFF, ZZAPP_FILE_BYTES);
        s2 = zzapp_get_be32(window + ZZAPP_FILE_OFF);
        if (s1 == s2 && (s2 & 1u) == 0) {
            memset(out, 0, sizeof(*out));
            out->seq = s1;
            out->op = zzapp_get_be32(raw + 4);
            out->fd = zzapp_get_be32s(raw + 8);
            out->flags = zzapp_get_be32s(raw + 12);
            out->whence = zzapp_get_be32s(raw + 16);
            out->err = zzapp_get_be32s(raw + 20);
            out->path_off = zzapp_get_be32(raw + 24);
            out->length = zzapp_get_be32(raw + 28);
            out->out_n = zzapp_get_be32(raw + 32);
            out->mode = zzapp_get_be32(raw + 36);
            out->offset = zzapp_get_be64(raw + 40);
            out->out_pos = zzapp_get_be64(raw + 48);
            out->mtime = zzapp_get_be32(raw + 56);
            return 1;
        }
    }
    return 0;
}

static void close_fd_index(int index)
{
    if (index < 0 || index >= ZZAPP_FD_COUNT)
        return;
    if (files[index].used && files[index].fh)
        Close(files[index].fh);
    files[index].fh = 0;
    files[index].used = 0;
}

static void close_list_index(int index)
{
    if (index < 0 || index >= ZZAPP_FD_COUNT)
        return;
    if (lists[index].fib)
        FreeDosObject(DOS_FIB, lists[index].fib);
    if (lists[index].lock)
        UnLock(lists[index].lock);
    lists[index].fib = NULL;
    lists[index].lock = 0;
    lists[index].used = 0;
    lists[index].primed = 0;
}

static void serve_open(const struct zzapp_file_wire *req)
{
    char path[256];
    char leaf[108];
    BPTR dir = 0;
    BPTR fh = 0;
    LONG mode = MODE_OLDFILE;
    LONG examined;
    int err = ZZ_OK;
    int index;
    int absolute = 0;
    int transport_ok;
    struct FileInfoBlock *fib = NULL;
    uint64_t size = 0;
    uint32_t mtime = 0;
    uint32_t mode_out = ZZAPP_MODE_FILE;

    index = slot_index(req->fd, &err);
    if (index < 0)
        goto done;
    if (files[index].used) {
        err = ZZ_EBUSY;
        goto done;
    }
    if (!open_mode(req->flags, &mode, &err))
        goto done;
    if (!path_from_wire(req, path, &err))
        goto done;
    if (path[0] == 0) {
        err = ZZ_EINVAL;
        goto done;
    }
    if (strchr(path, ':')) {
        if (!abs_paths) {
            err = ZZ_EACCES;
            goto done;
        }
        absolute = 1;
    }
    if (!generation_owned(file_generation))
        goto done;
    if (absolute) {
        fh = open_find_async(0, path, mode, &transport_ok);
        if (!transport_ok) {
            err = ZZ_EIO;
            goto done;
        }
    } else {
        dir = resolve_rel(path, leaf, sizeof(leaf), &err);
        if (!dir)
            goto done;
        if (!leaf[0]) {
            err = ZZ_EIO;
            goto done;
        }
        if (!final_path_under_root(dir, leaf,
                                   (req->flags & ZZAPP_FILE_CREATE) != 0,
                                   &err))
            goto done;
        fh = open_find_async(dir, leaf, mode, &transport_ok);
        if (!transport_ok) {
            err = ZZ_EIO;
            goto done;
        }
    }
    if (!fh) {
        err = zz_from_io();
        if (err == ZZ_OK)
            err = ZZ_ENOENT;
        goto done;
    }
    if (!file_request_is_current(req->seq))
        goto done;
    fib = AllocDosObject(DOS_FIB, NULL);
    if (!fib) {
        err = ZZ_ENOMEM;
        goto done;
    }
    examined = file_packet_async(fh, ACTION_EXAMINE_FH, (LONG)MKBADDR(fib),
                                 0, 0, 0, &transport_ok);
    if (!transport_ok) {
        err = ZZ_EIO;
        goto done;
    }
    if (examined != 0) {
        size = (uint32_t)fib->fib_Size;
        mtime = amiga_unix(&fib->fib_Date);
        if (fib->fib_DirEntryType > 0)
            mode_out = ZZAPP_MODE_DIR;
    }
    if (!file_request_is_current(req->seq))
        goto done;
    files[index].fh = fh;
    files[index].used = 1;
    fh = 0;
done:
    if (fib)
        FreeDosObject(DOS_FIB, fib);
    if (fh)
        end_or_close(fh);
    if (dir)
        UnLock(dir);
    file_reply(req->seq, req->fd, err, 0, mode_out, size, mtime);
}

static void serve_close(const struct zzapp_file_wire *req)
{
    int err = ZZ_OK;
    int index = slot_index(req->fd, &err);
    int delivered;
    int closed;

    if (index < 0 || !files[index].used)
        err = ZZ_EBADF;
    else if (files[index].fh) {
        closed = end_async(index, files[index].fh, &delivered);
        if (!closed)
            err = delivered ? zz_from_io() : ZZ_EIO;
    } else {
        files[index].used = 0;
    }
    file_reply(req->seq, req->fd, err, 0, 0, 0, 0);
}

static void serve_read(const struct zzapp_file_wire *req)
{
    int err = ZZ_OK;
    int index = slot_index(req->fd, &err);
    uint32_t left, done = 0;
    if (index < 0 || !files[index].used) {
        file_reply(req->seq, req->fd, ZZ_EBADF, 0, 0, 0, 0);
        return;
    }
    if (req->length > ZZAPP_DATA_BYTES) {
        file_reply(req->seq, req->fd, ZZ_EINVAL, 0, 0, 0, 0);
        return;
    }
    left = req->length;
    while (left) {
        uint32_t chunk = left > ZZRUN_READ_CHUNK ? ZZRUN_READ_CHUNK : left;
        LONG n;
        int transport_ok;

        if (!file_request_is_current(req->seq))
            return;
        if (!generation_owned(file_generation))
            return;
        if (ctrl_c_set())
            break;
        n = file_packet_async(files[index].fh, ACTION_READ,
                              (LONG)file_io_buffer, (LONG)chunk, 0, 0,
                              &transport_ok);
        if (!transport_ok) {
            err = ZZ_EIO;
            break;
        }
        if (n < 0) {
            err = zz_from_io();
            break;
        }
        if (n == 0)
            break;
        if (!file_copy_data_if_current(req->seq, done, file_io_buffer,
                                       (uint32_t)n))
            return;
        done += (uint32_t)n;
        left -= (uint32_t)n;
        if ((uint32_t)n < chunk)
            break;
    }
    file_reply(req->seq, req->fd, err, done, 0, 0, 0);
}

static void serve_write(const struct zzapp_file_wire *req)
{
    int err = ZZ_OK;
    int index = slot_index(req->fd, &err);
    uint32_t left, done = 0;
    if (index < 0 || !files[index].used) {
        file_reply(req->seq, req->fd, ZZ_EBADF, 0, 0, 0, 0);
        return;
    }
    if (req->length > ZZAPP_DATA_BYTES) {
        file_reply(req->seq, req->fd, ZZ_EINVAL, 0, 0, 0, 0);
        return;
    }
    left = req->length;
    while (left) {
        uint32_t chunk = left > ZZRUN_READ_CHUNK ? ZZRUN_READ_CHUNK : left;
        LONG n;
        int transport_ok;

        if (!file_request_is_current(req->seq))
            return;
        win_cache(ZZAPP_DATA_OFF + done, chunk);
        if (!file_request_is_current(req->seq))
            return;
        memcpy(file_io_buffer, window + ZZAPP_DATA_OFF + done, chunk);
        if (!generation_owned(file_generation))
            return;
        n = file_packet_async(files[index].fh, ACTION_WRITE,
                              (LONG)file_io_buffer, (LONG)chunk, 0, 0,
                              &transport_ok);
        if (!transport_ok) {
            err = ZZ_EIO;
            break;
        }
        if (n < 0) {
            err = zz_from_io();
            break;
        }
        if (n == 0) {
            err = ZZ_ENOSPC;
            break;
        }
        if (!file_request_is_current(req->seq))
            return;
        done += (uint32_t)n;
        left -= (uint32_t)n;
        if ((uint32_t)n < chunk)
            break;
    }
    file_reply(req->seq, req->fd, err, done, 0, 0, 0);
}

static void serve_seek(const struct zzapp_file_wire *req)
{
    int err = ZZ_OK;
    int index = slot_index(req->fd, &err);
    LONG mode, pos, current;
    int transport_ok;
    int64_t target;
    if (index < 0 || !files[index].used) {
        file_reply(req->seq, req->fd, ZZ_EBADF, 0, 0, 0, 0);
        return;
    }
    if (req->offset > 0x7fffffffULL && req->whence == ZZAPP_SEEK_SET) {
        file_reply(req->seq, req->fd, ZZ_EINVAL, 0, 0, 0, 0);
        return;
    }
    if (req->whence == ZZAPP_SEEK_SET)
        mode = OFFSET_BEGINNING;
    else if (req->whence == ZZAPP_SEEK_CUR)
        mode = OFFSET_CURRENT;
    else if (req->whence == ZZAPP_SEEK_END)
        mode = OFFSET_END;
    else {
        file_reply(req->seq, req->fd, ZZ_EINVAL, 0, 0, 0, 0);
        return;
    }
    target = (int64_t)req->offset;
    if (req->whence != ZZAPP_SEEK_SET && (req->offset & 0x8000000000000000ULL))
        target = (int64_t)req->offset;
    if (target > 2147483647LL || target < -2147483647LL) {
        file_reply(req->seq, req->fd, ZZ_EINVAL, 0, 0, 0, 0);
        return;
    }
    pos = file_packet_async(files[index].fh, ACTION_SEEK, (LONG)target,
                            mode, 0, 0, &transport_ok);
    if (!transport_ok || pos < 0) {
        file_reply(req->seq, req->fd,
                   transport_ok ? zz_from_io() : ZZ_EIO, 0, 0, 0, 0);
        return;
    }
    current = file_packet_async(files[index].fh, ACTION_SEEK, 0,
                                OFFSET_CURRENT, 0, 0, &transport_ok);
    if (!transport_ok)
        err = ZZ_EIO;
    else if (current < 0)
        err = zz_from_io();
    file_reply(req->seq, req->fd, err, 0, 0,
               err ? 0 : (uint64_t)current, 0);
}

static void serve_stat(const struct zzapp_file_wire *req)
{
    char path[256];
    char leaf[108];
    BPTR dir = 0;
    BPTR locked = 0;
    struct FileInfoBlock *fib = NULL;
    int err = ZZ_OK;
    int transport_ok;
    uint32_t mode = 0;
    uint64_t size = 0;
    uint32_t mtime = 0;

    if (!path_from_wire(req, path, &err))
        goto done;
    if (path[0] == 0) {
        err = ZZ_EINVAL;
        goto done;
    }
    if (!generation_owned(file_generation))
        goto done;
    if (strchr(path, ':')) {
        if (!abs_paths) {
            err = ZZ_EACCES;
            goto done;
        }
        locked = lock_abs_async(path, &transport_ok);
        if (!transport_ok) {
            err = ZZ_EIO;
            goto done;
        }
        if (!locked) {
            err = zz_from_io();
            goto done;
        }
    } else {
        dir = resolve_rel(path, leaf, sizeof(leaf), &err);
        if (!dir)
            goto done;
        if (!lock_target(dir, leaf, &locked, &err))
            goto done;
    }
    fib = AllocDosObject(DOS_FIB, NULL);
    if (!fib) {
        err = ZZ_ENOMEM;
        goto done;
    }
    if (!examine_lock(locked, fib, &transport_ok)) {
        err = transport_ok ? zz_from_io() : ZZ_EIO;
        goto done;
    }
    mode = fib->fib_DirEntryType > 0 ? ZZAPP_MODE_DIR : ZZAPP_MODE_FILE;
    size = (uint32_t)fib->fib_Size;
    mtime = amiga_unix(&fib->fib_Date);
done:
    if (fib)
        FreeDosObject(DOS_FIB, fib);
    if (locked)
        UnLock(locked);
    if (dir)
        UnLock(dir);
    file_reply(req->seq, req->fd, err, 0, mode, size, mtime);
}

static void serve_list_open(const struct zzapp_file_wire *req)
{
    char path[256];
    char leaf[108];
    BPTR dir = 0;
    BPTR locked = 0;
    struct FileInfoBlock *fib = NULL;
    int err = ZZ_OK;
    int index = slot_index(req->fd, &err);
    int transport_ok;

    if (index < 0)
        goto done;
    if (lists[index].used) {
        err = ZZ_EBUSY;
        goto done;
    }
    if (!path_from_wire(req, path, &err))
        goto done;
    if (!generation_owned(file_generation))
        goto done;
    if (strchr(path, ':')) {
        if (!abs_paths) {
            err = ZZ_EACCES;
            goto done;
        }
        locked = lock_abs_async(path, &transport_ok);
        if (!transport_ok) {
            err = ZZ_EIO;
            goto done;
        }
        if (!locked) {
            err = zz_from_io();
            goto done;
        }
    } else {
        if (path[0] == 0) {
            locked = DupLock(root_lock);
        } else {
            dir = resolve_rel(path, leaf, sizeof(leaf), &err);
            if (!dir)
                goto done;
            if (!lock_target(dir, leaf, &locked, &err))
                goto done;
        }
    }
    if (!file_request_is_current(req->seq))
        goto done;
    fib = AllocDosObject(DOS_FIB, NULL);
    if (!fib) {
        err = ZZ_ENOMEM;
        goto done;
    }
    if (!examine_lock(locked, fib, &transport_ok)) {
        err = transport_ok ? zz_from_io() : ZZ_EIO;
        goto done;
    }
    if (fib->fib_DirEntryType < 0) {
        err = ZZ_EIO;
        goto done;
    }
    if (!file_request_is_current(req->seq))
        goto done;
    lists[index].lock = locked;
    lists[index].fib = fib;
    lists[index].used = 1;
    lists[index].primed = 1;
    locked = 0;
    fib = NULL;
done:
    if (fib)
        FreeDosObject(DOS_FIB, fib);
    if (locked)
        UnLock(locked);
    if (dir)
        UnLock(dir);
    file_reply(req->seq, req->fd, err, 0,
               err ? 0 : ZZAPP_MODE_DIR, 0, 0);
}

static void serve_list_next(const struct zzapp_file_wire *req)
{
    int err = ZZ_OK;
    int index = slot_index(req->fd, &err);
    int transport_ok;
    uint32_t mode = 0;
    uint32_t n = 0;
    size_t len;
    if (index < 0 || !lists[index].used) {
        file_reply(req->seq, req->fd, ZZ_EBADF, 0, 0, 0, 0);
        return;
    }
    if (!file_request_is_current(req->seq))
        return;
    if (!generation_owned(file_generation))
        return;
    if (!examine_next_async(lists[index].lock, lists[index].fib,
                            &transport_ok)) {
        err = transport_ok ? zz_from_io() : ZZ_EIO;
        file_reply(req->seq, req->fd, err, 0, 0, 0, 0);
        return;
    }
    len = strlen((const char *)lists[index].fib->fib_FileName);
    if (len > ZZAPP_PATH_MAX)
        len = ZZAPP_PATH_MAX;
    memcpy(file_io_buffer, lists[index].fib->fib_FileName, len);
    file_io_buffer[len] = 0;
    if (!file_copy_data_if_current(req->seq, 0, file_io_buffer,
                                   (uint32_t)len + 1u))
        return;
    n = (uint32_t)len;
    mode = lists[index].fib->fib_DirEntryType > 0 ? ZZAPP_MODE_DIR : ZZAPP_MODE_FILE;
    file_reply(req->seq, req->fd, err, n, mode,
               (uint32_t)lists[index].fib->fib_Size,
               amiga_unix(&lists[index].fib->fib_Date));
}

static void serve_list_close(const struct zzapp_file_wire *req)
{
    int err = ZZ_OK;
    int index = slot_index(req->fd, &err);
    if (index < 0 || !lists[index].used)
        err = ZZ_EBADF;
    else
        close_list_index(index);
    file_reply(req->seq, req->fd, err, 0, 0, 0, 0);
}

static void serve_file_request(const struct zzapp_file_wire *req)
{
    switch (req->op) {
    case ZZAPP_FILE_OP_OPEN: serve_open(req); break;
    case ZZAPP_FILE_OP_CLOSE: serve_close(req); break;
    case ZZAPP_FILE_OP_READ: serve_read(req); break;
    case ZZAPP_FILE_OP_WRITE: serve_write(req); break;
    case ZZAPP_FILE_OP_SEEK: serve_seek(req); break;
    case ZZAPP_FILE_OP_STAT: serve_stat(req); break;
    case ZZAPP_FILE_OP_LIST_OPEN: serve_list_open(req); break;
    case ZZAPP_FILE_OP_LIST_NEXT: serve_list_next(req); break;
    case ZZAPP_FILE_OP_LIST_CLOSE: serve_list_close(req); break;
    default:
        file_reply(req->seq, req->fd, ZZ_ENOSYS, 0, 0, 0, 0);
        break;
    }
}


static void serve_files(void)
{
    struct zzapp_file_wire req;

    if (!window)
        return;
    file_generation = life_get(ZZAPP_LIFE_GENERATION_OFF);
    if (!zzrun_file_service_generation_owned(owned_generation,
                                             file_generation))
        return;
    if (!read_file_req(&req))
        return;
    if (req.seq == 0 || req.seq == file_reply_seq || (req.seq & 1u))
        return;
    serve_file_request(&req);
    (void)heartbeat_owned(file_generation);
}

static void push_event(uint16_t type, uint16_t code, int16_t dx, int16_t dy,
                       uint16_t buttons)
{
    uint32_t head, tail;
    uint32_t generation;
    uint8_t *hdr;
    uint8_t *ev;
    if (!window || !generation_owned(owned_generation))
        return;
    generation = owned_generation;
    win_cache(ZZAPP_INPUT_OFF, ZZAPP_INPUT_HDR);
    hdr = window + ZZAPP_INPUT_OFF;
    head = zzrun_owned_cursor_adopt(&input_head_cursor, generation,
                                    zzapp_get_be32(hdr));
    tail = zzapp_get_be32(hdr + 4);
    if (head - tail >= ZZAPP_INPUT_CAP) {
        (void)life_put_owned(ZZAPP_LIFE_INPUT_DROPS_OFF,
                             life_get(ZZAPP_LIFE_INPUT_DROPS_OFF) + 1u);
        return;
    }
    ev = window + ZZAPP_INPUT_OFF + ZZAPP_INPUT_HDR +
         (head & (ZZAPP_INPUT_CAP - 1u)) * ZZAPP_EVENT_BYTES;
    {
        uint8_t event[ZZAPP_EVENT_BYTES];

        zzapp_put_be16(event + 0, type);
        zzapp_put_be16(event + 2, code);
        zzapp_put_be16(event + 4, (uint16_t)dx);
        zzapp_put_be16(event + 6, (uint16_t)dy);
        zzapp_put_be16(event + 8, buttons);
        zzapp_put_be16(event + 10, 0);
        zzapp_put_be32(event + 12, now_ms());
        if (!shared_write_bytes(ev, event, sizeof(event), generation))
            return;
    }
    win_cache((uint32_t)(ev - window), ZZAPP_EVENT_BYTES);
    head = zzrun_owned_cursor_advance(&input_head_cursor, 1u);
    if (!shared_write_be32(hdr, head, generation))
        return;
    win_cache(ZZAPP_INPUT_OFF, 4);
}

static void request_stop(void)
{
    if (stop_asked)
        return;
    stop_asked = 1;
    stop_ms = now_ms();
    (void)life_put_owned(ZZAPP_LIFE_STOP_OFF, 1);
    if (!stop_event_sent) {
        push_event(ZZAPP_EV_STOP, 0, 0, 0, 0);
        stop_event_sent = 1;
    }
    stop_owned();
}

static uint16_t break_signal_for_rawkey(uint16_t code, uint16_t qualifiers)
{
    if (!(qualifiers & IEQUALIFIER_CONTROL))
        return 0;
    switch (code) {
    case 0x22u: return ZZAPP_BREAK_CTRL_D;
    case 0x12u: return ZZAPP_BREAK_CTRL_E;
    case 0x23u: return ZZAPP_BREAK_CTRL_F;
    default: return 0;
    }
}

static void drain_window(struct Window *win)
{
    struct IntuiMessage *im;
    if (!win || !win->UserPort)
        return;
    while ((im = (struct IntuiMessage *)GetMsg(win->UserPort)) != NULL) {
        ULONG class = im->Class;
        UWORD code = im->Code;
        UWORD qual = im->Qualifier;
        WORD mx = im->MouseX;
        WORD my = im->MouseY;
        ReplyMsg((struct Message *)im);
        if (class & IDCMP_CLOSEWINDOW)
            request_stop();
        if (class & IDCMP_RAWKEY) {
            uint16_t raw = (uint16_t)(code & 0x7f);
            uint16_t type = (code & IECODE_UP_PREFIX) ? ZZAPP_EV_KEY_UP
                                                     : ZZAPP_EV_KEY_DOWN;
            uint16_t signal = type == ZZAPP_EV_KEY_DOWN ?
                break_signal_for_rawkey(raw, qual) : 0;

            if (type == ZZAPP_EV_KEY_DOWN &&
                zzrun_rawkey_requests_stop(raw, qual, IEQUALIFIER_CONTROL)) {
                user_stop = 1;
                request_stop();
            }
            push_event(type, raw, 0, 0, qual);
            if (signal)
                push_event(ZZAPP_EV_BREAK, signal, 0, 0, qual);
        }
        if (class & IDCMP_MOUSEBUTTONS) {
            uint16_t id = 0;
            if (code == SELECTDOWN) { mouse_buttons |= 1u; id = 1; }
            else if (code == SELECTUP) { mouse_buttons &= (uint16_t)~1u; id = 1; }
            else if (code == MENUDOWN) { mouse_buttons |= 2u; id = 2; }
            else if (code == MENUUP) { mouse_buttons &= (uint16_t)~2u; id = 2; }
            else if (code == MIDDLEDOWN) { mouse_buttons |= 4u; id = 3; }
            else if (code == MIDDLEUP) { mouse_buttons &= (uint16_t)~4u; id = 3; }
            push_event(ZZAPP_EV_MOUSE, id, 0, 0, mouse_buttons);
        }
        if (class & IDCMP_MOUSEMOVE) {
            int16_t dx = (int16_t)mx;
            int16_t dy = (int16_t)my;

            if (dx || dy)
                push_event(ZZAPP_EV_MOUSE, 0, dx, dy, mouse_buttons);
        }
    }
}

/*
 * Read-only joystick sampling: JOYxDAT and the CIA-A fire bits.  ZZRun used
 * lowlevel.library's ReadJoyPort(), whose controller auto-sensing drives the
 * port pins and left the left mouse button repeating and releasing on its
 * own after ZZRun had exited.  Reading these registers changes no state.
 * One fire button per port; a mouse in a polled port reads as movement.
 */
#define ZZRUN_JOY0DAT ((volatile UWORD *)0xDFF00AUL)
#define ZZRUN_JOY1DAT ((volatile UWORD *)0xDFF00CUL)
#define ZZRUN_CIAA_PRA ((volatile UBYTE *)0xBFE001UL)

static void poll_joysticks(void)
{
    ULONG port;

    if (!input_win || !(input_win->Flags & WFLG_WINDOWACTIVE))
        return;
    for (port = joy_port0 ? 0 : 1; port < 2; port++) {
        UWORD dat = port ? *ZZRUN_JOY1DAT : *ZZRUN_JOY0DAT;
        int fire = (*ZZRUN_CIAA_PRA & (port ? 0x80u : 0x40u)) == 0;
        ULONG state = ((ULONG)(dat & 0x0303u) << 1) | (ULONG)fire;
        int16_t dx = 0, dy = 0;

        if (joy_valid[port] && state == joy_last[port])
            continue;
        joy_last[port] = state;
        joy_valid[port] = 1;
        if (dat & 0x0200u) dx = -1;
        else if (dat & 0x0002u) dx = 1;
        if (((dat >> 9) ^ (dat >> 8)) & 1u) dy = -1;
        else if (((dat >> 1) ^ dat) & 1u) dy = 1;
        push_event(ZZAPP_EV_JOY, (uint16_t)port, dx, dy, fire ? 1u : 0u);
    }
}

static uint8_t launcher_output[ZZRUN_LAUNCHER_OUTPUT_BYTES];
static uint32_t launcher_output_head;
static uint32_t launcher_output_tail;
static uint32_t launcher_output_used;

static int launcher_output_reserve(uint32_t bytes, int diagnostic)
{
    if (zzrun_launcher_output_accept(launcher_output_used, bytes, diagnostic))
        return 1;
    if (diagnostic)
        launcher_log_dropped++;
    return 0;
}

static void launcher_output_put(uint8_t value)
{
    launcher_output[launcher_output_head] = value;
    launcher_output_head = (launcher_output_head + 1u) %
                           ZZRUN_LAUNCHER_OUTPUT_BYTES;
    launcher_output_used++;
}

static void launcher_output_append(const uint8_t *data, uint32_t bytes,
                                   int diagnostic)
{
    uint32_t i;

    if (!launcher_output_reserve(bytes, diagnostic))
        return;
    for (i = 0; i < bytes; i++)
        launcher_output_put(data[i]);
}

static void launcher_status(const char *msg)
{
    uint32_t bytes = (uint32_t)strlen(msg);
    uint32_t i;

    if (!launcher_output_reserve(bytes + 1u, 0))
        return;
    for (i = 0; i < bytes; i++)
        launcher_output_put((uint8_t)msg[i]);
    launcher_output_put('\n');
}

static void launcher_statusf(const char *format, ...)
{
    char msg[256];
    va_list ap;
    int len;

    va_start(ap, format);
    len = vsnprintf(msg, sizeof(msg), format, ap);
    va_end(ap);
    if (len > 0)
        launcher_output_append((const uint8_t *)msg,
                               (uint32_t)(len < (int)sizeof(msg) ? len :
                                          (int)sizeof(msg) - 1),
                               0);
}

static uint32_t launcher_output_contiguous(void)
{
    uint32_t bytes = ZZRUN_LAUNCHER_OUTPUT_BYTES - launcher_output_tail;

    return launcher_output_used < bytes ? launcher_output_used : bytes;
}

static void launcher_output_consume(uint32_t bytes)
{
    launcher_output_tail = (launcher_output_tail + bytes) %
                           ZZRUN_LAUNCHER_OUTPUT_BYTES;
    launcher_output_used -= bytes;
}

static void print_log(void)
{
    uint32_t head, tail, usable, guard = 0;
    uint32_t generation;
    if (!window || !generation_owned(owned_generation))
        return;
    generation = owned_generation;
    usable = ZZAPP_LOG_BYTES - ZZAPP_LOG_HDR;
    win_cache(ZZAPP_LOG_OFF, 8);
    head = zzapp_get_be32(window + ZZAPP_LOG_OFF);
    tail = zzrun_owned_cursor_adopt(&log_tail_cursor, generation,
                                    zzapp_get_be32(window + ZZAPP_LOG_OFF + 4));
    while (tail != head && guard++ < 64) {
        uint32_t avail = head - tail;
        uint16_t len;
        uint32_t step, i;
        int newline;
        if (avail < 2)
            break;
        win_cache(ZZAPP_LOG_OFF + ZZAPP_LOG_HDR, usable);
        len = (uint16_t)((window[ZZAPP_LOG_OFF + ZZAPP_LOG_HDR + (tail % usable)] << 8) |
                         window[ZZAPP_LOG_OFF + ZZAPP_LOG_HDR + ((tail + 1) % usable)]);
        step = (2u + len + 3u) & ~3u;
        if (step == 0 || step > avail || len > usable) {
            tail = head;
            break;
        }
        newline = len == 0 || window[ZZAPP_LOG_OFF + ZZAPP_LOG_HDR +
                                     ((tail + 1 + len) % usable)] != '\n';
        if (launcher_output_reserve((uint32_t)len + (uint32_t)newline, 1)) {
            for (i = 0; i < len; i++)
                launcher_output_put(window[ZZAPP_LOG_OFF + ZZAPP_LOG_HDR +
                                          ((tail + 2 + i) % usable)]);
            if (newline)
                launcher_output_put('\n');
        }
        tail += step;
    }
    log_tail_cursor.value = tail;
    if (!shared_write_be32(window + ZZAPP_LOG_OFF + 4, log_tail_cursor.value,
                           generation))
        return;
    win_cache(ZZAPP_LOG_OFF + 4, 4);
}


static BPTR console_output(void)
{
    return console_handle ? console_handle : Output();
}

static BPTR console_input(void)
{
    return console_handle ? console_handle : Input();
}

static int console_open_if_needed(void)
{
    if (!_WBenchMsg || console_handle)
        return 1;
    console_handle = Open((CONST_STRPTR)"CON:0/0/640/160/ZZRun/CLOSE/WAIT",
                          MODE_OLDFILE);
    if (!console_handle) {
        if (!console_failed) {
            tell("ZZRun: cannot open the requested Workbench console.");
            console_failed = 1;
        }
        return 0;
    }
    return 1;
}
#define ZZRUN_CONSOLE_READ_BYTES 256
#define ZZRUN_CONSOLE_WRITE_BYTES 1024

static struct MsgPort *con_port;
static struct StandardPacket *con_packet;
static UBYTE *con_buffer;
static BPTR con_fh;
static int con_pending;
static int con_interactive;
static uint32_t con_have;
static uint32_t con_offset;
static int con_eof;
static struct StandardPacket *con_write_packet;
static UBYTE *con_write_buffer;
static int con_write_pending;
static int con_write_interactive;
static uint32_t con_write_bytes;
static int con_write_source;


static void console_reap_packets(void);
static int console_write_start(BPTR fh, const uint8_t *data, uint32_t bytes,
                               int source);
static void console_write_fh(BPTR fh, const char *msg);

static void console_output_error(void)
{
    request_stop();
    if (!console_output_failed)
        console_write_fh(console_output(),
                         "ZZRun: cannot write app console output.\n");
    console_output_failed = 1;
}

/* The app owns console head; ZZRun owns console tail. Read the tail only
 * when adopting a generation. A Zorro read immediately after our tail store
 * may return the previous value, so it must never choose the next range. */
static uint32_t console_output_tail(uint8_t *hdr)
{
    return zzrun_owned_cursor_adopt(
        &console_output_tail_cursor, life_get(ZZAPP_LIFE_GENERATION_OFF),
        zzapp_get_be32(hdr + 4));
}

static void console_drain_output(void)
{
    uint8_t *hdr = NULL;
    uint32_t head = 0, tail = 0, available = 0, at, chunk;
    int source;

    console_reap_packets();
    if (console_output_failed || con_write_pending)
        return;
    if (window && generation_owned(owned_generation) &&
        (life_get(ZZAPP_LIFE_FLAGS_OFF) & ZZAPP_F_CONSOLE)) {
        hdr = window + ZZAPP_CONSOLE_OUT_OFF;
        win_cache(ZZAPP_CONSOLE_OUT_OFF, ZZAPP_CONSOLE_HDR);
        head = zzapp_get_be32(hdr);
        tail = console_output_tail(hdr);
        available = zzrun_console_output_available(head, tail);
        if (available > ZZAPP_CONSOLE_OUT_CAP) {
            if (!generation_owned(owned_generation))
                return;
            console_output_tail_cursor.value = head;
            if (!shared_write_be32(hdr + 4, head, owned_generation))
                return;
            win_cache(ZZAPP_CONSOLE_OUT_OFF + 4, 4);
            available = 0;
        }
    }
    source = zzrun_output_next_source(launcher_output_used, available);
    if (source == ZZRUN_OUTPUT_NONE)
        return;
    if (hdr && !console_open_if_needed()) {
        console_output_error();
        return;
    }
    if (source == ZZRUN_OUTPUT_LAUNCHER) {
        chunk = launcher_output_contiguous();
        if (chunk > ZZRUN_CONSOLE_WRITE_BYTES)
            chunk = ZZRUN_CONSOLE_WRITE_BYTES;
        if (!console_write_start(console_output(),
                                 launcher_output + launcher_output_tail,
                                 chunk, source))
            console_output_error();
        return;
    }
    at = tail % ZZAPP_CONSOLE_OUT_CAP;
    chunk = ZZAPP_CONSOLE_OUT_CAP - at;
    if (chunk > available)
        chunk = available;
    if (chunk > ZZRUN_CONSOLE_WRITE_BYTES)
        chunk = ZZRUN_CONSOLE_WRITE_BYTES;
    win_cache(ZZAPP_CONSOLE_OUT_OFF + ZZAPP_CONSOLE_HDR + at, chunk);
    if (!console_write_start(console_output(),
                             window + ZZAPP_CONSOLE_OUT_OFF +
                                 ZZAPP_CONSOLE_HDR + at,
                             chunk, source))
        console_output_error();
}

static int console_output_pending(void)
{
    uint8_t *hdr;
    uint32_t head, tail;

    if (con_write_pending || launcher_output_used)
        return 1;
    if (!window || !(life_get(ZZAPP_LIFE_FLAGS_OFF) & ZZAPP_F_CONSOLE))
        return 0;
    hdr = window + ZZAPP_CONSOLE_OUT_OFF;
    win_cache(ZZAPP_CONSOLE_OUT_OFF, ZZAPP_CONSOLE_HDR);
    head = zzapp_get_be32(hdr);
    tail = console_output_tail(hdr);
    return head - tail <= ZZAPP_CONSOLE_OUT_CAP && head != tail;
}


/*
 * Input stays asynchronous for every handle. A keyboard read uses the
 * console's normal line mode, so the Shell keeps echo, line editing and
 * Ctrl-C/D/E/F. A pipe or file uses the same ACTION_READ: a blocking Read
 * would stop heartbeat, display and audio while the writer stays open.
 * A pending read returns a line on Return and 0 bytes on Ctrl-\. It is
 * started only while the owned app is RUNNING. Keyboard reads also require
 * WANT, sampled after the ring. Synchronous DOS calls keep using
 * pr_MsgPort, so their replies never meet this packet.
 */

static int console_read_start(BPTR fh)
{
    struct FileHandle *handle = (struct FileHandle *)BADDR(fh);

    if (!handle || !handle->fh_Type)
        return 0;
    if (!con_port)
        con_port = CreateMsgPort();
    if (!con_packet)
        con_packet = AllocMem(sizeof(*con_packet), MEMF_PUBLIC | MEMF_CLEAR);
    if (!con_buffer)
        con_buffer = AllocMem(ZZRUN_CONSOLE_READ_BYTES, MEMF_PUBLIC);
    if (!con_port || !con_packet || !con_buffer)
        return 0;
    memset(con_packet, 0, sizeof(*con_packet));
    con_packet->sp_Msg.mn_Node.ln_Name = (char *)&con_packet->sp_Pkt;
    con_packet->sp_Pkt.dp_Link = &con_packet->sp_Msg;
    con_packet->sp_Pkt.dp_Port = con_port;
    con_packet->sp_Pkt.dp_Type = ACTION_READ;
    con_packet->sp_Pkt.dp_Arg1 = handle->fh_Arg1;
    con_packet->sp_Pkt.dp_Arg2 = (LONG)con_buffer;
    con_packet->sp_Pkt.dp_Arg3 = ZZRUN_CONSOLE_READ_BYTES;
    con_fh = fh;
    con_pending = 1;
    PutMsg(handle->fh_Type, &con_packet->sp_Msg);
    return 1;
}

static void console_write_fh(BPTR fh, const char *msg)
{
    if (!fh || !msg || !msg[0])
        return;
    (void)Write(fh, (APTR)msg, (LONG)strlen(msg));
}

/* The user's console, never redirected stdout and never the pipe. */
static void console_user_note(const char *msg)
{
    BPTR fh;

    if (console_handle) {
        console_write_fh(console_handle, msg);
        return;
    }
    fh = Open((CONST_STRPTR)"CONSOLE:", MODE_OLDFILE);
    if (fh) {
        console_write_fh(fh, msg);
        Close(fh);
        return;
    }
    if (_WBenchMsg && IntuitionBase) {
        struct EasyStruct es;

        es.es_StructSize = sizeof(es);
        es.es_Flags = 0;
        es.es_Title = (UBYTE *)"ZZRun";
        es.es_TextFormat = (UBYTE *)msg;
        es.es_GadgetFormat = (UBYTE *)"OK";
        EasyRequestArgs(NULL, &es, NULL, NULL);
    }
}

static int console_write_start(BPTR fh, const uint8_t *data, uint32_t bytes,
                               int source)
{
    struct FileHandle *handle = (struct FileHandle *)BADDR(fh);

    if (!handle || !handle->fh_Type || !data || bytes == 0 ||
        bytes > ZZRUN_CONSOLE_WRITE_BYTES)
        return 0;
    if (!con_port)
        con_port = CreateMsgPort();
    if (!con_write_packet)
        con_write_packet = AllocMem(sizeof(*con_write_packet),
                                    MEMF_PUBLIC | MEMF_CLEAR);
    if (!con_write_buffer)
        con_write_buffer = AllocMem(ZZRUN_CONSOLE_WRITE_BYTES, MEMF_PUBLIC);
    if (!con_port || !con_write_packet || !con_write_buffer)
        return 0;
    memcpy(con_write_buffer, data, bytes);
    memset(con_write_packet, 0, sizeof(*con_write_packet));
    con_write_packet->sp_Msg.mn_Node.ln_Name =
        (char *)&con_write_packet->sp_Pkt;
    con_write_packet->sp_Pkt.dp_Link = &con_write_packet->sp_Msg;
    con_write_packet->sp_Pkt.dp_Port = con_port;
    con_write_packet->sp_Pkt.dp_Type = ACTION_WRITE;
    con_write_packet->sp_Pkt.dp_Arg1 = handle->fh_Arg1;
    con_write_packet->sp_Pkt.dp_Arg2 = (LONG)con_write_buffer;
    con_write_packet->sp_Pkt.dp_Arg3 = (LONG)bytes;
    con_write_bytes = bytes;
    con_write_source = source;
    con_write_interactive = IsInteractive(fh);
    con_write_pending = 1;
    PutMsg(handle->fh_Type, &con_write_packet->sp_Msg);
    return 1;
}

static void console_write_complete(void)
{
    LONG written = con_write_packet->sp_Pkt.dp_Res1;
    uint32_t advanced = zzrun_console_write_advance(
        con_write_bytes, (int32_t)written);

    con_write_pending = 0;
    if (con_write_source == ZZRUN_OUTPUT_LAUNCHER) {
        if (advanced)
            launcher_output_consume(advanced);
    } else if (advanced && window && generation_owned(owned_generation)) {
        uint8_t *hdr = window + ZZAPP_CONSOLE_OUT_OFF;
        uint32_t tail = zzrun_owned_cursor_advance(
            &console_output_tail_cursor, advanced);

        if (!generation_owned(owned_generation))
            return;
        if (!shared_write_be32(hdr + 4, tail, owned_generation))
            return;
        win_cache(ZZAPP_CONSOLE_OUT_OFF + 4, 4);
    }
    if (zzrun_console_write_failed(con_write_bytes, (int32_t)written))
        console_output_error();
}

static void console_read_complete(void)
{
    LONG got = con_packet->sp_Pkt.dp_Res1;

    con_pending = 0;
    if (got <= 0)
        con_eof = 1;
    else {
        con_have = (uint32_t)got;
        con_offset = 0;
    }
}

static void console_reap_packets(void)
{
    struct Message *msg;

    if (!con_port)
        return;
    while ((msg = GetMsg(con_port)) != NULL) {
        if (con_pending && con_packet && msg == &con_packet->sp_Msg)
            console_read_complete();
        else if (con_write_pending && con_write_packet &&
                 msg == &con_write_packet->sp_Msg)
            console_write_complete();
    }
}
static uint32_t console_input_head(uint8_t *hdr)
{
    return zzrun_owned_cursor_adopt(
        &console_input_head_cursor, life_get(ZZAPP_LIFE_GENERATION_OFF),
        zzapp_get_be32(hdr));
}


/* Head and tail are loaded before WANT. The barrier stops the compiler
 * from moving the WANT load ahead of that observation. */
static int console_keyboard_demand(uint8_t *hdr)
{
    uint32_t head = console_input_head(hdr);
    uint32_t tail = zzapp_get_be32(hdr + 4);
    uint32_t want;

    __asm__ volatile("" ::: "memory");
    want = zzapp_get_be32(hdr + ZZAPP_CONSOLE_WANT_OFF);
    return zzrun_keyboard_read_allowed(life_get(ZZAPP_LIFE_STATE_OFF),
                                       owns_current_generation(), stop_asked,
                                       head, tail, want, 0, 0);
}

/* Pending console packets and queued launcher messages must finish before
 * their port and storage are released. Neither pipes nor the console handler
 * can cancel a pending write. */
static void console_read_finish(void)
{
    int input_noted = 0;
    int output_noted = 0;

    while (con_pending || con_write_pending ||
           (!console_output_failed && launcher_output_used)) {
        console_reap_packets();
        console_drain_output();
        if (!con_pending && !con_write_pending &&
            (console_output_failed || !launcher_output_used))
            break;
        if (con_pending && !input_noted) {
            int kind = zzrun_finish_prompt(1, con_interactive);

            if (kind == ZZRUN_FINISH_RETURN_ON_READ_HANDLE)
                console_write_fh(con_fh ? con_fh : console_input(),
                                 "ZZRun: press Return to finish.\n");
            else
                console_user_note(
                    "ZZRun: waiting for pending input to finish.\n");
            input_noted = 1;
        }
        if (con_write_pending && !con_write_interactive && !output_noted) {
            console_user_note(
                "ZZRun: waiting for pending output to finish.\n");
            output_noted = 1;
        }
        if (con_port)
            WaitPort(con_port);
    }
    if (con_write_buffer)
        FreeMem(con_write_buffer, ZZRUN_CONSOLE_WRITE_BYTES);
    if (con_write_packet)
        FreeMem(con_write_packet, sizeof(*con_write_packet));
    if (con_buffer)
        FreeMem(con_buffer, ZZRUN_CONSOLE_READ_BYTES);
    if (con_packet)
        FreeMem(con_packet, sizeof(*con_packet));
    if (con_port)
        DeleteMsgPort(con_port);
    con_write_buffer = NULL;
    con_write_packet = NULL;
    con_write_pending = 0;
    con_write_interactive = 0;
    con_write_bytes = 0;
    con_write_source = ZZRUN_OUTPUT_NONE;
    memset(&console_output_tail_cursor, 0, sizeof(console_output_tail_cursor));
    memset(&console_input_head_cursor, 0, sizeof(console_input_head_cursor));
    con_buffer = NULL;
    con_packet = NULL;
    con_port = NULL;
    con_fh = 0;
    con_have = 0;
    con_offset = 0;
    con_eof = 0;
    con_interactive = 0;
}

/* Copies bytes into the stdin ring; returns how many fitted. */
static uint32_t console_push(uint8_t *hdr, const uint8_t *data, uint32_t n)
{
    uint32_t head = console_input_head(hdr);
    uint32_t tail = zzapp_get_be32(hdr + 4);
    uint32_t free_bytes = ZZAPP_CONSOLE_IN_CAP - (head - tail);
    uint32_t done = 0;

    if (!generation_owned(owned_generation) ||
        head - tail > ZZAPP_CONSOLE_IN_CAP)
        return 0;
    while (done < n && free_bytes) {
        uint32_t at = head % ZZAPP_CONSOLE_IN_CAP;
        uint32_t chunk = ZZAPP_CONSOLE_IN_CAP - at;
        if (chunk > free_bytes)
            chunk = free_bytes;
        if (chunk > n - done)
            chunk = n - done;
        if (!shared_write_bytes(
                window + ZZAPP_CONSOLE_IN_OFF + ZZAPP_CONSOLE_HDR + at,
                data + done, chunk, owned_generation))
            return 0;
        win_cache(ZZAPP_CONSOLE_IN_OFF + ZZAPP_CONSOLE_HDR + at, chunk);
        head += chunk;
        done += chunk;
        free_bytes -= chunk;
    }
    if (!generation_owned(owned_generation))
        return 0;
    head = zzrun_owned_cursor_advance(&console_input_head_cursor, done);
    if (!shared_write_be32(hdr, head, owned_generation))
        return 0;
    win_cache(ZZAPP_CONSOLE_IN_OFF, 4);
    return done;
}

static void console_set_eof(uint8_t *hdr)
{
    if (!generation_owned(owned_generation))
        return;
    if (!shared_write_be32(hdr + ZZAPP_CONSOLE_EOF_OFF, 1u, owned_generation))
        return;
    win_cache(ZZAPP_CONSOLE_IN_OFF + ZZAPP_CONSOLE_EOF_OFF, 4);
}

static void console_fill_async(uint8_t *hdr, int keyboard)
{
    uint32_t head, tail;

    console_reap_packets();
    if (con_pending)
        return;
    if (con_have) {
        uint32_t sent = console_push(hdr, con_buffer + con_offset, con_have);

        con_offset += sent;
        con_have -= sent;
        if (con_have)
            return;
    }
    if (con_eof) {
        console_set_eof(hdr);
        return;
    }
    if (keyboard) {
        if (!console_keyboard_demand(hdr))
            return;
    } else {
        head = console_input_head(hdr);
        tail = zzapp_get_be32(hdr + 4);
        if (!zzrun_async_read_allowed(life_get(ZZAPP_LIFE_STATE_OFF),
                                      owns_current_generation(), stop_asked,
                                      head, tail, ZZAPP_CONSOLE_IN_CAP, 0, 0))
            return;
    }
    /* Forced unwind can leave WANT set after the app is no longer running. */
    if (stop_asked || !owns_current_generation() ||
        life_get(ZZAPP_LIFE_STATE_OFF) != ZZAPP_ST_RUNNING)
        return;
    if (!console_read_start(console_input()))
        console_set_eof(hdr);
    else
        con_interactive = keyboard;
}

static void console_fill_input(void)
{
    uint8_t *hdr;

    if (!window || !generation_owned(owned_generation) ||
        !(life_get(ZZAPP_LIFE_FLAGS_OFF) & ZZAPP_F_CONSOLE))
        return;
    if (!console_open_if_needed())
        return;
    hdr = window + ZZAPP_CONSOLE_IN_OFF;
    win_cache(ZZAPP_CONSOLE_IN_OFF, ZZAPP_CONSOLE_HDR);
    if (zzapp_get_be32(hdr + ZZAPP_CONSOLE_EOF_OFF))
        return;
    console_fill_async(hdr, IsInteractive(console_input()));
}

/* Abort in-flight writes and drop their byte counts. Callers that are
 * retiring a live stream use ahi_reap instead, which publishes the tail. */
static void ahi_abort_pending(void)
{
    unsigned i;

    for (i = 0u; i < ZZRUN_AHI_BUFFER_COUNT; i++) {
        if (ahi_request[i] && ahi_pending[i]) {
            if (!CheckIO((struct IORequest *)ahi_request[i]))
                AbortIO((struct IORequest *)ahi_request[i]);
            WaitIO((struct IORequest *)ahi_request[i]);
        }
        ahi_pending[i] = 0u;
        ahi_pending_bytes[i] = 0u;
    }
    if (ahi_port) {
        while (GetMsg(ahi_port))
            ;
    }
}

static void ahi_close(void)
{
    unsigned i;

    ahi_abort_pending();
    if (ahi_device_open == 0 && ahi_request[0])
        CloseDevice((struct IORequest *)ahi_request[0]);
    ahi_device_open = -1;
    if (ahi_request[1]) {
        FreeVec(ahi_request[1]);
        ahi_request[1] = NULL;
    }
    if (ahi_request[0]) {
        DeleteIORequest((struct IORequest *)ahi_request[0]);
        ahi_request[0] = NULL;
    }
    for (i = 0u; i < ZZRUN_AHI_BUFFER_COUNT; i++) {
        if (ahi_buffer[i]) {
            FreeVec(ahi_buffer[i]);
            ahi_buffer[i] = NULL;
        }
    }
    if (ahi_port) {
        DeleteMsgPort(ahi_port);
        ahi_port = NULL;
    }
    AHIBase = NULL;
    ahi_rate = 0u;
    ahi_channels = 0u;
    ahi_stream_epoch = 0u;
    ahi_stream_epoch_valid = 0;
    memset(&ahi_tail_cursor, 0, sizeof(ahi_tail_cursor));
}

static int ahi_open(uint32_t rate, uint32_t channels)
{
    unsigned i;

    if (channels != 2u || rate == 0u)
        return 0;
    ahi_port = CreateMsgPort();
    if (!ahi_port)
        return 0;
    ahi_request[0] = (struct AHIRequest *)CreateIORequest(
        ahi_port, sizeof(*ahi_request[0]));
    if (!ahi_request[0])
        goto failed;
    ahi_request[0]->ahir_Version = 4;
    /* CMD_WRITE needs a real unit; AHI_NO_UNIT only opens the library
     * interface and has no unit to queue the request on. */
    ahi_device_open = OpenDevice((CONST_STRPTR)AHINAME, AHI_DEFAULT_UNIT,
                                 (struct IORequest *)ahi_request[0], 0);
    if (ahi_device_open)
        goto failed;
    AHIBase = (struct Library *)ahi_request[0]->ahir_Std.io_Device;
    ahi_request[1] = AllocVec(sizeof(*ahi_request[1]), MEMF_PUBLIC);
    if (!ahi_request[1])
        goto failed;
    CopyMem(ahi_request[0], ahi_request[1], sizeof(*ahi_request[1]));
    for (i = 0u; i < ZZRUN_AHI_BUFFER_COUNT; i++) {
        ahi_buffer[i] = AllocVec(ZZRUN_AHI_BUFFER_BYTES,
                                 MEMF_PUBLIC | MEMF_CLEAR);
        if (!ahi_buffer[i])
            goto failed;
    }
    ahi_rate = rate;
    ahi_channels = channels;
    ahi_underruns = 0u;
    return 1;
failed:
    ahi_close();
    return 0;
}

static int ahi_free_slot(void)
{
    unsigned i;

    for (i = 0u; i < ZZRUN_AHI_BUFFER_COUNT; i++) {
        if (!ahi_pending[i])
            return (int)i;
    }
    return -1;
}

static int ahi_reap(uint32_t *tail)
{
    struct Message *message;
    unsigned i;
    int completed = 0;

    while ((message = GetMsg(ahi_port)) != NULL) {
        for (i = 0u; i < ZZRUN_AHI_BUFFER_COUNT; i++) {
            if (ahi_request[i] &&
                message == &ahi_request[i]->ahir_Std.io_Message) {
                if (ahi_pending[i]) {
                    *tail += ahi_pending_bytes[i];
                    ahi_pending_bytes[i] = 0u;
                    ahi_pending[i] = 0u;
                    completed = 1;
                }
                break;
            }
        }
    }
    return completed;
}

static void ahi_submit(unsigned slot, uint32_t bytes)
{
    struct AHIRequest *request = ahi_request[slot];
    struct AHIRequest *link = NULL;
    unsigned i;

    for (i = 0u; i < ZZRUN_AHI_BUFFER_COUNT; i++) {
        if (ahi_pending[i]) {
            link = ahi_request[i];
            break;
        }
    }
    request->ahir_Std.io_Command = CMD_WRITE;
    request->ahir_Std.io_Length = bytes;
    request->ahir_Std.io_Data = ahi_buffer[slot];
    if (!link)
        request->ahir_Std.io_Offset = 0;
    request->ahir_Type = AHIST_S16S;
    request->ahir_Frequency = ahi_rate;
    request->ahir_Volume = 0x10000L;
    request->ahir_Position = 0x8000L;
    request->ahir_Link = link;
    ahi_pending_bytes[slot] = bytes;
    ahi_pending[slot] = 1u;
    SendIO((struct IORequest *)request);
}

static void service_ahi_audio(void)
{
    uint8_t *hdr;
    uint32_t head, tail, staged, available, contiguous, bytes, frames, at, i;
    uint32_t epoch, rate, channels;
    int completed;
    int slot;

    if (!window || !generation_owned(owned_generation))
        return;
    hdr = window + ZZAPP_AUDIO_OFF;
    win_cache(ZZAPP_AUDIO_OFF, ZZAPP_AUDIO_HDR);
    if (!(life_get(ZZAPP_LIFE_FLAGS_OFF) & ZZAPP_F_LAUNCHER_AUDIO_AHI) ||
        zzapp_get_be32(hdr + ZZAPP_AUDIO_ROUTE_OFF) != ZZAPP_AUDIO_ROUTE_AHI) {
        ahi_close();
        return;
    }
    epoch = zzapp_get_be32(hdr + ZZAPP_AUDIO_EPOCH_OFF);
    /* Firmware writes the epoch last on open. Zero means the rest of the
     * header may still belong to the previous stream. */
    if (epoch == 0u)
        return;
    rate = zzapp_get_be32(hdr + ZZAPP_AUDIO_RATE_OFF);
    channels = zzapp_get_be32(hdr + ZZAPP_AUDIO_CHANNELS_OFF);
    if (zzrun_audio_epoch_replaced(ahi_stream_epoch_valid, ahi_stream_epoch,
                                   epoch)) {
        /* A close and reopen can both land between polls. Old request
         * completions must not be added to the new stream's tail. */
        if (ahi_request[0] && (rate != ahi_rate || channels != ahi_channels))
            ahi_close();
        else
            ahi_abort_pending();
        ahi_underruns = 0u;
        ahi_stream_epoch_valid = 0;
    }
    if (audio_open_failed)
        return;
    if (!ahi_request[0] && !ahi_open(rate, channels)) {
        launcher_status("ZZRun: cannot open AHI for AUDIO=AHI.");
        audio_open_failed = 1;
        request_stop();
        return;
    }
    ahi_stream_epoch = epoch;
    ahi_stream_epoch_valid = 1;
    head = zzapp_get_be32(hdr + ZZAPP_AUDIO_HEAD_OFF);
    tail = zzrun_owned_cursor_adopt(&ahi_tail_cursor, epoch,
                                    zzapp_get_be32(hdr + ZZAPP_AUDIO_TAIL_OFF));
    if (head - tail > ZZAPP_AUDIO_CAP)
        return;
    completed = ahi_reap(&tail);
    if (completed) {
        if (!generation_owned(owned_generation))
            return;
        ahi_tail_cursor.value = tail;
        if (!shared_write_be32(hdr + ZZAPP_AUDIO_TAIL_OFF, tail,
                               owned_generation))
            return;
        win_cache(ZZAPP_AUDIO_OFF + ZZAPP_AUDIO_TAIL_OFF, 4);
    }
    /* Bytes already handed to AHI stay in the ring until their request
     * returns, so the next buffer starts after them, not at the tail. */
    staged = tail;
    for (i = 0u; i < ZZRUN_AHI_BUFFER_COUNT; i++)
        staged += ahi_pending_bytes[i];
    if (head - staged > ZZAPP_AUDIO_CAP)
        return;
    available = head - staged;
    contiguous = ZZAPP_AUDIO_CAP - (staged % ZZAPP_AUDIO_CAP);
    bytes = available < contiguous ? available : contiguous;
    frames = ahi_rate / 50u;
    if (frames == 0u)
        return;
    if (bytes > frames * ahi_channels * 2u)
        bytes = frames * ahi_channels * 2u;
    if (bytes > ZZRUN_AHI_BUFFER_BYTES)
        bytes = ZZRUN_AHI_BUFFER_BYTES;
    bytes -= bytes % (ahi_channels * 2u);
    if (bytes == 0u) {
        /* An underrun: a buffer just finished and AHI has nothing queued. */
        if (completed && staged == tail) {
            if (!generation_owned(owned_generation))
                return;
            ahi_underruns++;
            if (!shared_write_be32(hdr + ZZAPP_AUDIO_UNDERRUNS_OFF,
                                   ahi_underruns, owned_generation))
                return;
            (void)life_put_owned(ZZAPP_LIFE_AUDIO_UNDERRUNS_OFF,
                                 ahi_underruns);
            win_cache(ZZAPP_AUDIO_OFF + ZZAPP_AUDIO_UNDERRUNS_OFF, 4);
        }
        return;
    }
    slot = ahi_free_slot();
    if (slot < 0)
        return;
    at = staged % ZZAPP_AUDIO_CAP;
    win_cache(ZZAPP_AUDIO_OFF + ZZAPP_AUDIO_HDR + at, bytes);
    for (i = 0u; i < bytes; i += 2u) {
        ahi_buffer[slot][i] =
            window[ZZAPP_AUDIO_OFF + ZZAPP_AUDIO_HDR + at + i + 1u];
        ahi_buffer[slot][i + 1u] =
            window[ZZAPP_AUDIO_OFF + ZZAPP_AUDIO_HDR + at + i];
    }
    ahi_submit((unsigned)slot, bytes);
}

static void service_console(void)
{
    console_drain_output();
    console_fill_input();
}

static int bitmap_arm(struct BitMap *bm, ULONG *arm, ULONG *pitch)
{
    APTR mem;
    ULONG addr, bytes;
    if (!bm)
        return 0;
    mem = (APTR)p96GetBitMapAttr(bm, P96BMA_MEMORY);
    if (!mem)
        mem = bm->Planes[0];
    addr = (ULONG)mem;
    if (addr < board.address + ZZ_FRAMEBUFFER_BOARD_OFFSET)
        return 0;
    *arm = (addr - (board.address + ZZ_FRAMEBUFFER_BOARD_OFFSET)) + ZZRUN_VRAM_ARM;
    if (*arm < ZZRUN_VRAM_ARM || *arm >= ZZRUN_HEAP_END)
        return 0;
    bytes = (ULONG)p96GetBitMapAttr(bm, P96BMA_BYTESPERROW);
    if (!bytes)
        bytes = bm->BytesPerRow;
    if (!bytes)
        return 0;
    *pitch = bytes;
    return 1;
}

static void load_palette(void)
{
    ULONG colors[770];
    uint32_t generation, first, count, i;

    if (!screen || bound_fmt != ZZ_FMT_CLUT8)
        return;
    generation = life_get(ZZAPP_LIFE_PALETTE_GEN_OFF);
    if (generation == palette_generation)
        return;
    first = life_get(ZZAPP_LIFE_PALETTE_FIRST_OFF);
    count = life_get(ZZAPP_LIFE_PALETTE_COUNT_OFF);
    if (count == 0 || first >= 256 || count > 256 - first)
        return;
    win_cache(ZZAPP_PALETTE_OFF + first * 4u, count * 4u);
    /* LoadRGB32: (count << 16) | first, then R, G, B per colour, each
     * left-justified in 32 bits, then a 0 terminator. */
    colors[0] = (count << 16) | first;
    for (i = 0; i < count; i++) {
        uint32_t rgb = zzapp_get_be32(window + ZZAPP_PALETTE_OFF +
                                      (first + i) * 4u);

        colors[1 + i * 3u] = ((rgb >> 16) & 0xffu) * 0x01010101u;
        colors[2 + i * 3u] = ((rgb >> 8) & 0xffu) * 0x01010101u;
        colors[3 + i * 3u] = (rgb & 0xffu) * 0x01010101u;
    }
    colors[1 + count * 3u] = 0;
    LoadRGB32(&screen->ViewPort, colors);
    palette_generation = generation;
}

/* Only SafeMessage has a reply port: DisplayMessage is not used. Every
 * successful ChangeScreenBuffer contributes one reply before teardown. */
static void flip_reap_safe_messages(void)
{
    while (flip_safe_port && GetMsg(flip_safe_port))
        flip_safe_pending = zzrun_screen_safe_completed(flip_safe_pending);
}

static void flip_wait_safe_messages(void)
{
    while (flip_safe_pending != 0) {
        WaitPort(flip_safe_port);
        flip_reap_safe_messages();
    }
}

static void close_display(void)
{
    if (have_two && buf_front && buf_back && screen && flip_safe_port) {
        /* A prior app flip may still own either buffer. */
        flip_wait_safe_messages();
        flip_pending = 0;
        if (showing && ChangeScreenBuffer(screen, buf_front)) {
            flip_safe_pending = zzrun_screen_safe_started(flip_safe_pending);
            showing = 0;
            /* The restoring change has its own SafeMessage. */
            flip_wait_safe_messages();
        }
    }
    if (input_win) {
        CloseWindow(input_win);
        input_win = NULL;
    }
    if (buf_back && screen)
        FreeScreenBuffer(screen, buf_back);
    if (buf_front && screen)
        FreeScreenBuffer(screen, buf_front);
    if (flip_safe_port) {
        DeleteMsgPort(flip_safe_port);
        flip_safe_port = NULL;
    }
    flip_safe_pending = 0;
    buf_back = NULL;
    buf_front = NULL;
    if (screen) {
        p96CloseScreen(screen);
        screen = NULL;
    }
    /* AllocScreenBuffer owns both double-buffer bitmaps. */
    have_two = 0;
    have_pan = 0;
    pan_count = 0;
    showing = 0;
    if (pub_name[0]) {
        struct Screen *pub = LockPubScreen((CONST_STRPTR)pub_name);
        if (pub) {
            ScreenToFront(pub);
            UnlockPubScreen(NULL, pub);
        }
    }
}

static ULONG mode_for(uint32_t w, uint32_t h, uint32_t format, ULONG forced)
{
    ULONG depth = format == ZZ_FMT_BGRA8888 ? 32 :
                  (format == ZZ_FMT_CLUT8 ? 8 : 16);
    ULONG formats = format == ZZ_FMT_BGRA8888 ? RGBFF_B8G8R8A8 :
                    (format == ZZ_FMT_CLUT8 ? RGBFF_CLUT : RGBFF_R5G6B5);
    if (forced)
        return forced;
    return p96BestModeIDTags(
        P96BIDTAG_NominalWidth, w,
        P96BIDTAG_NominalHeight, h,
        P96BIDTAG_Depth, depth,
        P96BIDTAG_FormatsAllowed, formats,
        TAG_DONE);
}

static struct Screen *open_p96_screen(ULONG mode, uint32_t w, uint32_t h,
                                      ULONG depth, ULONG rgb)
{
    struct Screen *s = p96OpenScreenTags(
        P96SA_DisplayID, mode,
        P96SA_Width, w,
        P96SA_Height, h,
        P96SA_Depth, depth,
        P96SA_RGBFormat, rgb,
        P96SA_Quiet, TRUE,
        P96SA_ShowTitle, FALSE,
        P96SA_AutoScroll, FALSE,
        P96SA_Title, (ULONG)"ZZRun",
        TAG_DONE);

    if (s && (ULONG)p96GetBitMapAttr(s->RastPort.BitMap, P96BMA_RGBFORMAT) != rgb) {
        p96CloseScreen(s);
        s = NULL;
    }
    return s;
}

static int open_display(uint32_t w, uint32_t h, uint32_t format, ULONG forced_mode)
{
    ULONG mode, depth, rgb, pitch = 0, front = 0, back = 0, spare = 0;
    uint8_t payload[ZZAPP_BIND_EXT_REQ_BYTES];
    ZZ9KMailboxEntry reply;
    int status;

    if (w == 0 || h == 0 || w > ZZRUN_SCREEN_MAX || h > ZZRUN_SCREEN_MAX)
        return 0;
    if (format != ZZ_FMT_RGB565 && format != ZZ_FMT_BGRA8888 &&
        format != ZZ_FMT_CLUT8)
        return 0;
    if (!P96Base || !IntuitionBase || !GfxBase)
        return 0;
    if (!pub_name[0])
        GetDefaultPubScreen((STRPTR)pub_name);
    depth = format == ZZ_FMT_BGRA8888 ? 32 : (format == ZZ_FMT_CLUT8 ? 8 : 16);
    rgb = format == ZZ_FMT_BGRA8888 ? RGBFB_B8G8R8A8 :
          (format == ZZ_FMT_CLUT8 ? RGBFB_CLUT : RGBFB_R5G6B5);
    mode = mode_for(w, h, format, forced_mode);
    if (mode == (ULONG)INVALID_ID)
        return 0;
    if (!heartbeat_owned(owned_generation))
        return 0;
    screen = open_p96_screen(mode, w, h, depth, rgb);
    if (!screen)
        return 0;
    if (!bitmap_arm(screen->RastPort.BitMap, &front, &pitch)) {
        launcher_status("ZZRun: display unavailable (front bitmap address)");
        close_display();
        return 0;
    }
    buf_front = AllocScreenBuffer(screen, NULL, SB_SCREEN_BITMAP);
    if (!buf_front) {
        launcher_status("ZZRun: double buffering unavailable (front screen buffer)");
    } else {
        buf_back = AllocScreenBuffer(screen, NULL, 0);
        if (!buf_back) {
            launcher_status("ZZRun: double buffering unavailable (back screen buffer)");
        } else {
            ULONG back_pitch = 0;

            if (!bitmap_arm(buf_back->sb_BitMap, &back, &back_pitch)) {
                launcher_status("ZZRun: double buffering unavailable (back bitmap address)");
            } else if (back_pitch != pitch) {
                launcher_status("ZZRun: double buffering unavailable (back bitmap pitch)");
            } else {
                flip_safe_port = CreateMsgPort();
                if (!flip_safe_port) {
                    launcher_status("ZZRun: double buffering unavailable (safe message port)");
                } else if (!buf_front->sb_DBufInfo || !buf_back->sb_DBufInfo) {
                    launcher_status("ZZRun: double buffering unavailable (screen buffer info)");
                } else {
                    buf_front->sb_DBufInfo->dbi_SafeMessage.mn_ReplyPort =
                        flip_safe_port;
                    buf_back->sb_DBufInfo->dbi_SafeMessage.mn_ReplyPort =
                        flip_safe_port;
                    have_two = 1;
                }
            }
        }
    }
    if (!have_two) {
        struct Screen *tall = NULL;
        ULONG tall_front = 0, tall_pitch = 0;

        if (flip_safe_port) {
            DeleteMsgPort(flip_safe_port);
            flip_safe_port = NULL;
        }
        if (buf_back && screen)
            FreeScreenBuffer(screen, buf_back);
        if (buf_front && screen)
            FreeScreenBuffer(screen, buf_front);
        buf_back = NULL;
        buf_front = NULL;
        back = 0;
        if (h <= ZZRUN_SCREEN_MAX / 3u)
            tall = open_p96_screen(mode, w, h * 3u, depth, rgb);
        if (tall && tall->Height >= (WORD)(h * 3u) &&
            bitmap_arm(tall->RastPort.BitMap, &tall_front, &tall_pitch)) {
            p96CloseScreen(screen);
            screen = tall;
            front = tall_front;
            pitch = tall_pitch;
            back = front + pitch * h;
            spare = back + pitch * h;
            have_pan = 1;
            pan_count = 3;
            launcher_status("ZZRun: triple buffering by panning");
        } else {
            if (tall)
                p96CloseScreen(tall);
            tall = NULL;
            if (h <= ZZRUN_SCREEN_MAX / 2u)
                tall = open_p96_screen(mode, w, h * 2u, depth, rgb);
            if (tall && tall->Height >= (WORD)(h * 2u) &&
                bitmap_arm(tall->RastPort.BitMap, &tall_front, &tall_pitch)) {
                p96CloseScreen(screen);
                screen = tall;
                front = tall_front;
                pitch = tall_pitch;
                back = front + pitch * h;
                have_pan = 1;
                pan_count = 2;
                launcher_status("ZZRun: double buffering by panning");
            } else {
                if (tall)
                    p96CloseScreen(tall);
                launcher_status("ZZRun: single buffer; the firmware copies each frame");
            }
        }
    }
    input_win = OpenWindowTags(NULL,
        WA_CustomScreen, (ULONG)screen,
        WA_Left, 0, WA_Top, 0,
        WA_Width, screen->Width,
        WA_Height, screen->Height,
        WA_Borderless, TRUE,
        WA_Backdrop, TRUE,
        WA_RMBTrap, TRUE,
        WA_ReportMouse, TRUE,
        WA_Activate, TRUE,
        WA_NoCareRefresh, TRUE,
        WA_IDCMP, IDCMP_RAWKEY | IDCMP_MOUSEMOVE | IDCMP_DELTAMOVE |
                  IDCMP_MOUSEBUTTONS | IDCMP_CLOSEWINDOW,
        TAG_DONE);
    if (!input_win) {
        close_display();
        return 0;
    }
    card_front = front;
    card_back = back;
    bound_w = w;
    bound_h = h;
    bound_pitch = pitch;
    bound_fmt = format;
    if (!life_put_owned(ZZAPP_LIFE_CARD_FRONT_OFF, front) ||
        !life_put_owned(ZZAPP_LIFE_CARD_BACK_OFF, back) ||
        !life_put_owned(ZZAPP_LIFE_CARD_SPARE_OFF, spare)) {
        close_display();
        return 0;
    }
    zzapp_build_display_bind_request_triple(payload, w, h, pitch, format, front,
                                            back, format == ZZ_FMT_BGRA8888 ?
                                            ZZ_LAYOUT_BGRA8888 :
                                            (format == ZZ_FMT_CLUT8 ? ZZ_LAYOUT_CLUT8 :
                                             ZZ_LAYOUT_RGB565_BE), spare);
    status = mailbox(ZZAPP_OP_DISPLAY_BIND, payload, spare ?
                     ZZAPP_BIND_EXT_REQ_BYTES : ZZAPP_BIND_REQ_BYTES, &reply);
    if (status != ZZ9K_STATUS_OK) {
        launcher_statusf("ZZRun: display bind failed (%s)\n", status_name(status));
        close_display();
        return 0;
    }
    if (life_get(ZZAPP_LIFE_FLAGS_OFF) & ZZAPP_F_DISPLAY_UNBOUND) {
        launcher_status("ZZRun: display request stayed set after bind");
        close_display();
        return 0;
    }
    showing = 0;
    flip_pending = 0;
    flip_safe_pending = 0;
    scanout_submitted = 0;
    scanout_fallback = 0;
    palette_generation = 0;
    return 1;
}

static void ack_flip(void)
{
    uint32_t gen, flags;

    if (!window || !screen || !generation_owned(owned_generation))
        return;
    if (flip_pending != 0) {
        if (!flip_safe_port || !GetMsg(flip_safe_port))
            return;
        flip_safe_pending = zzrun_screen_safe_completed(flip_safe_pending);
        (void)life_put_owned(ZZAPP_LIFE_FLIP_ACK_OFF, flip_pending);
        flip_acked = flip_pending;
        flip_pending = 0;
        return;
    }
    if (scanout_submitted != 0) {
        /* Submitted by address: the firmware acknowledges at the card's
         * vblank.  If it never does (the latched scanout address did not
         * match), fall back to the Amiga vblank for the rest of the run
         * instead of leaving the app waiting. */
        if (life_get(ZZAPP_LIFE_FLIP_ACK_OFF) == scanout_submitted) {
            flip_acked = scanout_submitted;
            scanout_submitted = 0;
        } else if ((uint32_t)(now_ms() - scanout_submit_ms) <
                   ZZRUN_SCANOUT_ACK_MS) {
            return;
        } else {
            if (!scanout_fallback)
                launcher_status("ZZRun: no card vblank acknowledgement; using WaitTOF");
            scanout_fallback = 1;
            WaitTOF();
            (void)life_put_owned(ZZAPP_LIFE_FLIP_ACK_OFF, scanout_submitted);
            flip_acked = scanout_submitted;
            scanout_submitted = 0;
        }
    }
    gen = life_get(ZZAPP_LIFE_FLIP_GEN_OFF);
    if (gen == flip_acked)
        return;
    flags = life_get(ZZAPP_LIFE_FLAGS_OFF);
    if (have_two && buf_front && buf_back) {
        int next_showing = !showing;

        if (!ChangeScreenBuffer(screen,
                                next_showing ? buf_back : buf_front))
            return;
        showing = next_showing;
        flip_pending = gen;
        flip_safe_pending = zzrun_screen_safe_started(flip_safe_pending);
        return;
    }
    /* In negotiated mailbox mode core 0 owns every per-frame VDMA choice.
     * Keep P96's initial app page as the visibility guard; do not mirror
     * RyOffset or race the card with a 68k pan. */
    if (have_pan && (flags & ZZAPP_F_MAILBOX))
        return;
    if (have_pan) {
        if (flags & ZZAPP_F_SCANOUT_ACK) {
            uint32_t address = life_get(ZZAPP_LIFE_FLIP_ADDRESS_OFF);
            uint32_t page_bytes;
            uint32_t page;

            if (bound_h == 0 || bound_pitch > 0xffffffffUL / bound_h) {
                launcher_status("ZZRun: invalid panning geometry");
                request_stop();
                return;
            }
            page_bytes = bound_pitch * bound_h;
            if (page_bytes == 0 || address < card_front ||
                (address - card_front) % page_bytes != 0) {
                launcher_status("ZZRun: invalid scanout flip address");
                request_stop();
                return;
            }
            page = (address - card_front) / page_bytes;
            if (page >= (uint32_t)pan_count) {
                launcher_status("ZZRun: scanout flip address is outside the screen");
                request_stop();
                return;
            }
            screen->ViewPort.RasInfo->RyOffset = (WORD)(page * bound_h);
            ScrollVPort(&screen->ViewPort);
            showing = (int)page;
            /* The firmware acknowledges after its own next vblank has
             * latched this address, which keeps the old front unavailable
             * until scanout has stopped reading it. */
            if (scanout_fallback) {
                WaitTOF();
                (void)life_put_owned(ZZAPP_LIFE_FLIP_ACK_OFF, gen);
                flip_acked = gen;
                return;
            }
            (void)life_put_owned(ZZAPP_LIFE_FLIP_SUBMIT_OFF, gen);
            scanout_submitted = gen;
            scanout_submit_ms = now_ms();
            return;
        } else {
            int next_showing = (showing + 1) % pan_count;

            screen->ViewPort.RasInfo->RyOffset = (WORD)(next_showing * bound_h);
            ScrollVPort(&screen->ViewPort);
            WaitTOF();
            showing = next_showing;
            (void)life_put_owned(ZZAPP_LIFE_FLIP_ACK_OFF, gen);
            flip_acked = gen;
            return;
        }
    }
    if (flags & ZZAPP_F_VSYNC)
        WaitTOF();
    (void)life_put_owned(ZZAPP_LIFE_FLIP_ACK_OFF, gen);
    flip_acked = gen;
}

/* argv[0] is the app's file name, as C programs expect; the Shell or ARGS
 * tokens follow it. */
static int fill_argv(void)
{
    uint8_t argv_data[ZZAPP_ARGV_BYTES];
    uint32_t at = ZZAPP_ARGV_STR_OFF;
    const char *name = (const char *)FilePart((STRPTR)elf_path);
    uint32_t n;
    int i;

    memset(argv_data, 0, sizeof(argv_data));
    zzapp_put_be32(argv_data + ZZAPP_ARGV_ARGC_OFF, (uint32_t)app_argc + 1u);
    n = (uint32_t)strlen(name);
    if (n > 127u)
        n = 127u;
    zzapp_put_be32(argv_data + ZZAPP_ARGV_OFFS_OFF, ZZAPP_ARGV_OFF + at);
    memcpy(argv_data + at, name, n);
    argv_data[at + n] = 0;
    at += n + 1u;
    for (i = 0; i < app_argc; i++) {
        n = (uint32_t)strlen(arg_store[i]);
        if (n > 127 || at + n + 1u > ZZAPP_ARGV_BYTES)
            return 0;
        zzapp_put_be32(argv_data + ZZAPP_ARGV_OFFS_OFF + (uint32_t)(i + 1) * 4u,
                       ZZAPP_ARGV_OFF + at);
        memcpy(argv_data + at, arg_store[i], n + 1u);
        at += n + 1u;
    }
    if (!shared_write_bytes(window + ZZAPP_ARGV_OFF, argv_data, sizeof(argv_data),
                            owned_generation))
        return 0;
    win_cache(ZZAPP_ARGV_OFF, ZZAPP_ARGV_BYTES);
    return 1;
}

/*
 * A DOS packet may block on slow media. ZZRun waits on a reply port it owns
 * together with its lease timer and Ctrl-C, renewing the heartbeat while the
 * captured generation is still its own. Only the reply ends the wait: packet
 * and buffers stay alive until then, even after Ctrl-C or a lost generation.
 */
static int dos_packet_async(struct MsgPort *handler, LONG action, LONG arg1,
                            LONG arg2, LONG arg3, LONG arg4, LONG arg5,
                            uint32_t captured_generation, LONG *result_out,
                            int *ownership_lost, int *abandoned_out)
{
    struct MsgPort *port;
    struct StandardPacket *packet;
    struct Message *message;
    ULONG mask;
    ULONG signals;
    int abandoned = 0;

    if (result_out)
        *result_out = -1;
    if (ownership_lost)
        *ownership_lost = 0;
    if (abandoned_out)
        *abandoned_out = 0;
    if (!handler || !result_out)
        return 0;
    port = CreateMsgPort();
    packet = AllocMem(sizeof(*packet), MEMF_PUBLIC | MEMF_CLEAR);
    if (!port || !packet) {
        if (packet)
            FreeMem(packet, sizeof(*packet));
        if (port)
            DeleteMsgPort(port);
        SetIoErr(zzrun_dos_error_code(0, 0, ERROR_NO_FREE_STORE));
        return 0;
    }
    packet->sp_Msg.mn_Node.ln_Name = (char *)&packet->sp_Pkt;
    packet->sp_Pkt.dp_Link = &packet->sp_Msg;
    packet->sp_Pkt.dp_Port = port;
    packet->sp_Pkt.dp_Type = action;
    packet->sp_Pkt.dp_Arg1 = arg1;
    packet->sp_Pkt.dp_Arg2 = arg2;
    packet->sp_Pkt.dp_Arg3 = arg3;
    packet->sp_Pkt.dp_Arg4 = arg4;
    packet->sp_Pkt.dp_Arg5 = arg5;
    PutMsg(handler, &packet->sp_Msg);
    timer_reap();
    timer_arm();
    mask = (1UL << port->mp_SigBit) | SIGBREAKF_CTRL_C;
    if (timer_port)
        mask |= 1UL << timer_port->mp_SigBit;
    for (;;) {
        signals = Wait(mask);
        if ((signals & SIGBREAKF_CTRL_C) || ctrl_c_set()) {
            SetSignal(0, SIGBREAKF_CTRL_C);
            user_stop = 1;
            if (zzrun_ctrl_c_must_request_stop(1))
                request_stop();
            abandoned = zzrun_packet_abandoned(1, 1);
        }
        if (timer_port && (signals & (1UL << timer_port->mp_SigBit))) {
            timer_reap();
            if (captured_generation != 0 &&
                zzrun_packet_abandoned(0,
                    heartbeat_owned(captured_generation))) {
                abandoned = 1;
                if (ownership_lost)
                    *ownership_lost = 1;
            }
            if (!abandoned)
                timer_arm();
        }
        message = GetMsg(port);
        if (!zzrun_packet_keep_waiting(message != NULL))
            break;
    }
    timer_reap();
    if (captured_generation != 0 && !generation_owned(captured_generation)) {
        abandoned = 1;
        if (ownership_lost)
            *ownership_lost = 1;
    }
    SetIoErr(zzrun_dos_error_code(1, packet->sp_Pkt.dp_Res2,
                                  ERROR_NO_FREE_STORE));
    *result_out = packet->sp_Pkt.dp_Res1;
    if (abandoned_out)
        *abandoned_out = abandoned;
    FreeMem(packet, sizeof(*packet));
    DeleteMsgPort(port);
    return 1;
}

static LONG file_packet_async(BPTR fh, LONG action, LONG arg2, LONG arg3,
                              LONG arg4, LONG arg5, int *transport_ok)
{
    struct FileHandle *handle = (struct FileHandle *)BADDR(fh);
    LONG result;
    int abandoned;

    if (transport_ok)
        *transport_ok = 0;
    if (!handle || !handle->fh_Type || !transport_ok)
        return -1;
    if (!dos_packet_async(handle->fh_Type, action, handle->fh_Arg1, arg2,
                          arg3, arg4, arg5, file_generation, &result, NULL,
                          &abandoned) ||
        !zzrun_packet_result_usable(1, abandoned))
        return -1;
    *transport_ok = 1;
    return result;
}

static int examine_lock_async(BPTR lock, struct FileInfoBlock *fib,
                              int *transport_ok)
{
    struct FileLock *file_lock = (struct FileLock *)BADDR(lock);
    LONG result;
    int abandoned;

    if (transport_ok)
        *transport_ok = 0;
    if (!file_lock || !file_lock->fl_Task || !fib || !transport_ok)
        return 0;
    if (!dos_packet_async(file_lock->fl_Task, ACTION_EXAMINE_OBJECT,
                          (LONG)lock, (LONG)MKBADDR(fib), 0, 0, 0,
                          file_generation, &result, NULL, &abandoned) ||
        !zzrun_packet_result_usable(1, abandoned))
        return 0;
    *transport_ok = 1;
    return zzrun_dos_boolean_result(1, result);
}

static int examine_next_async(BPTR lock, struct FileInfoBlock *fib,
                              int *transport_ok)
{
    struct FileLock *file_lock = (struct FileLock *)BADDR(lock);
    LONG result;
    int abandoned;

    if (transport_ok)
        *transport_ok = 0;
    if (!file_lock || !file_lock->fl_Task || !fib || !transport_ok)
        return 0;
    if (!dos_packet_async(file_lock->fl_Task, ACTION_EXAMINE_NEXT,
                          (LONG)lock, (LONG)MKBADDR(fib), 0, 0, 0,
                          file_generation, &result, NULL, &abandoned) ||
        !zzrun_packet_result_usable(1, abandoned))
        return 0;
    *transport_ok = 1;
    return zzrun_dos_boolean_result(1, result);
}

static LONG load_read_async(BPTR fh, uint8_t *buffer, LONG bytes,
                            int *ownership_lost)
{
    struct FileHandle *handle = (struct FileHandle *)BADDR(fh);
    LONG result;
    int abandoned;

    if (ownership_lost)
        *ownership_lost = 0;
    if (!handle || !handle->fh_Type || !ownership_lost)
        return -1;
    if (!dos_packet_async(handle->fh_Type, ACTION_READ, handle->fh_Arg1,
                          (LONG)buffer, bytes, 0, 0, owned_generation,
                          &result, ownership_lost, &abandoned) ||
        !zzrun_packet_result_usable(1, abandoned))
        return -1;
    return result;
}

/* DOS packet names must remain in public, longword-aligned storage until the
 * handler replies. */
static uint8_t *bstr_storage_alloc(void)
{
    return AllocVec(258u, MEMF_PUBLIC);
}

static uint8_t *bstr_alloc(const char *name)
{
    uint8_t *buf;

    if (!name)
        return NULL;
    buf = bstr_storage_alloc();
    if (!buf)
        return NULL;
    if (zzrun_bstr_encode(name, buf, 258u) == 0u) {
        FreeVec(buf);
        SetIoErr(ERROR_OBJECT_TOO_LARGE);
        return NULL;
    }
    return buf;
}

struct lock_release {
    struct MsgPort *handler;
    BPTR lock;
};

static int send_free_lock(void *context, int32_t *result)
{
    struct lock_release *release = context;
    LONG packet_result = 0;
    int delivered;

    if (!release->handler || !release->lock)
        return 0;
    delivered = dos_packet_async(release->handler, ACTION_FREE_LOCK,
                                 (LONG)release->lock, 0, 0, 0, 0, 0,
                                 &packet_result, NULL, NULL);
    if (result)
        *result = packet_result;
    return delivered;
}

static void fallback_unlock(void *context)
{
    struct lock_release *release = context;

    UnLock(release->lock);
}

static void release_abandoned_lock(struct MsgPort *handler, BPTR lock)
{
    struct lock_release release;
    struct zzrun_dos_release_ops ops;

    release.handler = handler;
    release.lock = lock;
    ops.context = &release;
    ops.send_release = send_free_lock;
    ops.free_local = NULL;
    ops.fallback_release = fallback_unlock;
    ops.forget_slot = NULL;
    zzrun_dos_release_abandoned(&ops, lock != 0);
}

static BPTR lock_on_handler_async(struct MsgPort *handler, BPTR dir,
                                  BPTR name_bstr, LONG access,
                                  int *completed_out)
{
    LONG result;
    int abandoned;

    if (completed_out)
        *completed_out = 0;
    if (!handler || !name_bstr || !completed_out)
        return 0;
    if (!dos_packet_async(handler, ACTION_LOCATE_OBJECT, (LONG)dir,
                          (LONG)name_bstr, access, 0, 0,
                          file_generation, &result, NULL, &abandoned))
        return 0;
    if (abandoned) {
        release_abandoned_lock(handler, (BPTR)result);
        return 0;
    }
    if (!zzrun_packet_result_usable(1, abandoned))
        return 0;
    *completed_out = 1;
    return (BPTR)result;
}

static BPTR lock_async(BPTR dir, const char *name, LONG access,
                       int *transport_ok)
{
    struct FileLock *file_lock = (struct FileLock *)BADDR(dir);
    uint8_t *storage;
    BPTR lock;
    int completed;

    if (transport_ok)
        *transport_ok = 0;
    if (!file_lock || !file_lock->fl_Task || !transport_ok)
        return 0;
    storage = bstr_alloc(name);
    if (!storage) {
        SetIoErr(zzrun_dos_error_code(0, 0, ERROR_OBJECT_TOO_LARGE));
        return 0;
    }
    lock = lock_on_handler_async(file_lock->fl_Task, dir, MKBADDR(storage),
                                 access, &completed);
    FreeVec(storage);
    if (completed)
        *transport_ok = 1;
    return lock;
}

struct dos_path_resolver {
    const char *name;
    struct DevProc *devproc;
    struct MsgPort *handler;
    BPTR directory;
    int fixed_used;
};

static int resolve_dos_path(void *context, int advance,
                            struct zzrun_dos_path_candidate *candidate)
{
    struct dos_path_resolver *resolver = context;

    if (resolver->handler) {
        if (advance || resolver->fixed_used)
            return 0;
        resolver->fixed_used = 1;
        candidate->handler = (uintptr_t)resolver->handler;
        candidate->directory = (uint32_t)resolver->directory;
        return 1;
    }
    resolver->devproc = GetDeviceProc((STRPTR)resolver->name,
                                      advance ? resolver->devproc : NULL);
    if (!resolver->devproc)
        return 0;
    candidate->handler = (uintptr_t)resolver->devproc->dvp_Port;
    candidate->directory = (uint32_t)resolver->devproc->dvp_Lock;
    return 1;
}

static void release_dos_path_resolver(struct dos_path_resolver *resolver)
{
    if (resolver->devproc)
        FreeDeviceProc(resolver->devproc);
}

struct lock_path_request {
    LONG access;
    BPTR lock;
    LONG error;
};

static void send_lock_path_packet(
    void *context, const struct zzrun_dos_path_candidate *candidate,
    const uint8_t *name_bstr, uint32_t name_bstr_bytes,
    uint32_t name_bstr_bptr, struct zzrun_dos_path_reply *reply)
{
    struct lock_path_request *request = context;
    int completed;

    (void)name_bstr;
    (void)name_bstr_bytes;
    request->lock = lock_on_handler_async((struct MsgPort *)candidate->handler,
                                          (BPTR)candidate->directory,
                                          (BPTR)name_bstr_bptr,
                                          request->access, &completed);
    reply->completed = completed;
    reply->abandoned = !completed;
    reply->result = (int32_t)request->lock;
    request->error = IoErr();
    reply->error = request->error;
}

/* Absolute (ABS tooltype) paths use every GetDeviceProc candidate, including
 * late-binding, nonbinding, and multi-directory assigns. */
static BPTR lock_abs_async(const char *name, int *transport_ok)
{
    struct dos_path_resolver resolver;
    struct lock_path_request request;
    uint8_t *name_bstr;
    int usable;

    if (transport_ok)
        *transport_ok = 0;
    if (!name || !transport_ok)
        return 0;
    name_bstr = bstr_storage_alloc();
    if (!name_bstr) {
        SetIoErr(ERROR_NO_FREE_STORE);
        return 0;
    }
    memset(&resolver, 0, sizeof(resolver));
    memset(&request, 0, sizeof(request));
    resolver.name = name;
    request.access = ACCESS_READ;
    (void)zzrun_dos_dispatch_path(name, name_bstr, 258u,
                                  ERROR_OBJECT_NOT_FOUND, resolve_dos_path,
                                  &resolver, send_lock_path_packet, &request,
                                  NULL, &usable);
    release_dos_path_resolver(&resolver);
    FreeVec(name_bstr);
    if (usable)
        *transport_ok = 1;
    if (!request.lock && usable)
        SetIoErr(request.error);
    return request.lock;
}

static BPTR open_on_handler_async(struct MsgPort *handler, BPTR dir,
                                  BPTR name_bstr, LONG action,
                                  int *completed_out)
{
    struct FileHandle *handle;
    LONG result;
    int abandoned;

    if (completed_out)
        *completed_out = 0;
    if (!handler || !name_bstr || !completed_out)
        return 0;
    handle = AllocDosObject(DOS_FILEHANDLE, NULL);
    if (!handle) {
        SetIoErr(ERROR_NO_FREE_STORE);
        return 0;
    }
    handle->fh_Type = handler;
    handle->fh_Arg1 = 0;
    if (!dos_packet_async(handler, action, MKBADDR(handle), (LONG)dir,
                          (LONG)name_bstr, 0, 0,
                          file_generation, &result, NULL, &abandoned)) {
        FreeDosObject(DOS_FILEHANDLE, handle);
        return 0;
    }
    if (abandoned) {
        if (result != 0)
            end_or_close(MKBADDR(handle));
        else
            FreeDosObject(DOS_FILEHANDLE, handle);
        return 0;
    }
    if (!zzrun_packet_result_usable(1, abandoned)) {
        FreeDosObject(DOS_FILEHANDLE, handle);
        return 0;
    }
    *completed_out = 1;
    if (result == 0) {
        FreeDosObject(DOS_FILEHANDLE, handle);
        return 0;
    }
    return MKBADDR(handle);
}


struct open_path_request {
    LONG action;
    BPTR fh;
    LONG error;
};

static void send_open_path_packet(
    void *context, const struct zzrun_dos_path_candidate *candidate,
    const uint8_t *name_bstr, uint32_t name_bstr_bytes,
    uint32_t name_bstr_bptr, struct zzrun_dos_path_reply *reply)
{
    struct open_path_request *request = context;
    int completed;

    (void)name_bstr;
    (void)name_bstr_bytes;
    request->fh = open_on_handler_async((struct MsgPort *)candidate->handler,
                                        (BPTR)candidate->directory,
                                        (BPTR)name_bstr_bptr,
                                        request->action, &completed);
    reply->completed = completed;
    reply->abandoned = !completed;
    reply->result = (int32_t)request->fh;
    request->error = IoErr();
    reply->error = request->error;
}
static BPTR open_find_async(BPTR dir, const char *name, LONG mode,
                            int *transport_ok)
{
    struct FileLock *file_lock = (struct FileLock *)BADDR(dir);
    struct dos_path_resolver resolver;
    struct open_path_request request;
    uint8_t *name_bstr;
    int usable;

    if (transport_ok)
        *transport_ok = 0;
    if (!name || !transport_ok)
        return 0;
    name_bstr = bstr_storage_alloc();
    if (!name_bstr) {
        SetIoErr(ERROR_NO_FREE_STORE);
        return 0;
    }
    memset(&resolver, 0, sizeof(resolver));
    if (dir) {
        if (!file_lock || !file_lock->fl_Task) {
            FreeVec(name_bstr);
            return 0;
        }
        resolver.handler = file_lock->fl_Task;
        resolver.directory = dir;
    } else {
        resolver.name = name;
    }
    memset(&request, 0, sizeof(request));
    if (mode == MODE_NEWFILE)
        request.action = ACTION_FINDOUTPUT;
    else if (mode == MODE_READWRITE)
        request.action = ACTION_FINDUPDATE;
    else
        request.action = ACTION_FINDINPUT;
    (void)zzrun_dos_dispatch_path(name, name_bstr, 258u,
                                  ERROR_OBJECT_NOT_FOUND, resolve_dos_path,
                                  &resolver, send_open_path_packet, &request,
                                  NULL, &usable);
    release_dos_path_resolver(&resolver);
    FreeVec(name_bstr);
    if (usable)
        *transport_ok = 1;
    if (!request.fh && usable)
        SetIoErr(request.error);
    return request.fh;
}

struct file_handle_release {
    BPTR fh;
    int slot;
};

static int send_file_end(void *context, int32_t *result)
{
    struct file_handle_release *release = context;
    struct FileHandle *handle = (struct FileHandle *)BADDR(release->fh);
    LONG packet_result = 0;
    int abandoned;
    int delivered;

    if (!handle || !handle->fh_Type)
        return 0;
    delivered = dos_packet_async(handle->fh_Type, ACTION_END, handle->fh_Arg1,
                                 0, 0, 0, 0, file_generation, &packet_result,
                                 NULL, &abandoned);
    if (result)
        *result = zzrun_packet_result_usable(delivered, abandoned) ?
                  packet_result : 0;
    return delivered;
}

static void free_file_handle(void *context)
{
    struct file_handle_release *release = context;

    FreeDosObject(DOS_FILEHANDLE, BADDR(release->fh));
}

static void forget_file_slot(void *context)
{
    struct file_handle_release *release = context;

    if (release->slot >= 0 && release->slot < ZZAPP_FD_COUNT) {
        files[release->slot].fh = 0;
        files[release->slot].used = 0;
    }
}

static void close_file_handle(void *context)
{
    struct file_handle_release *release = context;

    Close(release->fh);
}

static struct zzrun_dos_release_ops file_handle_release_ops(
    struct file_handle_release *release)
{
    struct zzrun_dos_release_ops ops;

    ops.context = release;
    ops.send_release = send_file_end;
    ops.free_local = free_file_handle;
    ops.fallback_release = close_file_handle;
    ops.forget_slot = release->slot >= 0 ? forget_file_slot : NULL;
    return ops;
}

static int end_async(int slot, BPTR fh, int *delivered_out)
{
    struct file_handle_release release;
    struct zzrun_dos_release_ops ops;

    release.fh = fh;
    release.slot = slot;
    ops = file_handle_release_ops(&release);
    return zzrun_dos_close_slot(&ops, delivered_out);
}

static void end_or_close(BPTR fh)
{
    struct file_handle_release release;
    struct zzrun_dos_release_ops ops;

    release.fh = fh;
    release.slot = -1;
    ops = file_handle_release_ops(&release);
    zzrun_dos_end_or_close(&ops);
}

static int load_elf(BPTR fh, LONG size)
{
    uint8_t payload[ZZAPP_CHUNK_REQ_BYTES];
    uint8_t begin_payload[ZZRUN_LOAD_BEGIN_BYTES];
    uint16_t begin_len;
    ZZ9KMailboxEntry reply;
    uint32_t load_slot;
    uint32_t load_generation;
    LONG offset = 0;
    int ownership_lost;
    int status;
    int hostmem_decision = zzrun_hostmem_decision(runtime_caps, hostmem_mib);
    int hostmem_failure;

    if (hostmem_decision == ZZRUN_HOSTMEM_REJECT) {
        tell("ZZRun: HOSTMEM requires firmware with host-visible limits.");
        return 0;
    }
    begin_len = zzrun_load_begin_payload(
        begin_payload, maxmem_mib != 0u, maxmem_mib,
        hostmem_decision == ZZRUN_HOSTMEM_SEND, hostmem_mib);
    status = mailbox_long(ZZAPP_OP_LOAD_BEGIN, begin_len ? begin_payload : NULL,
                          begin_len, &reply);
    hostmem_failure = zzrun_hostmem_failure(hostmem_mib, status);
    if (hostmem_failure == ZZRUN_HOSTMEM_FAIL_TOO_LARGE) {
        printf("ZZRun: HOSTMEM too large (%lu MiB, the firmware grants at most %lu MiB)\n",
               (unsigned long)hostmem_mib,
               (unsigned long)ZZRUN_HOSTMEM_CEILING_MIB);
        stop_owned();
        return 0;
    }
    if (hostmem_failure == ZZRUN_HOSTMEM_FAIL_NO_MEMORY) {
        printf("ZZRun: not enough card memory to load (HOSTMEM %lu MiB may be more than the firmware can grant)\n",
               (unsigned long)hostmem_mib);
        stop_owned();
        return 0;
    }
    if (status != ZZ9K_STATUS_OK) {
        printf("ZZRun: load begin failed (%s)\n", status_name(status));
        stop_owned();
        return 0;
    }
    if (!owns_current_generation()) {
        puts("ZZRun: load begin did not claim this launcher generation");
        return 0;
    }
    load_slot = owned_slot;
    load_generation = owned_generation;
    if (!fill_argv()) {
        tell("Arguments do not fit in the argv area.");
        stop_owned();
        return 0;
    }
    while (offset < size) {
        LONG chunk = size - offset;
        LONG n;

        if (chunk > (LONG)ZZAPP_CHUNK_MAX)
            chunk = (LONG)ZZAPP_CHUNK_MAX;
        if (zzrun_user_break(ctrl_c_set(), user_stop)) {
            puts("ZZRun: stopped during load");
            stop_owned();
            return 0;
        }
        ownership_lost = 0;
        /* The staging area is firmware-owned while LOAD_BEGIN quarantines it. */
        n = load_read_async(fh, window + ZZAPP_STAGING_OFF, chunk,
                            &ownership_lost);
        if (ownership_lost) {
            (void)mailbox_selected(ZZAPP_OP_LOAD_END, NULL, 0, load_slot,
                                   load_generation, &reply);
            return 0;
        }
        if (n != chunk) {
            puts("ZZRun: short read while loading the ELF");
            stop_owned();
            return 0;
        }
        win_cache(ZZAPP_STAGING_OFF, (uint32_t)n);
        zzapp_build_chunk_request(payload, (uint32_t)offset, (uint32_t)n);
        status = mailbox(ZZAPP_OP_LOAD_CHUNK, payload, ZZAPP_CHUNK_REQ_BYTES, &reply);
        if (status != ZZ9K_STATUS_OK) {
            printf("ZZRun: chunk at %ld failed (%s)\n", (long)offset, status_name(status));
            stop_owned();
            return 0;
        }
        offset += n;
    }
    status = mailbox_long(ZZAPP_OP_LOAD_END, NULL, 0, &reply);
    if (status != ZZ9K_STATUS_OK) {
        printf("ZZRun: ELF rejected (%s)\n", status_name(status));
        stop_owned();
        return 0;
    }
    return 1;
}

static void report_fault(uint32_t code, int32_t exit_code, uint32_t pc,
                         uint32_t sp, uint32_t lr, uint32_t far, uint32_t fsr,
                         uint32_t cpsr, uint32_t generation)
{
    char line[160];

    launcher_statusf("ZZRun: %s\n", fault_name(code));
    launcher_statusf("  pc=%08lx sp=%08lx lr=%08lx\n",
                     (unsigned long)pc, (unsigned long)sp, (unsigned long)lr);
    launcher_statusf("  far=%08lx fsr=%08lx cpsr=%08lx\n",
                     (unsigned long)far, (unsigned long)fsr,
                     (unsigned long)cpsr);
    launcher_statusf("  exit=%ld generation=%lu\n", (long)exit_code,
                     (unsigned long)generation);
    if (_WBenchMsg && IntuitionBase) {
        struct EasyStruct es;

        snprintf(line, sizeof(line),
                 "%s\\npc=%08lx far=%08lx\\nexit=%ld",
                 fault_name(code), (unsigned long)pc, (unsigned long)far,
                 (long)exit_code);
        es.es_StructSize = sizeof(es);
        es.es_Flags = 0;
        es.es_Title = (UBYTE *)"ZZRun";
        es.es_TextFormat = (UBYTE *)line;
        es.es_GadgetFormat = (UBYTE *)"OK";
        EasyRequestArgs(NULL, &es, NULL, NULL);
    }
}

static int exit_from_app(int32_t code, uint32_t fault, int user_stop)
{
    if (fault == ZZAPP_FAULT_DATA || fault == ZZAPP_FAULT_PREFETCH ||
        fault == ZZAPP_FAULT_UNDEF || fault == ZZAPP_FAULT_WATCHDOG ||
        fault == ZZAPP_FAULT_WATCHDOG_RESTART)
        return 20;
    if (fault == ZZAPP_FAULT_FORCED && !user_stop)
        return 20;
    if (code < 0 || code > 255)
        return user_stop ? 0 : 20;
    return (int)code;
}

static void timer_arm(void)
{
    if (!timer_io || timer_pending)
        return;
    timer_io->tr_node.io_Command = TR_ADDREQUEST;
    timer_io->tr_time.tv_secs = 0;
    timer_io->tr_time.tv_micro = ZZRUN_POLL_US;
    SendIO((struct IORequest *)timer_io);
    timer_pending = 1;
}

static void timer_reap(void)
{
    if (!timer_io || !timer_pending)
        return;
    if (!CheckIO((struct IORequest *)timer_io))
        AbortIO((struct IORequest *)timer_io);
    WaitIO((struct IORequest *)timer_io);
    timer_pending = 0;
}

static int run_loop(ULONG forced_mode)
{
    int entered = 1;
    int saw_fault = 0;
    int display_failed = 0;
    int ownership_lost = 0;
    uint32_t last_gen = 0;
    struct zzrun_low_memory_state low_memory = { 0 };
    uint32_t fault = ZZAPP_FAULT_NONE;
    int32_t exit_code = 0;
    ZZ9KMailboxEntry reply;
    if (window) {
        last_gen = life_get(ZZAPP_LIFE_GENERATION_OFF);
        low_memory.generation = last_gen;
    }
    timer_arm();
    for (;;) {
        ULONG mask = SIGBREAKF_CTRL_C | SIGBREAKF_CTRL_D | SIGBREAKF_CTRL_E |
                     SIGBREAKF_CTRL_F;
        ULONG signals;
        uint32_t state, flags, crossings, generation;
        if (timer_port)
            mask |= 1UL << timer_port->mp_SigBit;
        if (con_port)
            mask |= 1UL << con_port->mp_SigBit;
        if (input_win && input_win->UserPort)
            mask |= 1UL << input_win->UserPort->mp_SigBit;
        signals = Wait(mask);
        if (signals & (1UL << timer_port->mp_SigBit))
            timer_reap();
        /* Everything below services shared app state. Stop before the
         * new generation's files, console, display, audio, or input can
         * be touched. */
        if (!owns_current_generation()) {
            ownership_lost = 1;
            break;
        }
        if (!heartbeat_owned(owned_generation)) {
            ownership_lost = 1;
            break;
        }
        if ((signals & SIGBREAKF_CTRL_C) || ctrl_c_set()) {
            SetSignal(0, SIGBREAKF_CTRL_C);
            user_stop = 1;
            request_stop();
        }
        /* A Shell delivers Ctrl-D/E/F as task signals, not as keys in the
         * app's display window, so map them here as well. */
        if (signals & SIGBREAKF_CTRL_D)
            push_event(ZZAPP_EV_BREAK, ZZAPP_BREAK_CTRL_D, 0, 0, 0);
        if (signals & SIGBREAKF_CTRL_E)
            push_event(ZZAPP_EV_BREAK, ZZAPP_BREAK_CTRL_E, 0, 0, 0);
        if (signals & SIGBREAKF_CTRL_F)
            push_event(ZZAPP_EV_BREAK, ZZAPP_BREAK_CTRL_F, 0, 0, 0);
        drain_window(input_win);
        poll_joysticks();
        serve_files();
        service_console();
        service_ahi_audio();
        print_log();
        load_palette();
        /* A new launcher may have acquired the window while one of the
         * services above was returning. Do not read its status or flip. */
        if (!owns_current_generation()) {
            ownership_lost = 1;
            break;
        }
        if (!window)
            break;
        state = life_get(ZZAPP_LIFE_STATE_OFF);
        flags = life_get(ZZAPP_LIFE_FLAGS_OFF);
        fault = life_get(ZZAPP_LIFE_FAULT_OFF);
        generation = life_get(ZZAPP_LIFE_GENERATION_OFF);
        if (zzrun_low_memory_supported(runtime_caps)) {
            crossings = life_get(ZZAPP_LIFE_LOW_MEMORY_CROSSINGS_OFF);
            if (zzrun_low_memory_changed(&low_memory, runtime_caps, generation,
                                         crossings))
                launcher_statusf("ZZRun: card memory low (%lu)\n",
                                 (unsigned long)crossings);
        }
        if ((flags & ZZAPP_F_DISPLAY_UNBOUND) && !screen) {
            if (!open_display(life_get(ZZAPP_LIFE_WIDTH_OFF),
                              life_get(ZZAPP_LIFE_HEIGHT_OFF),
                              life_get(ZZAPP_LIFE_FORMAT_OFF),
                              forced_mode)) {
                launcher_status("ZZRun: cannot open the requested screen");
                display_failed = 1;
                request_stop();
            }
        }
        ack_flip();
        {
            struct zzrun_exit_in leave;
            uint32_t generation = life_get(ZZAPP_LIFE_GENERATION_OFF);

            if (state == ZZAPP_ST_FAULTED ||
                fault == ZZAPP_FAULT_WATCHDOG_RESTART)
                saw_fault = 1;
            memset(&leave, 0, sizeof(leave));
            leave.state = state;
            leave.fault = fault;
            leave.elapsed_ms = stop_asked ? now_ms() - stop_ms : 0u;
            leave.generation = generation;
            leave.last_generation = last_gen;
            leave.entered = entered;
            leave.stop_asked = stop_asked;
            leave.user_stop = user_stop;
            leave.output_pending = console_output_pending();
            leave.output_failed = console_output_failed;
            if (zzrun_run_should_exit(&leave))
                break;
        }
        timer_arm();
    }
    timer_reap();
    if (ownership_lost) {
        /* cleanup() must only release local resources after ownership loss. */
        window = NULL;
        launcher_status("ZZRun: another launcher started an app; this app's exit status is lost.");
        return 20;
    }
    /* Legacy firmware has no selector fields for this terminal status read. */
    if (op_empty(ZZAPP_OP_STATUS, &reply) == ZZ9K_STATUS_OK &&
        reply.payload_len >= ZZAPP_STATUS_REPLY_BYTES) {
        uint8_t *p = reply.payload.inline_data;
        fault = zzapp_get_be32(p + ZZAPP_STATUS_FAULT);
        exit_code = zzapp_get_be32s(p + ZZAPP_STATUS_EXIT);
        if (fault || saw_fault)
            report_fault(fault, exit_code,
                         zzapp_get_be32(p + ZZAPP_STATUS_PC),
                         zzapp_get_be32(p + ZZAPP_STATUS_SP),
                         zzapp_get_be32(p + ZZAPP_STATUS_LR),
                         zzapp_get_be32(p + ZZAPP_STATUS_FAR),
                         zzapp_get_be32(p + ZZAPP_STATUS_FSR),
                         zzapp_get_be32(p + ZZAPP_STATUS_CPSR),
                         zzapp_get_be32(p + ZZAPP_STATUS_GENERATION));
        else
            launcher_statusf("ZZRun: exit %ld (%s)\n", (long)exit_code,
                             state_name(zzapp_get_be32(p + ZZAPP_STATUS_STATE)));
        return zzrun_process_exit(exit_from_app(exit_code, fault, stop_asked),
                                  display_failed, console_output_failed,
                                  audio_open_failed);
    }
    if (window) {
        fault = life_get(ZZAPP_LIFE_FAULT_OFF);
        exit_code = (int32_t)life_get(ZZAPP_LIFE_EXIT_OFF);
        if (fault)
            report_fault(fault, exit_code,
                         life_get(ZZAPP_LIFE_PC_OFF),
                         life_get(ZZAPP_LIFE_SP_OFF),
                         life_get(ZZAPP_LIFE_LR_OFF),
                         life_get(ZZAPP_LIFE_FAR_OFF),
                         life_get(ZZAPP_LIFE_FSR_OFF),
                         life_get(ZZAPP_LIFE_CPSR_OFF),
                         life_get(ZZAPP_LIFE_GENERATION_OFF));
        return zzrun_process_exit(exit_from_app(exit_code, fault, stop_asked),
                                  display_failed, console_output_failed,
                                  audio_open_failed);
    }
    return 20;
}

static int open_root(const char *root_text)
{
    if (root_text && root_text[0]) {
        root_lock = Lock((STRPTR)root_text, ACCESS_READ);
        if (!root_lock) {
            printf("ZZRun: cannot lock ROOT %s\n", root_text);
            return 0;
        }
        return 1;
    }
    if (_WBenchMsg && _WBenchMsg->sm_NumArgs >= 1) {
        root_lock = DupLock(_WBenchMsg->sm_ArgList[0].wa_Lock);
    } else {
        BPTR cur = CurrentDir(0);
        CurrentDir(cur);
        root_lock = DupLock(cur);
    }
    if (!root_lock) {
        puts("ZZRun: cannot lock the root directory");
        return 0;
    }
    return 1;
}

static void cleanup(void)
{
    int i;
    timer_reap();
    if (!stop_asked && owns_current_generation() &&
        life_get(ZZAPP_LIFE_STATE_OFF) != ZZAPP_ST_IDLE)
        stop_owned();
    ahi_close();
    window_cache_release();
    /* A pending console read cannot be cancelled. Close the app screen
     * before that wait so the prompt is not hidden behind it. */
    close_display();
    console_read_finish();
    if (console_handle) {
        Close(console_handle);
        console_handle = 0;
    }
    for (i = 0; i < ZZAPP_FD_COUNT; i++) {
        close_fd_index(i);
        close_list_index(i);
    }
    if (root_lock) {
        UnLock(root_lock);
        root_lock = 0;
    }
    if (timer_io) {
        if (timer_io->tr_node.io_Device)
            CloseDevice((struct IORequest *)timer_io);
        DeleteIORequest((struct IORequest *)timer_io);
        timer_io = NULL;
        TimerBase = NULL;
    }
    if (timer_port) {
        DeleteMsgPort(timer_port);
        timer_port = NULL;
    }
    if (P96Base) {
        CloseLibrary(P96Base);
        P96Base = NULL;
    }
    if (IconBase) {
        CloseLibrary(IconBase);
        IconBase = NULL;
    }
    if (ZZ9KBase) {
        CloseLibrary(ZZ9KBase);
        ZZ9KBase = NULL;
    }
    if (IntuitionBase) {
        CloseLibrary((struct Library *)IntuitionBase);
        IntuitionBase = NULL;
    }
    if (GfxBase) {
        CloseLibrary((struct Library *)GfxBase);
        GfxBase = NULL;
    }
    window = NULL;
    if (launcher_output_active) {
        launcher_output_active = 0;
        if (launcher_log_dropped)
            printf("ZZRun: dropped %lu diagnostic log records.\n",
                   (unsigned long)launcher_log_dropped);
    }
}

int main(int argc, char **argv)
{
    char app_from_icon[256];
    char root_text[256];
    ULONG forced_mode = 0;
    int have_mode = 0;
    ZZ9KMailboxEntry reply;
    uint8_t payload[16];
    int status;
    BPTR fh = 0;
    LONG size;
    int rc = 20;
    int show_status = 0;
    int release_slot = -1;
    uint32_t abi_major, abi_minor, caps, state;
    uint32_t arm_address, host_offset, win_size;
    struct zzrun_claim_reply claim;
    struct zzrun_slots_query slots_query;

    app_from_icon[0] = 0;
    root_text[0] = 0;
    elf_path[0] = 0;
    if (!stack_ok()) {
        tell("Run Stack 32768 in this Shell before ZZRun.");
        return 10;
    }
    GfxBase = (struct GfxBase *)OpenLibrary((CONST_STRPTR)"graphics.library", 39);
    IntuitionBase = (struct IntuitionBase *)OpenLibrary(
        (CONST_STRPTR)"intuition.library", 39);
    IconBase = OpenLibrary((CONST_STRPTR)"icon.library", 37);
    if (!GfxBase || !IntuitionBase) {
        tell("AmigaOS 3 graphics and Intuition are required.");
        cleanup();
        return 20;
    }
    if (!load_program_icon(app_from_icon, sizeof(app_from_icon),
                           root_text, sizeof(root_text),
                           &forced_mode, &have_mode, &maxmem_mib,
                           &hostmem_mib)) {
        usage();
        cleanup();
        return 10;
    }
    if (_WBenchMsg) {
        if (_WBenchMsg->sm_NumArgs >= 2) {
            char name[256];
            if (NameFromLock(_WBenchMsg->sm_ArgList[1].wa_Lock, (STRPTR)name,
                             (LONG)sizeof(name)) > 0) {
                size_t n = strlen(name);
                size_t m = strlen((char *)_WBenchMsg->sm_ArgList[1].wa_Name);
                if (n + 1 + m < sizeof(elf_path)) {
                    memcpy(elf_path, name, n);
                    if (n && elf_path[n - 1] != ':')
                        elf_path[n++] = '/';
                    memcpy(elf_path + n, _WBenchMsg->sm_ArgList[1].wa_Name, m + 1);
                }
            }
        } else if (app_from_icon[0]) {
            memcpy(elf_path, app_from_icon, strlen(app_from_icon) + 1);
        }
        if (!elf_path[0]) {
            tell("Set the APP tooltype, or start ZZRun from a project icon.");
            cleanup();
            return 10;
        }
    } else {
        int i;

        if (argc == 3 && strcmp(argv[1], "RELEASE") == 0) {
            char *end = NULL;
            unsigned long slot = strtoul(argv[2], &end, 10);

            if (!end || *end || slot >= ZZAPP_SLOT_COUNT) {
                usage();
                cleanup();
                return 10;
            }
            release_slot = (int)slot;
        } else if (argc == 2 && zzrun_is_status_keyword(argv[1])) {
            show_status = 1;
        } else {
            if (argc < 2) {
                usage();
                cleanup();
                return 10;
            }
            if (strlen(argv[1]) >= sizeof(elf_path)) {
                tell("ELF path is too long.");
                cleanup();
                return 10;
            }
            memcpy(elf_path, argv[1], strlen(argv[1]) + 1);
            for (i = 2; i < argc; i++) {
                if (strncmp(argv[i], "AUDIO=", 6) == 0) {
                    if (!tool_is_ahi(argv[i] + 6)) {
                        tell("AUDIO must be AHI.");
                        cleanup();
                        return 10;
                    }
                    audio_ahi = 1;
                    continue;
                }
                if (strncmp(argv[i], "HOSTMEM=", 8) == 0) {
                    if (!zzrun_parse_hostmem(argv[i] + 8, &hostmem_mib)) {
                        usage();
                        cleanup();
                        return 10;
                    }
                    continue;
                }
                if (strncmp(argv[i], "MAXMEM=", 7) == 0) {
                    if (!zzrun_parse_maxmem(argv[i] + 7, &maxmem_mib)) {
                        usage();
                        cleanup();
                        return 10;
                    }
                    continue;
                }
                if (!add_arg(argv[i])) {
                    cleanup();
                    return 10;
                }
            }
        }
    }
    if (!show_status && release_slot < 0 && !open_root(root_text)) {
        cleanup();
        return 20;
    }
    if (!zz9000_find_board(&board) || !board.address) {
        tell("ZZ9000 board not found.");
        cleanup();
        return 20;
    }
    if (!show_status && release_slot < 0)
        printf("ZZRun %s\nboard %08lx Zorro %lu\n", ZZRUN_VERSION,
               (unsigned long)board.address, (unsigned long)board.zorro_version);
    ZZ9KBase = OpenLibrary((CONST_STRPTR)"zz9k.library", 0);
    if (!ZZ9KBase) {
        tell("zz9k.library is not installed.");
        cleanup();
        return 20;
    }
    /* The extended query carries every capability bit and the slot counts. */
    status = op_empty(ZZAPP_OP_QUERY, &reply);
    if (status != ZZ9K_STATUS_OK || reply.payload_len < ZZAPP_QUERY_REPLY_BYTES) {
        printf("ZZRun: APP_QUERY failed (%s). Zorro III app firmware is required.\n",
               status_name(status));
        cleanup();
        return 20;
    }
    abi_major = zzapp_get_be32(reply.payload.inline_data + ZZAPP_QUERY_ABI_MAJOR);
    abi_minor = zzapp_get_be32(reply.payload.inline_data + ZZAPP_QUERY_ABI_MINOR);
    state = zzapp_get_be32(reply.payload.inline_data + ZZAPP_QUERY_STATE);
    caps = zzapp_get_be32(reply.payload.inline_data + ZZAPP_QUERY_CAPS);
    if (!show_status)
        printf("runtime abi %lu.%lu state %s caps %08lx\n",
               (unsigned long)abi_major, (unsigned long)abi_minor,
               state_name(state), (unsigned long)caps);
    if (board.zorro_version != 3 || !(caps & ZZAPP_CAP_Z3) || abi_major != 0) {
        tell("ZZRun requires a Zorro III board and app-runtime firmware.");
        cleanup();
        return 20;
    }
    if (!(caps & ZZAPP_CAP_SLOTS) ||
        !zzrun_slots_query_decode(reply.payload.inline_data, reply.payload_len,
                                  &slots_query)) {
        tell("ZZRun requires app-runtime firmware with app slots.");
        cleanup();
        return 20;
    }
    runtime_caps = caps;
    if (show_status) {
        rc = print_status(&slots_query);
        cleanup();
        return rc;
    }
    if (release_slot >= 0) {
        rc = release_quarantined_slot((uint32_t)release_slot);
        cleanup();
        return rc;
    }
    if (zzrun_hostmem_decision(runtime_caps, hostmem_mib) ==
        ZZRUN_HOSTMEM_REJECT) {
        tell("ZZRun: HOSTMEM requires firmware with host-visible limits.");
        cleanup();
        return 20;
    }
    status = op_empty(ZZAPP_OP_ALLOC_WINDOW, &reply);
    if (status != ZZ9K_STATUS_OK ||
        !zzrun_claim_reply_decode(reply.payload.inline_data, reply.payload_len,
                                  1, &claim)) {
        printf("ZZRun: window allocation failed (%s)\n", status_name(status));
        cleanup();
        return 20;
    }
    arm_address = claim.arm_address;
    win_size = claim.size;
    host_offset = claim.host_offset;
    owned_slot = claim.slot;
    owned_generation = claim.generation;
    if (win_size < ZZAPP_WINDOW_BYTES ||
        host_offset > board.board_size ||
        win_size > board.board_size - host_offset) {
        puts("ZZRun: rings window is outside the board");
        cleanup();
        return 20;
    }
    (void)arm_address;
    window = (uint8_t *)(board.address + host_offset);
    window_bytes = win_size;
    win_cache(0, 256);
    if (!window_cache_disable()) {
        cleanup();
        return 20;
    }
    if (life_get(ZZAPP_LIFE_MAGIC_OFF) != ZZAPP_LIFE_MAGIC ||
        life_get(ZZAPP_LIFE_VERSION_OFF) != ZZAPP_LIFE_VERSION) {
        puts("ZZRun: rings window magic does not match");
        cleanup();
        return 20;
    }
    P96Base = OpenLibrary((CONST_STRPTR)"Picasso96API.library", 2);
    timer_port = CreateMsgPort();
    if (timer_port)
        timer_io = (struct timerequest *)CreateIORequest(
            timer_port, sizeof(struct timerequest));
    if (!timer_io || OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_MICROHZ,
                                (struct IORequest *)timer_io, 0)) {
        tell("timer.device is required.");
        cleanup();
        return 20;
    }
    TimerBase = timer_io->tr_node.io_Device;
    if (zzrun_lease_gate_stops((int)owned_generation,
                                heartbeat_owned(owned_generation))) {
        cleanup();
        return 20;
    }
    file_generation = owned_generation;
    {
        BPTR base_dir = 0;
        int transport_ok;

        if (!strchr(elf_path, ':')) {
            base_dir = CurrentDir(0);
            CurrentDir(base_dir);
        }
        fh = open_find_async(base_dir, elf_path, MODE_OLDFILE, &transport_ok);
        if (!transport_ok || !fh) {
            printf("ZZRun: cannot open %s\n", elf_path);
            cleanup();
            return zzrun_load_failure_exit(ctrl_c_set(), user_stop);
        }
    }
    if (zzrun_lease_gate_stops((int)owned_generation,
                                heartbeat_owned(owned_generation))) {
        end_or_close(fh);
        cleanup();
        return 20;
    }
    {
        int transport_ok = 0;
        LONG end_seek = file_packet_async(fh, ACTION_SEEK, 0, OFFSET_END,
                                          0, 0, &transport_ok);

        if (!transport_ok || end_seek < 0) {
            printf("ZZRun: cannot read %s (io error %ld)\n", elf_path,
                   (long)zz_from_io());
            end_or_close(fh);
            cleanup();
            return zzrun_load_failure_exit(ctrl_c_set(), user_stop);
        }
    }
    if (zzrun_lease_gate_stops((int)owned_generation,
                                heartbeat_owned(owned_generation))) {
        end_or_close(fh);
        cleanup();
        return 20;
    }
    {
        int transport_ok = 0;

        size = file_packet_async(fh, ACTION_SEEK, 0, OFFSET_BEGINNING, 0, 0,
                                 &transport_ok);
        if (!transport_ok || size < 0) {
            printf("ZZRun: cannot read %s\n", elf_path);
            end_or_close(fh);
            cleanup();
            return zzrun_load_failure_exit(ctrl_c_set(), user_stop);
        }
    }
    if (zzrun_lease_gate_stops((int)owned_generation,
                                heartbeat_owned(owned_generation))) {
        end_or_close(fh);
        cleanup();
        return 20;
    }
    if (size <= 0 || (ULONG)size > ZZRUN_IMAGE_LIMIT) {
        printf("ZZRun: %s is empty or larger than the 16 MiB image limit\n",
               elf_path);
        end_or_close(fh);
        cleanup();
        return 20;
    }
    printf("loading %s (%ld bytes)\n", elf_path, (long)size);
    if (!load_elf(fh, size)) {
        end_or_close(fh);
        cleanup();
        return zzrun_load_failure_exit(ctrl_c_set(), user_stop);
    }
    end_or_close(fh);
    fh = 0;
    /* Launcher capabilities go in after the load: LOAD_BEGIN clears the
     * flags word and LOAD_END replaces it with the ELF note's flags.  The
     * app is not running yet, so this read-modify-write cannot race. */
    if (!life_put_owned(
            ZZAPP_LIFE_FLAGS_OFF,
            zzrun_launcher_lease_flags(
                life_get(ZZAPP_LIFE_FLAGS_OFF) | ZZAPP_F_LAUNCHER_CLUT8 |
                ZZAPP_F_LAUNCHER_SCANOUT_ACK | ZZAPP_F_LAUNCHER_MAILBOX |
                (audio_ahi ? ZZAPP_F_LAUNCHER_AUDIO_AHI : 0u)))) {
        cleanup();
        return 20;
    }
    zzapp_build_start_request(payload, (uint32_t)app_argc + 1u);
    status = mailbox_long(ZZAPP_OP_START, payload, ZZAPP_START_REQ_BYTES,
                          &reply);
    if (status != ZZ9K_STATUS_OK) {
        printf("ZZRun: start failed (%s)\n", status_name(status));
        stop_owned();
        cleanup();
        return 20;
    }
    fflush(stdout);
    launcher_output_active = 1;
    rc = run_loop(have_mode ? forced_mode : 0);
    cleanup();
    return rc;
}
