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
#include <proto/aar.h>
#include "zzrun_policy.h"
#include "zzrun_policy.c"
#include "zzrun_serve.h"
#include "zzrun_serve.c"
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
struct Library *AHIBase;

extern struct WBStartup *_WBenchMsg;

#define ZZRUN_VERSION "0.1"
static const char version[] __attribute__((used)) =
    "$VER: ZZRun " ZZRUN_VERSION " (06.11.2026)\r\n";

#define ZZRUN_MIN_STACK     16384UL
#define ZZRUN_GRACE_MS      500u
#define ZZRUN_POLL_US       1000UL
_Static_assert(ZZRUN_POLICY_GRACE_MS == ZZRUN_GRACE_MS, "grace");

/* The serve layer branches on these AmigaDOS error numbers without dos.h. */
_Static_assert(ZZRUN_IOERR_NO_FREE_STORE == ERROR_NO_FREE_STORE, "dos error");
_Static_assert(ZZRUN_IOERR_OBJECT_IN_USE == ERROR_OBJECT_IN_USE, "dos error");
_Static_assert(ZZRUN_IOERR_OBJECT_EXISTS == ERROR_OBJECT_EXISTS, "dos error");
_Static_assert(ZZRUN_IOERR_DIR_NOT_FOUND == ERROR_DIR_NOT_FOUND, "dos error");
_Static_assert(ZZRUN_IOERR_OBJECT_NOT_FOUND == ERROR_OBJECT_NOT_FOUND,
               "dos error");
_Static_assert(ZZRUN_IOERR_ACTION_NOT_KNOWN == ERROR_ACTION_NOT_KNOWN,
               "dos error");
_Static_assert(ZZRUN_IOERR_OBJECT_TOO_LARGE == ERROR_OBJECT_TOO_LARGE,
               "dos error");
_Static_assert(ZZRUN_IOERR_OBJECT_WRONG_TYPE == ERROR_OBJECT_WRONG_TYPE,
               "dos error");
_Static_assert(ZZRUN_IOERR_DEVICE_NOT_MOUNTED == ERROR_DEVICE_NOT_MOUNTED,
               "dos error");
_Static_assert(ZZRUN_IOERR_SEEK_ERROR == ERROR_SEEK_ERROR, "dos error");
_Static_assert(ZZRUN_IOERR_DISK_FULL == ERROR_DISK_FULL, "dos error");
_Static_assert(ZZRUN_IOERR_DELETE_PROTECTED == ERROR_DELETE_PROTECTED,
               "dos error");
_Static_assert(ZZRUN_IOERR_WRITE_PROTECTED == ERROR_WRITE_PROTECTED,
               "dos error");
_Static_assert(ZZRUN_IOERR_READ_PROTECTED == ERROR_READ_PROTECTED,
               "dos error");
_Static_assert(ZZRUN_IOERR_NO_MORE_ENTRIES == ERROR_NO_MORE_ENTRIES,
               "dos error");
_Static_assert(ZZRUN_IOERR_BREAK == ERROR_BREAK, "dos error");

#define ZZRUN_AHI_BUFFER_COUNT 2u
#define ZZRUN_AHI_BUFFER_BYTES 8192u

struct Library *AARBase;
static AARHandle *aar_handle;
static AARInfo platform_info;
static const aar_client_ext_zz9000 *zz_ext;
static int claim_open;
static int claim_lost;
static char elf_full[512];
static int abs_paths;
static int stop_asked;
static int user_stop;
static int stop_event_sent;
static uint32_t stop_ms;
static int have_two;
/* Panning fallback uses a triple-height bitmap when P96 permits it.  The
 * spare third page lets the app start the next frame while the last pan is
 * pending; a two-page bitmap remains a compatible fallback. */
static int have_pan;
static int pan_count;
static char pub_name[128];
static char elf_path[256];
static char arg_store[ZZRUN_ARGV_MAX][ZZRUN_ARGV_STR_MAX];
static int app_argc;
static BPTR root_lock;
static struct Screen *screen;
static struct Window *input_win;
static struct ScreenBuffer *buf_front;
static struct ScreenBuffer *buf_back;
static int joy_port0;
static uint16_t mouse_buttons;
static ULONG joy_last[2];
static int joy_valid[2];
static struct MsgPort *timer_port;
static struct timerequest *timer_io;
static int timer_pending;
static struct MsgPort *flip_safe_port;
static uint32_t flip_pending;
static uint32_t flip_safe_pending;
static uint32_t maxmem_mib;
static uint32_t hostmem_mib;
static uint32_t weight_value;
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
static int console_output_failed;
static int audio_open_failed;
static int display_failed;
static int queue_corrupt;
static int launcher_output_active;
static uint32_t app_state;
static ULONG display_forced_mode;
static struct zzrun_serve serve;
static struct zzrun_events events;

static void cleanup(void);
static int tool_is_ahi(const char *value);
static void launcher_status(const char *msg);
static void request_stop(void);
static int dos_packet_async(struct MsgPort *handler, LONG action, LONG arg1,
                            LONG arg2, LONG arg3, LONG arg4, LONG arg5,
                            int check_live, LONG *result_out,
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


/* One walk over the candidates of a DOS name: GetDeviceProc candidates, or
 * one fixed handler and directory lock (a relative parent, or a candidate
 * pinned after a probe). A pinned resolver may still own the DevProc that
 * its directory lock belongs to, so it is released only when the operation
 * ends. */
struct dos_path_resolver {
    const char *name;
    struct DevProc *devproc;
    struct MsgPort *handler;
    BPTR directory;
    int fixed_used;
};

static void release_dos_path_resolver(struct dos_path_resolver *resolver);
static int dos_path_resolver_start(struct dos_path_resolver *resolver,
                                   BPTR dir, const char *name);
static BPTR lock_resolved_async(struct dos_path_resolver *resolver,
                                const char *name,
                                struct zzrun_dos_path_candidate *selected,
                                int *transport_ok);
static BPTR open_resolved_async(struct dos_path_resolver *resolver,
                                const char *name, LONG mode,
                                int *transport_ok);
static int end_async(BPTR fh, int *transport_ok);
static void end_or_close(BPTR fh);
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

/* True while the claim is ours: it was opened and no call has reported it
 * revoked. A flag, no card access: every library call below reports
 * AAR_E_STALE through claim_note(). */
static int claim_live(void)
{
    return aar_handle != NULL && !claim_lost;
}

static LONG claim_note(LONG rc)
{
    if (rc == AAR_E_STALE)
        claim_lost = 1;
    return rc;
}

/* Is the claim still ours? A status read of the smallest size: the state
 * word and the handle's generation. The library renews the lease itself
 * (from a vertical-blank interrupt), so this is not a renewal. */
static int claim_check(void)
{
    AARStatusInfo st;

    if (!claim_live())
        return 0;
    memset(&st, 0, sizeof(st));
    st.size = offsetof(AARStatusInfo, generation);
    return claim_note(AARStatus(aar_handle, &st)) == AAR_OK;
}

/* The state and the end reason: size 16 reads three control block words. */
static LONG claim_state(uint32_t *state, uint32_t *end_reason)
{
    AARStatusInfo st;
    LONG rc;

    if (!claim_live())
        return AAR_E_STALE;
    memset(&st, 0, sizeof(st));
    st.size = offsetof(AARStatusInfo, exit_code);
    rc = claim_note(AARStatus(aar_handle, &st));
    if (rc == AAR_OK) {
        *state = st.state;
        *end_reason = st.end_reason;
    }
    return rc;
}

/* The AAR_REASON_* of the last refused load or start. */
static ULONG claim_reason(void)
{
    AARStatusInfo st;

    memset(&st, 0, sizeof(st));
    st.size = sizeof(st);
    if (claim_live() && claim_note(AARStatus(aar_handle, &st)) == AAR_OK)
        return st.reason;
    return AAR_REASON_NONE;
}

static uint32_t now_ms(void)
{
    struct timeval tv;
    GetSysTime(&tv);
    return (uint32_t)tv.tv_secs * 1000u + (uint32_t)(tv.tv_micro / 1000);
}


static int ctrl_c_set(void)
{
    return (SetSignal(0, 0) & SIGBREAKF_CTRL_C) != 0;
}

static void usage(void)
{
    puts("ZZRun <app.elf> [MAXMEM=<MiB>] [HOSTMEM=<MiB>] [WEIGHT=<1-256>] [AUDIO=AHI] [args]\n"
         "ZZRun STATUS\n"
         "ZZRun RELEASE <handle>\n"
         "Run Stack 32768 in this Shell first.\n"
         "Workbench ToolTypes: APP, ROOT, ABS, MODE, ARGS, JOYPORT0, AUDIO, MAXMEM, HOSTMEM, WEIGHT.\n"
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

static const char *fault_name(uint32_t code)
{
    return zzrun_fault_class_name(code);
}

static const char *state_name(uint32_t state)
{
    return zzrun_slot_state_name(state);
}

static int print_status(void)
{
    uint32_t cursor = 0;
    unsigned shown = 0;

    for (;;) {
        AARZZ9000List item;
        LONG rc;

        memset(&item, 0, sizeof(item));
        item.size = sizeof(item);
        rc = zz_ext->list(cursor, &item);
        if (rc < 0) {
            printf("ZZRun: APP_LIST failed (%s)\n", zzrun_error_name((int)rc));
            return 20;
        }
        if (rc == 0 || item.handle == 0 || shown >= 4096u)
            break;
        printf("claim %08lx state %s name %s committed %lu MiB cpu %lu s "
               "block %s reason %s\n",
               (unsigned long)item.handle, state_name(item.state),
               item.name[0] ? (const char *)item.name : "-",
               (unsigned long)(item.committed / ZZRUN_MIB_BYTES),
               (unsigned long)(item.cpu_ms / 1000u),
               zzrun_block_reason_name(item.block_reason),
               zzrun_end_reason_name(item.end_reason));
        shown++;
        /* index is the card's slot index: the next cursor is one past it. */
        cursor = item.index + 1u;
    }
    if (!shown)
        puts("ZZRun: no claims");
    return 0;
}

/* RELEASE <handle>: ends a claim that is no longer running (its app ended,
 * or the card revoked it). A running app is not ended this way. */
static int release_claim(uint32_t handle)
{
    uint32_t cursor = 0;
    unsigned guard;

    for (guard = 0; guard < 4096u; guard++) {
        AARZZ9000List item;
        LONG rc;

        memset(&item, 0, sizeof(item));
        item.size = sizeof(item);
        rc = zz_ext->list(cursor, &item);
        if (rc < 0) {
            printf("ZZRun: APP_LIST failed (%s)\n", zzrun_error_name((int)rc));
            return 20;
        }
        if (rc == 0 || item.handle == 0)
            break;
        if (item.handle == handle) {
            if (!zzrun_release_allowed(item.state, item.end_reason)) {
                printf("ZZRun: claim %08lx is still in use (%s)\n",
                       (unsigned long)handle, state_name(item.state));
                return 20;
            }
            rc = zz_ext->release_handle(handle);
            if (rc != AAR_OK) {
                printf("ZZRun: release of claim %08lx failed (%s)\n",
                       (unsigned long)handle, zzrun_error_name((int)rc));
                return 20;
            }
            printf("ZZRun: released claim %08lx\n", (unsigned long)handle);
            return 0;
        }
        cursor = item.index + 1u;
    }
    printf("ZZRun: there is no claim %08lx\n", (unsigned long)handle);
    return 20;
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
        app_argc >= (int)ZZRUN_ARGV_MAX - 1) {
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
                          uint32_t *hostmem, uint32_t *weight)
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
    value = tool_value(icon, "WEIGHT");
    if (value && !zzrun_parse_weight(value, weight))
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
                             uint32_t *maxmem, uint32_t *hostmem,
                             uint32_t *weight)
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
                                have_mode, maxmem, hostmem, weight);
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

/* ---- The DOS backend of the file handlers --------------------------------
 *
 * zzrun_serve.c decides what each request means (open plans, ROOT
 * confinement, descriptors); these functions are the AmigaDOS calls under it.
 * Every DOS call is an asynchronous packet (dos_packet_async), so a slow
 * handler never stops the main loop's Ctrl-C. */

struct dos_list {
    BPTR lock;
    struct FileInfoBlock *fib;
};

/* The object one open acts on. dir is the parent lock of a relative name,
 * or 0 for an absolute (ABS) name that GetDeviceProc resolves. Resolving an
 * absolute name restarts at the first directory of a multi-directory assign,
 * so the probe pins the resolver to the candidate that holds the file and
 * every later packet of the same open (update, truncate, NEWFILE fallback)
 * reaches that file, never a new one made in an earlier directory. */
struct open_target {
    const char *name;
    BPTR dir;
    struct dos_path_resolver resolver;
};

static struct open_target cur_target;

/* Ready the resolver for the next packet. A pinned or relative resolver
 * (handler set) replays its one candidate; an unresolved absolute name
 * starts a new walk and drops the previous walk's DevProc. */
static int open_target_arm(struct open_target *target)
{
    if (target->resolver.handler) {
        target->resolver.fixed_used = 0;
        return 1;
    }
    release_dos_path_resolver(&target->resolver);
    return dos_path_resolver_start(&target->resolver, target->dir,
                                   target->name);
}

static int be_live(void *ctx)
{
    (void)ctx;
    return claim_live();
}

static int be_user_break(void *ctx)
{
    (void)ctx;
    return zzrun_user_break(ctrl_c_set(), user_stop);
}

static uintptr_t be_dup_lock(void *ctx, uintptr_t lock)
{
    (void)ctx;
    return (uintptr_t)DupLock((BPTR)lock);
}

static void be_unlock(void *ctx, uintptr_t lock)
{
    (void)ctx;
    UnLock((BPTR)lock);
}

static uintptr_t be_parent_dir(void *ctx, uintptr_t lock)
{
    (void)ctx;
    return (uintptr_t)ParentDir((BPTR)lock);
}

static int be_same_lock(void *ctx, uintptr_t a, uintptr_t b)
{
    (void)ctx;
    return SameLock((BPTR)a, (BPTR)b) == LOCK_SAME;
}

static uintptr_t be_lock_rel(void *ctx, uintptr_t dir, const char *name,
                             int *transport_ok, int32_t *io)
{
    BPTR lock;

    (void)ctx;
    lock = lock_async((BPTR)dir, name, ACCESS_READ, transport_ok);
    *io = (int32_t)IoErr();
    return (uintptr_t)lock;
}

static uintptr_t be_lock_abs(void *ctx, const char *name, int *transport_ok,
                             int32_t *io)
{
    BPTR lock;

    (void)ctx;
    lock = lock_abs_async(name, transport_ok);
    *io = (int32_t)IoErr();
    return (uintptr_t)lock;
}

static void info_from_fib(const struct FileInfoBlock *fib,
                          struct zzrun_info *info)
{
    memset(info, 0, sizeof(*info));
    info->is_dir = fib->fib_DirEntryType > 0;
    info->size = (uint32_t)fib->fib_Size;
    info->mtime = zzrun_amiga_unix((uint32_t)fib->fib_Date.ds_Days,
                                   (uint32_t)fib->fib_Date.ds_Minute,
                                   (uint32_t)fib->fib_Date.ds_Tick);
    zzrun_bstr_decode((const uint8_t *)fib->fib_FileName,
                      sizeof(fib->fib_FileName), info->name, sizeof(info->name));
}

static int be_lock_examine(void *ctx, uintptr_t lock, struct zzrun_info *info,
                           int *transport_ok, int32_t *io)
{
    struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
    int ok;

    (void)ctx;
    *transport_ok = 1;
    *io = 0;
    if (!fib) {
        *io = ERROR_NO_FREE_STORE;
        return 0;
    }
    ok = examine_lock_async((BPTR)lock, fib, transport_ok);
    *io = (int32_t)IoErr();
    if (ok)
        info_from_fib(fib, info);
    FreeDosObject(DOS_FIB, fib);
    return ok;
}

static void be_target_begin(void *ctx, uintptr_t dir, const char *name)
{
    (void)ctx;
    memset(&cur_target, 0, sizeof(cur_target));
    cur_target.name = name;
    cur_target.dir = (BPTR)dir;
}

/* Does the object exist? ERROR_OBJECT_IN_USE counts as existing: the handler
 * refused the lock because someone holds the object. Any other lock failure
 * is the caller's error. An absolute name that exists pins the resolver to
 * the candidate that answered; one that does not exist stays unresolved. */
static int be_target_probe(void *ctx, int *exists, int *transport_ok,
                           int32_t *io)
{
    struct zzrun_dos_path_candidate selected;
    enum zzrun_probe_result found;
    BPTR locked;

    (void)ctx;
    memset(&selected, 0, sizeof(selected));
    *io = 0;
    *transport_ok = 0;
    if (cur_target.dir) {
        locked = lock_async(cur_target.dir, cur_target.name, ACCESS_READ,
                            transport_ok);
    } else {
        if (!open_target_arm(&cur_target))
            return 0;
        locked = lock_resolved_async(&cur_target.resolver, cur_target.name,
                                     &selected, transport_ok);
    }
    if (!*transport_ok)
        return 0;
    *io = (int32_t)(locked ? 0 : IoErr());
    found = zzrun_probe_result_for_lock(locked != 0, *io,
                                        ERROR_OBJECT_NOT_FOUND,
                                        ERROR_OBJECT_IN_USE);
    if (found == ZZRUN_PROBE_ERROR)
        return 0;
    if (locked)
        UnLock(locked);
    *exists = found == ZZRUN_PROBE_EXISTS;
    if (!cur_target.dir && *exists) {
        cur_target.resolver.handler = (struct MsgPort *)selected.handler;
        cur_target.resolver.directory = (BPTR)selected.directory;
        cur_target.resolver.fixed_used = 0;
    }
    return 1;
}

static uintptr_t be_target_open(void *ctx, enum zzrun_mode mode,
                                int *transport_ok, int32_t *io)
{
    LONG dos_mode = mode == ZZRUN_MODE_NEW ? MODE_NEWFILE :
                    (mode == ZZRUN_MODE_READWRITE ? MODE_READWRITE :
                     MODE_OLDFILE);
    BPTR fh;

    (void)ctx;
    *transport_ok = 0;
    *io = 0;
    if (!open_target_arm(&cur_target))
        return 0;
    fh = open_resolved_async(&cur_target.resolver, cur_target.name, dos_mode,
                             transport_ok);
    *io = (int32_t)IoErr();
    return (uintptr_t)fh;
}

static void be_target_end(void *ctx)
{
    (void)ctx;
    release_dos_path_resolver(&cur_target.resolver);
    memset(&cur_target, 0, sizeof(cur_target));
}

static int32_t be_fh_set_size0(void *ctx, uintptr_t fh, int *transport_ok,
                               int32_t *io)
{
    LONG result;

    (void)ctx;
    result = file_packet_async((BPTR)fh, ACTION_SET_FILE_SIZE, 0,
                               OFFSET_BEGINNING, 0, 0, transport_ok);
    *io = (int32_t)IoErr();
    return (int32_t)result;
}

static int32_t be_fh_read(void *ctx, uintptr_t fh, uint8_t *dst,
                          uint32_t bytes, int *transport_ok, int32_t *io)
{
    LONG result;

    (void)ctx;
    result = file_packet_async((BPTR)fh, ACTION_READ, (LONG)dst, (LONG)bytes,
                               0, 0, transport_ok);
    *io = (int32_t)IoErr();
    return (int32_t)result;
}

static int32_t be_fh_write(void *ctx, uintptr_t fh, const uint8_t *src,
                           uint32_t bytes, int *transport_ok, int32_t *io)
{
    LONG result;

    (void)ctx;
    result = file_packet_async((BPTR)fh, ACTION_WRITE, (LONG)src, (LONG)bytes,
                               0, 0, transport_ok);
    *io = (int32_t)IoErr();
    return (int32_t)result;
}

static int32_t be_fh_seek(void *ctx, uintptr_t fh, int32_t offset, int whence,
                          int *transport_ok, int32_t *io)
{
    LONG mode = whence == 0 ? OFFSET_BEGINNING :
                (whence == 1 ? OFFSET_CURRENT : OFFSET_END);
    LONG result;

    (void)ctx;
    result = file_packet_async((BPTR)fh, ACTION_SEEK, (LONG)offset, mode, 0, 0,
                               transport_ok);
    *io = (int32_t)IoErr();
    return (int32_t)result;
}

static void be_fh_end_or_close(void *ctx, uintptr_t fh)
{
    (void)ctx;
    end_or_close((BPTR)fh);
}

static int be_fh_close(void *ctx, uintptr_t fh, int *delivered, int32_t *io)
{
    int closed;

    (void)ctx;
    closed = end_async((BPTR)fh, delivered);
    *io = (int32_t)IoErr();
    return closed;
}

static int be_list_open(void *ctx, uintptr_t lock, uintptr_t *list,
                        int *transport_ok, int32_t *io)
{
    struct dos_list *item = AllocVec(sizeof(*item), MEMF_PUBLIC | MEMF_CLEAR);

    (void)ctx;
    *transport_ok = 1;
    *io = 0;
    if (!item) {
        *io = ERROR_NO_FREE_STORE;
        return 0;
    }
    item->fib = AllocDosObject(DOS_FIB, NULL);
    if (!item->fib) {
        FreeVec(item);
        *io = ERROR_NO_FREE_STORE;
        return 0;
    }
    if (!examine_lock_async((BPTR)lock, item->fib, transport_ok)) {
        *io = (int32_t)IoErr();
        FreeDosObject(DOS_FIB, item->fib);
        FreeVec(item);
        return 0;
    }
    if (item->fib->fib_DirEntryType < 0) {
        *io = ERROR_OBJECT_WRONG_TYPE;
        FreeDosObject(DOS_FIB, item->fib);
        FreeVec(item);
        return 0;
    }
    item->lock = (BPTR)lock;
    *list = (uintptr_t)item;
    return 1;
}

static int be_list_next(void *ctx, uintptr_t list, struct zzrun_info *info,
                        int *transport_ok, int32_t *io)
{
    struct dos_list *item = (struct dos_list *)list;

    (void)ctx;
    if (!examine_next_async(item->lock, item->fib, transport_ok)) {
        *io = (int32_t)IoErr();
        return 0;
    }
    info_from_fib(item->fib, info);
    return 1;
}

static void be_list_close(void *ctx, uintptr_t list)
{
    struct dos_list *item = (struct dos_list *)list;

    (void)ctx;
    if (item->fib)
        FreeDosObject(DOS_FIB, item->fib);
    if (item->lock)
        UnLock(item->lock);
    FreeVec(item);
}

static const struct zzrun_dos dos_ops = {
    .ctx = NULL,
    .live = be_live,
    .user_break = be_user_break,
    .dup_lock = be_dup_lock,
    .unlock = be_unlock,
    .parent_dir = be_parent_dir,
    .same_lock = be_same_lock,
    .lock_rel = be_lock_rel,
    .lock_abs = be_lock_abs,
    .lock_examine = be_lock_examine,
    .target_begin = be_target_begin,
    .target_probe = be_target_probe,
    .target_open = be_target_open,
    .target_end = be_target_end,
    .fh_set_size0 = be_fh_set_size0,
    .fh_read = be_fh_read,
    .fh_write = be_fh_write,
    .fh_seek = be_fh_seek,
    .fh_end_or_close = be_fh_end_or_close,
    .fh_close = be_fh_close,
    .list_open = be_list_open,
    .list_next = be_list_next,
    .list_close = be_list_close,
};

static void push_event(uint16_t type, uint16_t code, int16_t dx, int16_t dy,
                       uint16_t buttons)
{
    if (!claim_live())
        return;
    (void)zzrun_events_push(&events, type, code, dx, dy, buttons, now_ms());
}

static void stop_owned(void)
{
    if (claim_live())
        (void)claim_note(AARStop(aar_handle));
}

/* A stop: the stop event first, which ZZRun keeps asking for until the
 * queue takes it, then AARStop (the stop request word, the stop event and
 * the platform's stop). */
static void request_stop(void)
{
    if (stop_asked)
        return;
    stop_asked = 1;
    stop_ms = now_ms();
    if (!stop_event_sent) {
        push_event(AAR_EV_STOP, 0, 0, 0, 0);
        stop_event_sent = 1;
    }
    stop_owned();
}

static uint16_t break_signal_for_rawkey(uint16_t code, uint16_t qualifiers)
{
    if (!(qualifiers & IEQUALIFIER_CONTROL))
        return 0;
    switch (code) {
    case 0x22u: return AAR_BREAK_CTRL_D;
    case 0x12u: return AAR_BREAK_CTRL_E;
    case 0x23u: return AAR_BREAK_CTRL_F;
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
            uint16_t type = (code & IECODE_UP_PREFIX) ? AAR_EV_KEY_UP
                                                     : AAR_EV_KEY_DOWN;
            uint16_t signal = type == AAR_EV_KEY_DOWN ?
                break_signal_for_rawkey(raw, qual) : 0;

            if (type == AAR_EV_KEY_DOWN &&
                zzrun_rawkey_requests_stop(raw, qual, IEQUALIFIER_CONTROL)) {
                user_stop = 1;
                request_stop();
            }
            push_event(type, raw, 0, 0, qual);
            if (signal)
                push_event(AAR_EV_BREAK, signal, 0, 0, qual);
        }
        if (class & IDCMP_MOUSEBUTTONS) {
            uint16_t id = 0;
            if (code == SELECTDOWN) { mouse_buttons |= 1u; id = 1; }
            else if (code == SELECTUP) { mouse_buttons &= (uint16_t)~1u; id = 1; }
            else if (code == MENUDOWN) { mouse_buttons |= 2u; id = 2; }
            else if (code == MENUUP) { mouse_buttons &= (uint16_t)~2u; id = 2; }
            else if (code == MIDDLEDOWN) { mouse_buttons |= 4u; id = 3; }
            else if (code == MIDDLEUP) { mouse_buttons &= (uint16_t)~4u; id = 3; }
            push_event(AAR_EV_MOUSE, id, 0, 0, mouse_buttons);
        }
        if (class & IDCMP_MOUSEMOVE) {
            int16_t dx = (int16_t)mx;
            int16_t dy = (int16_t)my;

            /* The library sums consecutive motion into one pending event. */
            if (dx || dy)
                push_event(AAR_EV_MOUSE, 0, dx, dy, mouse_buttons);
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
        push_event(AAR_EV_JOY, (uint16_t)port, dx, dy, fire ? 1u : 0u);
    }
}

static void launcher_status(const char *msg)
{
    zzrun_console_launcher_status(&serve.console, msg);
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
        zzrun_console_launcher_put(&serve.console, (const uint8_t *)msg,
                                   (uint32_t)(len < (int)sizeof(msg) ? len :
                                              (int)sizeof(msg) - 1),
                                   0);
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
static struct MsgPort *con_port;
static struct StandardPacket *con_packet;
static UBYTE *con_buffer;
static BPTR con_fh;
static int con_pending;
static int con_interactive;
static struct StandardPacket *con_write_packet;
static UBYTE *con_write_buffer;
static int con_write_pending;
static int con_write_interactive;


static void console_reap_packets(void);
static int console_write_start(BPTR fh, const uint8_t *data, uint32_t bytes);
static void console_write_fh(BPTR fh, const char *msg);

static void console_output_error(void)
{
    request_stop();
    if (!console_output_failed)
        console_write_fh(console_output(),
                         "ZZRun: cannot write app console output.\n");
    console_output_failed = 1;
}


/*
 * Input stays asynchronous for every handle. A keyboard read uses the
 * console's normal line mode, so the Shell keeps echo, line editing and
 * Ctrl-C/D/E/F. A pipe or file uses the same ACTION_READ: a blocking Read
 * would stop the main loop, display and audio while the writer stays open.
 * A pending read returns a line on Return and 0 bytes on Ctrl-\. It starts
 * only for an outstanding CONSOLE_READ request, while the app is RUNNING;
 * the request is the app's demand for input. Synchronous DOS calls keep
 * using pr_MsgPort, so their replies never meet this packet.
 */

static int console_read_start(BPTR fh, uint32_t max)
{
    struct FileHandle *handle = (struct FileHandle *)BADDR(fh);

    if (!handle || !handle->fh_Type || max == 0 ||
        max > ZZRUN_CONSOLE_READ_BYTES)
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
    con_packet->sp_Pkt.dp_Arg3 = (LONG)max;
    con_fh = fh;
    con_interactive = IsInteractive(fh);
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

static int console_write_start(BPTR fh, const uint8_t *data, uint32_t bytes)
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
    con_write_interactive = IsInteractive(fh);
    con_write_pending = 1;
    PutMsg(handle->fh_Type, &con_write_packet->sp_Msg);
    return 1;
}

static void console_write_complete(void)
{
    con_write_pending = 0;
    zzrun_console_write_done(&serve.console,
                             (int32_t)con_write_packet->sp_Pkt.dp_Res1);
}

static void console_read_complete(void)
{
    LONG got = con_packet->sp_Pkt.dp_Res1;

    con_pending = 0;
    zzrun_console_read_done(&serve.console, (int32_t)got, con_buffer);
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
static void console_read_finish(void)
{
    int input_noted = 0;
    int output_noted = 0;

    while (con_pending || con_write_pending ||
           (!serve.console.failed && serve.console.launcher_used)) {
        console_reap_packets();
        zzrun_console_pump(&serve.console);
        if (!con_pending && !con_write_pending &&
            (serve.console.failed || !serve.console.launcher_used))
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
    con_buffer = NULL;
    con_packet = NULL;
    con_port = NULL;
    con_fh = 0;
    con_interactive = 0;
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

static int ahi_reap(void)
{
    struct Message *message;
    unsigned i;
    int completed = 0;

    while ((message = GetMsg(ahi_port)) != NULL) {
        for (i = 0u; i < ZZRUN_AHI_BUFFER_COUNT; i++) {
            if (ahi_request[i] &&
                message == &ahi_request[i]->ahir_Std.io_Message) {
                if (ahi_pending[i]) {
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

/* The frames AARAudioRead could hand over now: a read with no buffer and no
 * frames returns the readable count and does not count an underrun. */
static uint32_t audio_available(void)
{
    ULONG got = 0;

    if (!claim_live() ||
        claim_note(AARAudioRead(aar_handle, NULL, 0, &got)) != AAR_OK)
        return 0u;
    return got;
}

static int ahi_idle(void)
{
    unsigned i;

    for (i = 0u; i < ZZRUN_AHI_BUFFER_COUNT; i++) {
        if (ahi_pending[i])
            return 0;
    }
    return 1;
}

/* Moves PCM from the app's audio ring to AHI: one buffer of at most 1/50 s
 * per pass, the way the earlier launcher did. AARAudioRead copies the
 * ring's little-endian words into samples in CPU order. */
static void service_ahi_audio(void)
{
    uint32_t available, frames;
    ULONG got = 0;
    uint32_t bytes;
    int completed;
    int slot;

    if (!serve.audio.open || !ahi_request[0])
        return;
    completed = ahi_reap();
    available = audio_available();
    frames = zzrun_audio_chunk_frames(available, ahi_rate, ahi_channels,
                                      ZZRUN_AHI_BUFFER_BYTES);
    if (frames == 0u) {
        /* An underrun: a buffer just finished and AHI has nothing queued.
         * A read that finds less than asked counts it in the ring header. */
        if (completed && available == 0u && ahi_idle())
            (void)claim_note(AARAudioRead(aar_handle, ahi_buffer[0], 1u,
                                          &got));
        return;
    }
    slot = ahi_free_slot();
    if (slot < 0)
        return;
    if (claim_note(AARAudioRead(aar_handle, ahi_buffer[slot], frames,
                                &got)) != AAR_OK || got == 0u)
        return;
    bytes = got * ahi_channels * 2u;
    ahi_submit((unsigned)slot, bytes);
}

static void service_console(void)
{
    console_reap_packets();
    zzrun_console_pump(&serve.console);
}

/* Tokens are board offsets: AARSharedToken turns the address of a P96 bitmap
 * into the token the card understands and refuses an address outside the
 * card's shared aperture. ZZRun also checks the framebuffer window the card
 * accepts for a scanout, so a bitmap outside it is refused here. */
#define ZZRUN_TOKEN_MIN 0x00010000UL
#define ZZRUN_TOKEN_END 0x06010000UL

static int bitmap_token(struct BitMap *bm, ULONG *token, ULONG *pitch)
{
    APTR mem;
    ULONG bytes, rows;
    if (!bm)
        return 0;
    mem = (APTR)p96GetBitMapAttr(bm, P96BMA_MEMORY);
    if (!mem)
        mem = bm->Planes[0];
    bytes = (ULONG)p96GetBitMapAttr(bm, P96BMA_BYTESPERROW);
    if (!bytes)
        bytes = bm->BytesPerRow;
    if (!bytes)
        return 0;
    rows = bm->Rows ? (ULONG)bm->Rows : 1UL;
    if (!claim_live() ||
        claim_note(AARSharedToken(aar_handle, mem, bytes * rows, token)) !=
            AAR_OK)
        return 0;
    if (*token < ZZRUN_TOKEN_MIN || *token >= ZZRUN_TOKEN_END)
        return 0;
    *pitch = bytes;
    return 1;
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
        if (serve.display.showing && ChangeScreenBuffer(screen, buf_front)) {
            flip_safe_pending = zzrun_screen_safe_started(flip_safe_pending);
            serve.display.showing = 0;
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
    serve.display.showing = 0;
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
    ULONG depth = zzrun_display_depth(format);
    ULONG formats = format == AAR_FMT_BGRA8888 ? RGBFF_B8G8R8A8 :
                    (format == AAR_FMT_CLUT8 ? RGBFF_CLUT : RGBFF_R5G6B5);
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

/* Opens the P96 screen for a DISPLAY_OPEN and finds the tokens of its bitmap
 * memory (pages of one tall screen when it pans). */
static int open_screen(uint32_t w, uint32_t h, uint32_t format,
                       ULONG forced_mode, ULONG *pitch_out, ULONG *front_out,
                       ULONG *back_out, ULONG *spare_out)
{
    ULONG mode, depth, rgb, pitch = 0, front = 0, back = 0, spare = 0;
    if (w == 0 || h == 0 || w > ZZRUN_SCREEN_MAX || h > ZZRUN_SCREEN_MAX)
        return 0;
    if (format != AAR_FMT_RGB565 && format != AAR_FMT_BGRA8888 &&
        format != AAR_FMT_CLUT8)
        return 0;
    if (!P96Base || !IntuitionBase || !GfxBase)
        return 0;
    if (!pub_name[0])
        GetDefaultPubScreen((STRPTR)pub_name);
    depth = zzrun_display_depth(format);
    rgb = format == AAR_FMT_BGRA8888 ? RGBFB_B8G8R8A8 :
          (format == AAR_FMT_CLUT8 ? RGBFB_CLUT : RGBFB_R5G6B5);
    mode = mode_for(w, h, format, forced_mode);
    if (mode == (ULONG)INVALID_ID)
        return 0;
    if (!claim_check())
        return 0;
    screen = open_p96_screen(mode, w, h, depth, rgb);
    if (!screen)
        return 0;
    if (!bitmap_token(screen->RastPort.BitMap, &front, &pitch)) {
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

            if (!bitmap_token(buf_back->sb_BitMap, &back, &back_pitch)) {
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
            bitmap_token(tall->RastPort.BitMap, &tall_front, &tall_pitch)) {
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
                bitmap_token(tall->RastPort.BitMap, &tall_front, &tall_pitch)) {
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
    *pitch_out = pitch;
    *front_out = front;
    *back_out = back;
    *spare_out = spare;
    flip_pending = 0;
    flip_safe_pending = 0;
    return 1;
}

/* A SafeMessage of a ChangeScreenBuffer flip came back: the old buffer may be
 * drawn into again, so the app's flip completes. */
static void service_display(void)
{
    while (flip_safe_port && flip_safe_pending && GetMsg(flip_safe_port)) {
        flip_safe_pending = zzrun_screen_safe_completed(flip_safe_pending);
        zzrun_display_flip_safe(&serve.display);
    }
}

/* DISPLAY_OPEN: opens the screen, finds the bitmaps' tokens and reports them
 * in the completion. The card binds its scanout to them when it sees it. */
static void open_display(const struct zzrun_req *req, struct zzrun_done *done)
{
    ULONG pitch = 0, front = 0, back = 0, spare = 0;

    if (!open_screen(req->arg[AAR_A_DISPLAY_OPEN_W],
                     req->arg[AAR_A_DISPLAY_OPEN_H],
                     req->arg[AAR_A_DISPLAY_OPEN_FORMAT], display_forced_mode,
                     &pitch, &front, &back, &spare)) {
        launcher_status("ZZRun: cannot open the requested screen");
        display_failed = 1;
        request_stop();
        memset(done, 0, sizeof(*done));
        done->result = AAR_E_IO;
        return;
    }
    zzrun_display_opened(&serve.display, req, pitch, front, back, spare,
                         have_two, have_pan, (uint32_t)pan_count, done);
}

/*
 * A DOS packet may block on slow media. ZZRun waits on a reply port it owns
 * together with its poll timer and Ctrl-C, asking the library on each tick
 * whether the claim is still live (the library renews the lease itself).
 * Only the reply ends the wait: packet and buffers stay alive until then,
 * even after Ctrl-C or a lost claim.
 */
static int dos_packet_async(struct MsgPort *handler, LONG action, LONG arg1,
                            LONG arg2, LONG arg3, LONG arg4, LONG arg5,
                            int check_live, LONG *result_out,
                            int *ownership_lost, int *abandoned_out)
{
    struct MsgPort *port;
    struct StandardPacket *packet;
    struct Message *message;
    ULONG mask;
    ULONG signals;
    int abandoned = 0;
    int ctrl_c;
    int owned;

    if (result_out)
        *result_out = -1;
    if (ownership_lost)
        *ownership_lost = 0;
    if (abandoned_out)
        *abandoned_out = 0;
    if (!handler || !result_out)
        return 0;
    /* A packet whose operation is already lost is not sent at all: the
     * handler would act on it (create, truncate, write) before the result
     * is discarded as abandoned. Releases always go out. */
    ctrl_c = ctrl_c_set();
    owned = !check_live || claim_live();
    if (!zzrun_packet_may_send(action == ACTION_END ||
                               action == ACTION_FREE_LOCK, owned, ctrl_c)) {
        if (ctrl_c) {
            SetSignal(0, SIGBREAKF_CTRL_C);
            user_stop = 1;
            if (zzrun_ctrl_c_must_request_stop(1))
                request_stop();
        }
        if (!owned && ownership_lost)
            *ownership_lost = 1;
        *result_out = 0;
        if (abandoned_out)
            *abandoned_out = 1;
        SetIoErr(ERROR_BREAK);
        return 1;
    }
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
            if (zzrun_packet_abandoned(0, claim_check()) && check_live) {
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
    if (check_live && !claim_live()) {
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
                          arg3, arg4, arg5, 1, &result, NULL,
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
                          1, &result, NULL, &abandoned) ||
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
                          1, &result, NULL, &abandoned) ||
        !zzrun_packet_result_usable(1, abandoned))
        return 0;
    *transport_ok = 1;
    return zzrun_dos_boolean_result(1, result);
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
                          1, &result, NULL, &abandoned))
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

/* Locks name through resolver: every GetDeviceProc candidate, including
 * late-binding, nonbinding, and multi-directory assigns, or the one fixed
 * candidate of a pinned resolver. selected (optional) receives the candidate
 * that decided the answer. The caller releases the resolver. */
static BPTR lock_resolved_async(struct dos_path_resolver *resolver,
                                const char *name,
                                struct zzrun_dos_path_candidate *selected,
                                int *transport_ok)
{
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
    memset(&request, 0, sizeof(request));
    request.access = ACCESS_READ;
    (void)zzrun_dos_dispatch_path(name, name_bstr, 258u,
                                  ERROR_OBJECT_NOT_FOUND, resolve_dos_path,
                                  resolver, send_lock_path_packet, &request,
                                  NULL, &usable, selected);
    FreeVec(name_bstr);
    if (usable)
        *transport_ok = 1;
    if (!request.lock && usable)
        SetIoErr(request.error);
    return request.lock;
}

/* Absolute (ABS tooltype) paths use every GetDeviceProc candidate. */
static BPTR lock_abs_async(const char *name, int *transport_ok)
{
    struct dos_path_resolver resolver;
    BPTR lock;
    LONG io;

    memset(&resolver, 0, sizeof(resolver));
    resolver.name = name;
    lock = lock_resolved_async(&resolver, name, NULL, transport_ok);
    io = IoErr();
    release_dos_path_resolver(&resolver);
    SetIoErr(io);
    return lock;
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
                          1, &result, NULL, &abandoned)) {
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

/* A resolver for name: one fixed candidate on dir's handler when dir is a
 * lock, GetDeviceProc candidates when dir is 0. Returns 0 for a lock that
 * has no handler. */
static int dos_path_resolver_start(struct dos_path_resolver *resolver,
                                   BPTR dir, const char *name)
{
    struct FileLock *file_lock = (struct FileLock *)BADDR(dir);

    memset(resolver, 0, sizeof(*resolver));
    if (dir) {
        if (!file_lock || !file_lock->fl_Task)
            return 0;
        resolver->handler = file_lock->fl_Task;
        resolver->directory = dir;
    } else {
        resolver->name = name;
    }
    return 1;
}

static BPTR open_resolved_async(struct dos_path_resolver *resolver,
                                const char *name, LONG mode,
                                int *transport_ok)
{
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
    memset(&request, 0, sizeof(request));
    if (mode == MODE_NEWFILE)
        request.action = ACTION_FINDOUTPUT;
    else if (mode == MODE_READWRITE)
        request.action = ACTION_FINDUPDATE;
    else
        request.action = ACTION_FINDINPUT;
    (void)zzrun_dos_dispatch_path(name, name_bstr, 258u,
                                  ERROR_OBJECT_NOT_FOUND, resolve_dos_path,
                                  resolver, send_open_path_packet, &request,
                                  NULL, &usable, NULL);
    FreeVec(name_bstr);
    if (usable)
        *transport_ok = 1;
    if (!request.fh && usable)
        SetIoErr(request.error);
    return request.fh;
}

/* Finds the ELF: a lock on the file, searched from dir (a lock) or through
 * every GetDeviceProc candidate of a volume or assign name. */
static BPTR lock_find_async(BPTR dir, const char *name, int *transport_ok)
{
    struct dos_path_resolver resolver;
    BPTR lock;
    LONG io;

    if (transport_ok)
        *transport_ok = 0;
    if (!name || !transport_ok)
        return 0;
    if (!dos_path_resolver_start(&resolver, dir, name))
        return 0;
    lock = lock_resolved_async(&resolver, name, NULL, transport_ok);
    io = IoErr();
    release_dos_path_resolver(&resolver);
    SetIoErr(io);
    return lock;
}

struct file_handle_release {
    BPTR fh;
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
                                 0, 0, 0, 0, 1, &packet_result,
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
    ops.forget_slot = NULL;
    return ops;
}

static int end_async(BPTR fh, int *delivered_out)
{
    struct file_handle_release release;
    struct zzrun_dos_release_ops ops;

    release.fh = fh;
    ops = file_handle_release_ops(&release);
    return zzrun_dos_close_slot(&ops, delivered_out);
}

static void end_or_close(BPTR fh)
{
    struct file_handle_release release;
    struct zzrun_dos_release_ops ops;

    release.fh = fh;
    ops = file_handle_release_ops(&release);
    zzrun_dos_end_or_close(&ops);
}

/* ---- Serving the app's requests ----------------------------------------- */

struct pending_reply {
    struct zzrun_req req;
    struct zzrun_done done;
};

static struct pending_reply reply_backlog[ZZRUN_REPLY_BACKLOG];
static unsigned reply_backlog_n;

static int post_reply(const struct zzrun_req *q, const struct zzrun_done *d)
{
    AARServiceReq r;
    unsigned i;

    memset(&r, 0, sizeof(r));
    r.size = sizeof(r);
    r.op = q->op;
    r.id = q->id;
    r.result = d->result;
    r.reply_flags = d->flags;
    for (i = 0; i < AAR_ENTRY_OUTS; i++)
        r.out[i] = d->out[i];
    return (int)claim_note(AARServiceReply(aar_handle, &r));
}

/* Completions that found the CQ full wait here until the app has drained it. */
static void reply_flush(void)
{
    unsigned i = 0;

    while (i < reply_backlog_n) {
        if (post_reply(&reply_backlog[i].req, &reply_backlog[i].done) ==
            AAR_E_BUSY)
            break;
        i++;
    }
    if (i) {
        memmove(reply_backlog, reply_backlog + i,
                (reply_backlog_n - i) * sizeof(reply_backlog[0]));
        reply_backlog_n -= i;
    }
}

static void reply_request(const struct zzrun_req *q, const struct zzrun_done *d)
{
    if (reply_backlog_n == 0 && post_reply(q, d) != AAR_E_BUSY)
        return;
    if (reply_backlog_n < ZZRUN_REPLY_BACKLOG) {
        reply_backlog[reply_backlog_n].req = *q;
        reply_backlog[reply_backlog_n].done = *d;
        reply_backlog_n++;
        return;
    }
    launcher_status("ZZRun: completion queue full; a completion was lost.");
}

static void sink_complete(void *ctx, const struct zzrun_req *req,
                          const struct zzrun_done *done)
{
    (void)ctx;
    reply_request(req, done);
}

static void service_requests(void)
{
    unsigned served = 0;
    AARServiceReq r;
    LONG rc = AAR_OK;

    reply_flush();
    for (;;) {
        struct zzrun_req q;
        struct zzrun_done d;
        unsigned i;

        if (served >= 16u || !zzrun_reply_may_take_request(reply_backlog_n))
            break;
        memset(&r, 0, sizeof(r));
        r.size = sizeof(r);
        rc = claim_note(AARServiceNext(aar_handle, &r));
        if (rc != AAR_OK)
            break;
        served++;
        memset(&q, 0, sizeof(q));
        q.op = (uint16_t)r.op;
        q.flags = (uint16_t)r.flags;
        q.id = r.id;
        for (i = 0; i < AAR_ENTRY_ARGS; i++)
            q.arg[i] = r.arg[i];
        q.buf_token = r.buf;
        q.buf_off = r.buf_off;
        q.buf_len = r.buf_len;
        q.buf = (uint8_t *)r.ptr;
        memset(&d, 0, sizeof(d));
        if (zzrun_serve_request(&serve, &q, &d) == ZZRUN_SERVE_DONE)
            reply_request(&q, &d);
    }
    if (rc == AAR_E_IO && !queue_corrupt) {
        queue_corrupt = 1;
        launcher_status("ZZRun: the request queue is corrupt; stopping the app.");
        request_stop();
    }
}

static const struct zzrun_sink serve_sink = { NULL, sink_complete };

static int con_op_open(void *ctx)
{
    (void)ctx;
    return console_open_if_needed();
}

static int con_op_write_start(void *ctx, const uint8_t *data, uint32_t bytes)
{
    (void)ctx;
    return console_write_start(console_output(), data, bytes);
}

static int con_op_read_start(void *ctx, uint32_t max)
{
    (void)ctx;
    return console_read_start(console_input(), max);
}

static int con_op_read_allowed(void *ctx)
{
    (void)ctx;
    return zzrun_console_read_allowed(app_state, stop_asked);
}

static void con_op_output_error(void *ctx)
{
    (void)ctx;
    console_output_error();
}

static int con_op_live(void *ctx)
{
    (void)ctx;
    return claim_live();
}

static const struct zzrun_console_ops console_ops = {
    NULL, con_op_open, con_op_write_start, con_op_read_start,
    con_op_read_allowed, con_op_live, con_op_output_error
};

static void display_op_open(void *ctx, const struct zzrun_req *req,
                            struct zzrun_done *done)
{
    (void)ctx;
    open_display(req, done);
}

static int display_op_change_buffer(void *ctx, int to_back)
{
    (void)ctx;
    if (!screen || !buf_front || !buf_back ||
        !ChangeScreenBuffer(screen, to_back ? buf_back : buf_front))
        return 0;
    flip_safe_pending = zzrun_screen_safe_started(flip_safe_pending);
    return 1;
}

static void display_op_pan(void *ctx, uint32_t page)
{
    (void)ctx;
    screen->ViewPort.RasInfo->RyOffset = (WORD)(page * serve.display.height);
    ScrollVPort(&screen->ViewPort);
}

static void display_op_wait_tof(void *ctx)
{
    (void)ctx;
    WaitTOF();
}

static void display_op_load_rgb32(void *ctx, const uint32_t *table)
{
    (void)ctx;
    LoadRGB32(&screen->ViewPort, (ULONG *)table);
}

static void display_op_close(void *ctx)
{
    (void)ctx;
    close_display();
}

static const struct zzrun_display_ops display_ops = {
    NULL, display_op_open, display_op_change_buffer, display_op_pan,
    display_op_wait_tof, display_op_load_rgb32, display_op_close
};

/* The audio ring the app writes: the library attaches it when ZZRun completes
 * the AUDIO_OPEN request with AAR_OK and detaches it at AUDIO_CLOSE, so ZZRun
 * only opens and closes AHI here. */
static int audio_op_open(void *ctx, uint32_t rate, uint32_t channels,
                         uint32_t token, uint32_t bytes, uint32_t frames)
{
    (void)ctx;
    (void)token;
    (void)bytes;
    (void)frames;
    if (!ahi_open(rate, channels)) {
        ahi_close();
        /* An AHI open failure on the AUDIO=AHI route ends the run with an
         * error, as before. */
        launcher_status("ZZRun: cannot open AHI for AUDIO=AHI.");
        audio_open_failed = 1;
        request_stop();
        return 0;
    }
    return 1;
}

static void audio_op_close(void *ctx)
{
    (void)ctx;
    ahi_close();
}

static const struct zzrun_audio_ops audio_ops = {
    NULL, audio_op_open, audio_op_close
};

static int events_op_post(void *ctx, const aar_event *ev)
{
    (void)ctx;
    return (int)claim_note(AARPostEvent(aar_handle, ev));
}

static const struct zzrun_events_ops events_ops = { NULL, events_op_post };

static void log_sink(APTR user, const void *data, ULONG len)
{
    (void)user;
    zzrun_console_launcher_log(&serve.console, (const uint8_t *)data, len);
}

/* Wires the handlers to the DOS, console, display and audio backends. */
static void serve_setup(void)
{
    zzrun_serve_init(&serve, &serve_sink);
    zzrun_files_init(&serve.files, &dos_ops, (uintptr_t)root_lock, abs_paths);
    zzrun_console_init(&serve.console, &console_ops, &serve.sink);
    zzrun_display_init(&serve.display, &display_ops, &serve.sink);
    zzrun_audio_init(&serve.audio, &audio_ops);
    zzrun_events_init(&events, &events_ops);
    if (zz_ext->set_log_sink)
        (void)zz_ext->set_log_sink(aar_handle, log_sink, NULL);
}

/* argv[0] is the app's file name, as C programs expect; the Shell or ARGS
 * tokens follow it. */
static int start_app(void)
{
    const char *argv_list[ZZRUN_ARGV_MAX];
    char line[160];
    int i;
    LONG rc;

    argv_list[0] = (const char *)FilePart((STRPTR)elf_path);
    for (i = 0; i < app_argc; i++)
        argv_list[i + 1] = arg_store[i];
    rc = claim_note(AARStart(aar_handle, argv_list, (ULONG)app_argc + 1u));
    if (rc != AAR_OK) {
        zzrun_failure_line(line, sizeof(line), "start", rc,
                           rc == AAR_E_NORESOURCE ? claim_reason() :
                                                    AAR_REASON_NONE);
        puts(line);
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
    launcher_statusf("  exit=%ld generation=%08lx\n", (long)exit_code,
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

static int run_loop(void)
{
    int entered = 1;
    int saw_fault = 0;
    int ownership_lost = 0;
    AARStatusInfo st;
    uint32_t state, end_reason;

    timer_arm();
    for (;;) {
        ULONG mask = SIGBREAKF_CTRL_C | SIGBREAKF_CTRL_D | SIGBREAKF_CTRL_E |
                     SIGBREAKF_CTRL_F;
        ULONG signals;

        if (timer_port)
            mask |= 1UL << timer_port->mp_SigBit;
        if (con_port)
            mask |= 1UL << con_port->mp_SigBit;
        if (input_win && input_win->UserPort)
            mask |= 1UL << input_win->UserPort->mp_SigBit;
        signals = Wait(mask);
        if (timer_port && (signals & (1UL << timer_port->mp_SigBit)))
            timer_reap();
        /* The library renews the lease from its own interrupt. A claim
         * whose handle went stale is not served any more. */
        if (!claim_live()) {
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
            push_event(AAR_EV_BREAK, AAR_BREAK_CTRL_D, 0, 0, 0);
        if (signals & SIGBREAKF_CTRL_E)
            push_event(AAR_EV_BREAK, AAR_BREAK_CTRL_E, 0, 0, 0);
        if (signals & SIGBREAKF_CTRL_F)
            push_event(AAR_EV_BREAK, AAR_BREAK_CTRL_F, 0, 0, 0);
        drain_window(input_win);
        poll_joysticks();
        zzrun_events_flush(&events);
        /* The library flushes a pending coalesced mouse event itself. */
        service_requests();
        service_console();
        service_display();
        service_ahi_audio();
        reply_flush();
        /* The claim may have been revoked while a request was served. One
         * small status read gives the state and the end reason; any failure
         * of it means the claim is gone. */
        if (claim_state(&state, &end_reason) != AAR_OK) {
            ownership_lost = 1;
            break;
        }
        app_state = state;
        if (state == ZZRUN_ST_FAULTED)
            saw_fault = 1;
        {
            struct zzrun_exit_in leave;

            memset(&leave, 0, sizeof(leave));
            leave.state = state;
            leave.end_reason = end_reason;
            leave.elapsed_ms = stop_asked ? now_ms() - stop_ms : 0u;
            leave.entered = entered;
            leave.stop_asked = stop_asked;
            leave.user_stop = user_stop;
            leave.output_pending =
                zzrun_console_output_pending(&serve.console);
            leave.output_failed = console_output_failed;
            if (zzrun_run_should_exit(&leave))
                break;
        }
        timer_arm();
    }
    timer_reap();
    if (ownership_lost) {
        launcher_statusf("ZZRun: claim lost (%s).\n",
                         zzrun_claim_loss_text(1, AAR_END_NONE));
        return 20;
    }
    memset(&st, 0, sizeof(st));
    st.size = sizeof(st);
    if (claim_note(AARStatus(aar_handle, &st)) != AAR_OK) {
        launcher_status("ZZRun: the app's final status could not be read.");
        return 20;
    }
    if (st.fault_class || saw_fault)
        report_fault(st.fault_class, st.exit_code, st.fault_pc.lo,
                     st.fault_sp.lo, st.fault_lr.lo, st.fault_addr.lo,
                     st.fault_syndrome, st.fault_pstate, st.generation);
    else if (zzrun_claim_loss_text(0, st.end_reason))
        launcher_statusf("ZZRun: claim lost (%s).\n",
                         zzrun_claim_loss_text(0, st.end_reason));
    else
        launcher_statusf("ZZRun: exit %ld (%s)\n", (long)st.exit_code,
                         state_name(st.state));
    return zzrun_process_exit(
        zzrun_app_exit_code(st.end_reason, st.exit_code, stop_asked),
        display_failed, console_output_failed, audio_open_failed,
        queue_corrupt);
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
    timer_reap();
    if (claim_open && !stop_asked && claim_live()) {
        uint32_t leaving, leaving_end;

        if (claim_state(&leaving, &leaving_end) == AAR_OK &&
            leaving != AAR_STATE_IDLE)
            stop_owned();
    }
    ahi_close();
    /* A pending console read cannot be cancelled. Close the app screen
     * before that wait so the prompt is not hidden behind it. */
    close_display();
    console_read_finish();
    if (console_handle) {
        Close(console_handle);
        console_handle = 0;
    }
    if (serve.files.dos)
        zzrun_files_close_all(&serve.files);
    if (root_lock) {
        UnLock(root_lock);
        root_lock = 0;
    }
    /* The library returns the control block, the queues, every service
     * buffer and every map at the close, so nothing above may touch them
     * afterwards. */
    if (claim_open) {
        (void)AARClose(aar_handle);
        aar_handle = NULL;
        claim_open = 0;
    }
    if (AARBase) {
        CloseLibrary(AARBase);
        AARBase = NULL;
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
    if (IntuitionBase) {
        CloseLibrary((struct Library *)IntuitionBase);
        IntuitionBase = NULL;
    }
    if (GfxBase) {
        CloseLibrary((struct Library *)GfxBase);
        GfxBase = NULL;
    }
    if (launcher_output_active) {
        launcher_output_active = 0;
        if (serve.console.launcher_dropped)
            printf("ZZRun: dropped %lu diagnostic log records.\n",
                   (unsigned long)serve.console.launcher_dropped);
    }
}

/* The claim gate before each load step: a claim the platform has already
 * revoked is not worth another step. The library renews the lease, so this
 * only asks whether the claim is still ours. */
static int claim_gone(void)
{
    return !claim_check();
}

/* The library's options, before the load (the old limits write and client
 * capabilities of LOAD_BEGIN). */
static int apply_options(void)
{
    struct TagItem tags[ZZRUN_TAG_MAX];
    char line[160];
    LONG rc;

    (void)zzrun_build_tags(tags, maxmem_mib, hostmem_mib, weight_value,
                           audio_ahi);
    rc = claim_note(AARSetTagList(aar_handle, tags));
    if (rc == AAR_OK)
        return 1;
    zzrun_tags_failure_line(line, sizeof(line), rc);
    puts(line);
    return 0;
}

/* The path AARLoadFile opens: the file ZZRun found, named in full. ZZRun
 * searches the assign or the current directory itself (every directory of a
 * multi-directory assign), so the library gets one name that Open() takes. */
static int resolve_elf_path(BPTR lock, char *out, size_t size)
{
    return NameFromLock(lock, (STRPTR)out, (LONG)size) != 0;
}

static void load_failed(LONG rc)
{
    char line[200];

    zzrun_load_failure_line(line, sizeof(line), rc,
                            rc == AAR_E_NORESOURCE ? claim_reason() :
                                                     AAR_REASON_NONE,
                            ctrl_c_set(), platform_info.max_image_bytes);
    puts(line);
}

int main(int argc, char **argv)
{
    char app_from_icon[256];
    char root_text[256];
    ULONG forced_mode = 0;
    int have_mode = 0;
    ULONG reason = 0;
    LONG status;
    enum zzrun_query_verdict verdict;
    BPTR lock = 0;
    int rc = 20;
    int show_status = 0;
    uint32_t release_handle = 0;

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
                           &hostmem_mib, &weight_value)) {
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
            if (!zzrun_parse_handle(argv[2], &release_handle)) {
                usage();
                cleanup();
                return 10;
            }
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
                {
                    int w = zzrun_weight_arg(argv[i], &weight_value);

                    if (w == ZZRUN_ARG_BAD) {
                        usage();
                        cleanup();
                        return 10;
                    }
                    if (w == ZZRUN_ARG_TAKEN)
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
    if (!show_status && !release_handle && !open_root(root_text)) {
        cleanup();
        return 20;
    }
    AARBase = OpenLibrary((CONST_STRPTR)AAR_LIB_NAME, AAR_LIB_VERSION);
    if (!AARBase) {
        tell("aar.library not found. Install Libs:aar.library from the "
             "ZZ9000 package.");
        cleanup();
        return 20;
    }
    memset(&platform_info, 0, sizeof(platform_info));
    platform_info.size = sizeof(platform_info);
    status = AARQuery(&platform_info);
    if (status != AAR_OK) {
        char line[160];

        zzrun_query_failure_line(line, sizeof(line), status);
        tell(line);
        cleanup();
        return 20;
    }
    if (!show_status && !release_handle)
        printf("ZZRun %s\n", ZZRUN_VERSION);
    if (!show_status)
        printf("runtime api %lu.%lu caps %08lx\n",
               (unsigned long)platform_info.api_major,
               (unsigned long)platform_info.api_minor,
               (unsigned long)platform_info.platform[AAR_ZZ9000_INFO_CAPS]);
    verdict = zzrun_query_check(&platform_info);
    if (verdict != ZZRUN_QUERY_OK) {
        tell(zzrun_query_verdict_text(verdict));
        cleanup();
        return 20;
    }
    zz_ext = (const aar_client_ext_zz9000 *)AARExt(AAR_PLATFORM_ZZ9000,
                                                   AAR_EXT_ZZ9000_CLIENT);
    if (!AAR_CLIENT_EXT_ZZ9000_COVERS(zz_ext, set_log_sink) ||
        !AAR_CLIENT_EXT_ZZ9000_COVERS(zz_ext, list) ||
        !AAR_CLIENT_EXT_ZZ9000_COVERS(zz_ext, release_handle)) {
        tell("aar.library has no ZZ9000 client extension.");
        cleanup();
        return 20;
    }
    if (show_status) {
        rc = print_status();
        cleanup();
        return rc;
    }
    if (release_handle) {
        rc = release_claim(release_handle);
        cleanup();
        return rc;
    }
    status = AAROpen(&aar_handle, NULL, &reason);
    if (status != AAR_OK) {
        char line[200];

        aar_handle = NULL;
        zzrun_claim_failure_line(line, sizeof(line), status, reason);
        puts(line);
        cleanup();
        return 20;
    }
    claim_open = 1;
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
    serve_setup();
    /* The options go to the library before the load. */
    if (!apply_options()) {
        cleanup();
        return zzrun_load_failure_exit(ctrl_c_set(), user_stop);
    }
    if (claim_gone()) {
        cleanup();
        return 20;
    }
    {
        BPTR base_dir = 0;
        int transport_ok;

        if (!strchr(elf_path, ':')) {
            base_dir = CurrentDir(0);
            CurrentDir(base_dir);
        }
        lock = lock_find_async(base_dir, elf_path, &transport_ok);
        if (!transport_ok || !lock) {
            printf("ZZRun: cannot open %s\n", elf_path);
            cleanup();
            return zzrun_load_failure_exit(ctrl_c_set(), user_stop);
        }
    }
    if (!resolve_elf_path(lock, elf_full, sizeof(elf_full))) {
        printf("ZZRun: cannot resolve the path of %s\n", elf_path);
        UnLock(lock);
        cleanup();
        return 20;
    }
    UnLock(lock);
    lock = 0;
    if (claim_gone()) {
        cleanup();
        return 20;
    }
    printf("loading %s\n", elf_full);
    status = claim_note(AARLoadFile(aar_handle, elf_full));
    if (status != AAR_OK) {
        load_failed(status);
        cleanup();
        return zzrun_load_failure_exit(ctrl_c_set(), user_stop);
    }
    if (!start_app()) {
        stop_owned();
        cleanup();
        return 20;
    }
    fflush(stdout);
    launcher_output_active = 1;
    display_forced_mode = have_mode ? forced_mode : 0;
    rc = run_loop();
    cleanup();
    return rc;
}
