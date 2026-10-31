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
#include <dos/dostags.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <graphics/gfxbase.h>
#include <graphics/modeid.h>
#include <workbench/startup.h>
#include <workbench/workbench.h>
#include <libraries/Picasso96.h>
#include <libraries/lowlevel.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/timer.h>
#include <proto/icon.h>
#include <proto/Picasso96.h>
#include <proto/lowlevel.h>
#include <proto/zz9k.h>
#include "zz9k/request.h"
#include "zzapp_proto.h"
#include "zz9000_hw.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

struct GfxBase *GfxBase;
struct IntuitionBase *IntuitionBase;
struct Library *IconBase;
struct Device *TimerBase;
struct Library *P96Base;
struct Library *ZZ9KBase;

extern struct WBStartup *_WBenchMsg;

#define ZZRUN_VERSION "0.1"
static const char version[] __attribute__((used)) =
    "$VER: ZZRun " ZZRUN_VERSION " (31.10.2026)\r\n";

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

#define ZZRUN_MIN_STACK     16384UL
#define ZZRUN_IMAGE_LIMIT   ((16u * 1024u * 1024u) + (256u * 1024u))
#define ZZRUN_GRACE_MS      500u
#define ZZRUN_POLL_US       20000UL
#define ZZRUN_VRAM_ARM      0x00200000UL
#define ZZRUN_VRAM_END      0x033F0000UL
#define ZZRUN_HEAP_END      0x08200000UL
#define ZZRUN_SCREEN_MAX    4096u
#define ZZRUN_UNIX_DELTA    252460800UL
#define ZZRUN_READ_CHUNK    8192u

static struct ZZ9000Board board;
static uint8_t *window;
static uint32_t window_bytes;
static int abs_paths;
static int stop_asked;
static int stop_event_sent;
static uint32_t stop_ms;
static uint32_t flip_acked;
static int have_two;
static int showing;
static ULONG pub_name_len;
static char pub_name[128];
static char elf_path[256];
static char arg_store[ZZAPP_ARGV_MAX][ZZAPP_ARGV_STR_MAX];
static int app_argc;
static BPTR root_lock;
static struct Screen *screen;
static struct Window *input_win;
static struct Window *close_win;
static struct ScreenBuffer *buf_front;
static struct ScreenBuffer *buf_back;
static struct BitMap *back_bm;
static ULONG card_front;
static ULONG card_back;
static ULONG bound_w, bound_h, bound_pitch, bound_fmt;
static uint16_t mouse_buttons;
static int mouse_valid;
static WORD mouse_x, mouse_y;
static ULONG joy_last[2];
static int joy_valid[2];
static struct MsgPort *timer_port;
static struct timerequest *timer_io;
static int timer_pending;

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

static void cleanup(void);

static int stack_ok(void)
{
    struct Task *task = FindTask(NULL);
    if (!task || (uintptr_t)task->tc_SPUpper <= (uintptr_t)task->tc_SPLower ||
        (uintptr_t)task->tc_SPUpper - (uintptr_t)task->tc_SPLower < ZZRUN_MIN_STACK)
        return 0;
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

static void life_put(uint32_t off, uint32_t value)
{
    win_cache(ZZAPP_LIFE_OFF + off, 4);
    zzapp_put_be32(window + ZZAPP_LIFE_OFF + off, value);
    win_cache(ZZAPP_LIFE_OFF + off, 4);
}

static uint32_t now_ms(void)
{
    struct timeval tv;
    GetSysTime(&tv);
    return (uint32_t)tv.tv_secs * 1000u + (uint32_t)(tv.tv_micro / 1000);
}

static void heartbeat(void)
{
    if (window)
        life_put(ZZAPP_LIFE_HEARTBEAT_OFF, now_ms());
}

static int ctrl_c_set(void)
{
    return (SetSignal(0, 0) & SIGBREAKF_CTRL_C) != 0;
}

static void usage(void)
{
    puts("ZZRun <app.elf> [args]\n"
         "Run Stack 32768 in this Shell first.\n"
         "Workbench ToolTypes: APP, ROOT, ABS, MODE, ARGS.\n"
         "ROOT defaults to the current directory, or the icon directory.");
}

static void tell(const char *msg)
{
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
    switch (state) {
    case ZZAPP_ST_IDLE: return "idle";
    case ZZAPP_ST_LOADING: return "loading";
    case ZZAPP_ST_STARTING: return "starting";
    case ZZAPP_ST_RUNNING: return "running";
    case ZZAPP_ST_PAUSED: return "paused";
    case ZZAPP_ST_STOPPING: return "stopping";
    case ZZAPP_ST_FAULTED: return "faulted";
    default: return "unknown";
    }
}

static int mailbox(uint16_t opcode, const uint8_t *payload, uint16_t len,
                   ZZ9KMailboxEntry *reply)
{
    ZZ9KRequest request;
    int status;

    if (!reply)
        return ZZ9K_STATUS_BAD_REQUEST;
    zz9k_request_init(&request, opcode);
    request.entry.payload_len = len;
    if (len && payload)
        memcpy(request.entry.payload.inline_data, payload, len);
    status = ZZ9KCall(&request, reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
    if (status == ZZ9K_STATUS_OK && reply->status != ZZ9K_STATUS_OK)
        status = reply->status;
    return status;
}

static int op_empty(uint16_t opcode, ZZ9KMailboxEntry *reply)
{
    return mailbox(opcode, NULL, 0, reply);
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
    if (n == 0)
        return 1;
    if (n > 127 || app_argc >= (int)ZZAPP_ARGV_MAX) {
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
                          int *have_mode)
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
    value = tool_value(icon, "ARGS");
    return !value || add_args_split(value);
}

static int load_program_icon(char *app_out, size_t app_n, char *root_out,
                             size_t root_n, ULONG *mode, int *have_mode)
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
                                have_mode);
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
    BPTR old = CurrentDir(*cur);
    BPTR child = Lock((STRPTR)name, ACCESS_READ);
    CurrentDir(old);
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

static int examine_lock(BPTR lock, struct FileInfoBlock *fib)
{
    if (!Examine(lock, fib))
        return 0;
    return 1;
}

static int lock_target(BPTR dir, const char *leaf, BPTR *out, int *err)
{
    BPTR old;
    BPTR locked;
    if (!leaf[0]) {
        *out = DupLock(dir);
        if (!*out) {
            *err = ZZ_EIO;
            return 0;
        }
        return 1;
    }
    old = CurrentDir(dir);
    locked = Lock((STRPTR)leaf, ACCESS_READ);
    CurrentDir(old);
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

static int path_from_wire(const struct zzapp_file_wire *req, char *out, int *err)
{
    uint32_t off = req->path_off;
    const uint8_t *src;
    size_t i;
    if (off < ZZAPP_PATH_OFF || off >= ZZAPP_PATH_OFF + ZZAPP_PATH_BYTES) {
        *err = ZZ_EINVAL;
        return 0;
    }
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

static void file_reply(uint32_t request_seq, int32_t fd, int32_t err,
                       uint32_t out_n, uint32_t mode, uint64_t out_pos,
                       uint32_t mtime)
{
    uint8_t *slot = window + ZZAPP_FILE_OFF;
    uint32_t done = request_seq + 2u;
    if (life_get(ZZAPP_LIFE_GENERATION_OFF) != file_generation)
        return;
    win_cache(ZZAPP_FILE_OFF, ZZAPP_FILE_BYTES);
    zzapp_put_be32(slot + offsetof(struct zzapp_file_wire, seq), request_seq + 1u);
    win_cache(ZZAPP_FILE_OFF, 4);
    zzapp_put_be32(slot + offsetof(struct zzapp_file_wire, fd), (uint32_t)fd);
    zzapp_put_be32(slot + offsetof(struct zzapp_file_wire, err), (uint32_t)err);
    zzapp_put_be32(slot + offsetof(struct zzapp_file_wire, out_n), out_n);
    zzapp_put_be32(slot + offsetof(struct zzapp_file_wire, mode), mode);
    zzapp_put_be64(slot + offsetof(struct zzapp_file_wire, out_pos), out_pos);
    zzapp_put_be32(slot + offsetof(struct zzapp_file_wire, mtime), mtime);
    win_cache(ZZAPP_FILE_OFF, ZZAPP_FILE_BYTES);
    zzapp_put_be32(slot + offsetof(struct zzapp_file_wire, seq), done);
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
        FreeMem(lists[index].fib, sizeof(struct FileInfoBlock));
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
    BPTR old;
    BPTR fh = 0;
    LONG mode = MODE_OLDFILE;
    int err = ZZ_OK;
    int index;
    int absolute = 0;
    struct FileInfoBlock fib;
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
    heartbeat();
    if (absolute) {
        fh = Open((STRPTR)path, mode);
    } else {
        dir = resolve_rel(path, leaf, sizeof(leaf), &err);
        if (!dir)
            goto done;
        if (!leaf[0]) {
            err = ZZ_EIO;
            goto done;
        }
        old = CurrentDir(dir);
        fh = Open((STRPTR)leaf, mode);
        CurrentDir(old);
    }
    if (!fh) {
        err = zz_from_io();
        if (err == ZZ_OK)
            err = ZZ_ENOENT;
        goto done;
    }
    files[index].fh = fh;
    files[index].used = 1;
    fh = 0;
    if (ExamineFH(files[index].fh, &fib)) {
        size = (uint32_t)fib.fib_Size;
        mtime = amiga_unix(&fib.fib_Date);
        if (fib.fib_DirEntryType > 0)
            mode_out = ZZAPP_MODE_DIR;
    }
done:
    if (fh)
        Close(fh);
    if (dir)
        UnLock(dir);
    if (err != ZZ_OK)
        close_fd_index(index);
    file_reply(req->seq, req->fd, err, 0, mode_out, size, mtime);
}

static void serve_close(const struct zzapp_file_wire *req)
{
    int err = ZZ_OK;
    int index = slot_index(req->fd, &err);
    if (index < 0 || !files[index].used)
        err = ZZ_EBADF;
    else
        close_fd_index(index);
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
        heartbeat();
        if (ctrl_c_set())
            break;
        n = Read(files[index].fh, window + ZZAPP_DATA_OFF + done, (LONG)chunk);
        if (n < 0) {
            err = zz_from_io();
            break;
        }
        if (n == 0)
            break;
        done += (uint32_t)n;
        left -= (uint32_t)n;
        if ((uint32_t)n < chunk)
            break;
    }
    win_cache(ZZAPP_DATA_OFF, done);
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
    win_cache(ZZAPP_DATA_OFF, req->length);
    left = req->length;
    while (left) {
        uint32_t chunk = left > ZZRUN_READ_CHUNK ? ZZRUN_READ_CHUNK : left;
        LONG n;
        heartbeat();
        n = Write(files[index].fh, window + ZZAPP_DATA_OFF + done, (LONG)chunk);
        if (n < 0) {
            err = zz_from_io();
            break;
        }
        if (n == 0) {
            err = ZZ_ENOSPC;
            break;
        }
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
    pos = Seek(files[index].fh, (LONG)target, mode);
    if (pos < 0) {
        file_reply(req->seq, req->fd, zz_from_io(), 0, 0, 0, 0);
        return;
    }
    current = Seek(files[index].fh, 0, OFFSET_CURRENT);
    if (current < 0)
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
    struct FileInfoBlock fib;
    int err = ZZ_OK;
    uint32_t mode = 0;
    uint64_t size = 0;
    uint32_t mtime = 0;

    if (!path_from_wire(req, path, &err))
        goto done;
    if (path[0] == 0) {
        err = ZZ_EINVAL;
        goto done;
    }
    heartbeat();
    if (strchr(path, ':')) {
        if (!abs_paths) {
            err = ZZ_EACCES;
            goto done;
        }
        locked = Lock((STRPTR)path, ACCESS_READ);
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
    if (!examine_lock(locked, &fib)) {
        err = zz_from_io();
        goto done;
    }
    mode = fib.fib_DirEntryType > 0 ? ZZAPP_MODE_DIR : ZZAPP_MODE_FILE;
    size = (uint32_t)fib.fib_Size;
    mtime = amiga_unix(&fib.fib_Date);
done:
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
    struct FileInfoBlock *fib;
    int err = ZZ_OK;
    int index = slot_index(req->fd, &err);

    if (index < 0)
        goto done;
    if (lists[index].used) {
        err = ZZ_EBUSY;
        goto done;
    }
    if (!path_from_wire(req, path, &err))
        goto done;
    heartbeat();
    if (strchr(path, ':')) {
        if (!abs_paths) {
            err = ZZ_EACCES;
            goto done;
        }
        locked = Lock((STRPTR)path, ACCESS_READ);
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
    fib = AllocMem(sizeof(*fib), MEMF_PUBLIC | MEMF_CLEAR);
    if (!fib) {
        err = ZZ_ENOMEM;
        goto done;
    }
    if (!Examine(locked, fib)) {
        err = zz_from_io();
        FreeMem(fib, sizeof(*fib));
        goto done;
    }
    if (fib->fib_DirEntryType < 0) {
        err = ZZ_EIO;
        FreeMem(fib, sizeof(*fib));
        goto done;
    }
    lists[index].lock = locked;
    lists[index].fib = fib;
    lists[index].used = 1;
    lists[index].primed = 1;
    locked = 0;
done:
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
    uint32_t mode = 0;
    uint32_t n = 0;
    size_t len;
    if (index < 0 || !lists[index].used) {
        file_reply(req->seq, req->fd, ZZ_EBADF, 0, 0, 0, 0);
        return;
    }
    heartbeat();
    if (!ExNext(lists[index].lock, lists[index].fib)) {
        err = zz_from_io();
        file_reply(req->seq, req->fd, err, 0, 0, 0, 0);
        return;
    }
    len = strlen((const char *)lists[index].fib->fib_FileName);
    if (len > ZZAPP_PATH_MAX)
        len = ZZAPP_PATH_MAX;
    memcpy(window + ZZAPP_DATA_OFF, lists[index].fib->fib_FileName, len);
    window[ZZAPP_DATA_OFF + len] = 0;
    win_cache(ZZAPP_DATA_OFF, (uint32_t)len + 1u);
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

static void serve_files(void)
{
    struct zzapp_file_wire req;
    if (!window)
        return;
    file_generation = life_get(ZZAPP_LIFE_GENERATION_OFF);
    if (!read_file_req(&req))
        return;
    if (req.seq == 0 || req.seq == file_reply_seq || (req.seq & 1u))
        return;
    switch (req.op) {
    case ZZAPP_FILE_OP_OPEN: serve_open(&req); break;
    case ZZAPP_FILE_OP_CLOSE: serve_close(&req); break;
    case ZZAPP_FILE_OP_READ: serve_read(&req); break;
    case ZZAPP_FILE_OP_WRITE: serve_write(&req); break;
    case ZZAPP_FILE_OP_SEEK: serve_seek(&req); break;
    case ZZAPP_FILE_OP_STAT: serve_stat(&req); break;
    case ZZAPP_FILE_OP_LIST_OPEN: serve_list_open(&req); break;
    case ZZAPP_FILE_OP_LIST_NEXT: serve_list_next(&req); break;
    case ZZAPP_FILE_OP_LIST_CLOSE: serve_list_close(&req); break;
    default:
        file_reply(req.seq, req.fd, ZZ_ENOSYS, 0, 0, 0, 0);
        break;
    }
}

static void push_event(uint16_t type, uint16_t code, int16_t dx, int16_t dy,
                       uint16_t buttons)
{
    uint32_t head, tail;
    uint8_t *hdr;
    uint8_t *ev;
    if (!window)
        return;
    win_cache(ZZAPP_INPUT_OFF, ZZAPP_INPUT_HDR);
    hdr = window + ZZAPP_INPUT_OFF;
    head = zzapp_get_be32(hdr);
    tail = zzapp_get_be32(hdr + 4);
    if (head - tail >= ZZAPP_INPUT_CAP) {
        life_put(ZZAPP_LIFE_INPUT_DROPS_OFF,
                 life_get(ZZAPP_LIFE_INPUT_DROPS_OFF) + 1u);
        return;
    }
    ev = window + ZZAPP_INPUT_OFF + ZZAPP_INPUT_HDR +
         (head & (ZZAPP_INPUT_CAP - 1u)) * ZZAPP_EVENT_BYTES;
    zzapp_put_be16(ev + 0, type);
    zzapp_put_be16(ev + 2, code);
    zzapp_put_be16(ev + 4, (uint16_t)dx);
    zzapp_put_be16(ev + 6, (uint16_t)dy);
    zzapp_put_be16(ev + 8, buttons);
    zzapp_put_be16(ev + 10, 0);
    zzapp_put_be32(ev + 12, now_ms());
    win_cache((uint32_t)(ev - window), ZZAPP_EVENT_BYTES);
    zzapp_put_be32(hdr, head + 1u);
    win_cache(ZZAPP_INPUT_OFF, 4);
}

static void request_stop(void)
{
    ZZ9KMailboxEntry reply;
    if (stop_asked)
        return;
    stop_asked = 1;
    stop_ms = now_ms();
    if (window)
        life_put(ZZAPP_LIFE_STOP_OFF, 1);
    if (!stop_event_sent) {
        push_event(ZZAPP_EV_STOP, 0, 0, 0, 0);
        stop_event_sent = 1;
    }
    op_empty(ZZAPP_OP_STOP, &reply);
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
            uint16_t type = (code & IECODE_UP_PREFIX) ? ZZAPP_EV_KEY_UP
                                                     : ZZAPP_EV_KEY_DOWN;
            push_event(type, (uint16_t)(code & 0x7f), 0, 0, qual);
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
            int16_t dx = 0, dy = 0;
            if (mouse_valid) {
                dx = (int16_t)(mx - mouse_x);
                dy = (int16_t)(my - mouse_y);
            }
            mouse_x = mx;
            mouse_y = my;
            mouse_valid = 1;
            if (dx || dy)
                push_event(ZZAPP_EV_MOUSE, 0, dx, dy, mouse_buttons);
        }
    }
}

static void poll_joysticks(void)
{
    ULONG port;
    if (!LowLevelBase)
        return;
    for (port = 0; port < 2; port++) {
        ULONG raw = ReadJoyPort(port);
        ULONG type = raw & JP_TYPE_MASK;
        int16_t dx = 0, dy = 0;
        uint16_t buttons = 0;
        if (type != JP_TYPE_JOYSTK && type != JP_TYPE_GAMECTLR)
            continue;
        if (joy_valid[port] && raw == joy_last[port])
            continue;
        joy_last[port] = raw;
        joy_valid[port] = 1;
        if (raw & JPF_JOY_LEFT) dx = -1;
        else if (raw & JPF_JOY_RIGHT) dx = 1;
        if (raw & JPF_JOY_UP) dy = -1;
        else if (raw & JPF_JOY_DOWN) dy = 1;
        if (raw & JPF_BUTTON_RED) buttons |= 1u;
        if (raw & JPF_BUTTON_BLUE) buttons |= 2u;
        if (raw & JPF_BUTTON_YELLOW) buttons |= 4u;
        push_event(ZZAPP_EV_JOY, (uint16_t)port, dx, dy, buttons);
    }
}

static void print_log(void)
{
    uint32_t head, tail, usable, guard = 0;
    if (!window)
        return;
    usable = ZZAPP_LOG_BYTES - ZZAPP_LOG_HDR;
    win_cache(ZZAPP_LOG_OFF, 8);
    head = zzapp_get_be32(window + ZZAPP_LOG_OFF);
    tail = zzapp_get_be32(window + ZZAPP_LOG_OFF + 4);
    while (tail != head && guard++ < 64) {
        uint32_t avail = head - tail;
        uint16_t len;
        uint32_t step, i;
        char text[256];
        uint32_t copy;
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
        copy = len < sizeof(text) - 1 ? len : (uint32_t)sizeof(text) - 1;
        for (i = 0; i < copy; i++)
            text[i] = (char)window[ZZAPP_LOG_OFF + ZZAPP_LOG_HDR +
                                   ((tail + 2 + i) % usable)];
        text[copy] = 0;
        fputs(text, stdout);
        if (copy == 0 || text[copy - 1] != '\n')
            fputc('\n', stdout);
        tail += step;
    }
    fflush(stdout);
    zzapp_put_be32(window + ZZAPP_LOG_OFF + 4, tail);
    win_cache(ZZAPP_LOG_OFF + 4, 4);
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

static void close_display(void)
{
    if (buf_back && screen)
        FreeScreenBuffer(screen, buf_back);
    if (buf_front && screen)
        FreeScreenBuffer(screen, buf_front);
    buf_back = NULL;
    buf_front = NULL;
    if (back_bm) {
        p96FreeBitMap(back_bm);
        back_bm = NULL;
    }
    if (close_win) {
        CloseWindow(close_win);
        close_win = NULL;
    }
    if (input_win) {
        CloseWindow(input_win);
        input_win = NULL;
    }
    if (screen) {
        p96CloseScreen(screen);
        screen = NULL;
    }
    have_two = 0;
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
    ULONG depth = format == ZZ_FMT_BGRA8888 ? 32 : 16;
    ULONG formats = format == ZZ_FMT_BGRA8888 ? RGBFF_B8G8R8A8 : RGBFF_R5G6B5;
    if (forced)
        return forced;
    return p96BestModeIDTags(
        P96BIDTAG_NominalWidth, w,
        P96BIDTAG_NominalHeight, h,
        P96BIDTAG_Depth, depth,
        P96BIDTAG_FormatsAllowed, formats,
        TAG_DONE);
}

static int open_display(uint32_t w, uint32_t h, uint32_t format, ULONG forced_mode)
{
    ULONG mode, depth, rgb, pitch = 0, front = 0, back = 0;
    uint8_t payload[ZZAPP_BIND_REQ_BYTES];
    ZZ9KMailboxEntry reply;
    int status;
    struct Screen *pub;

    if (w == 0 || h == 0 || w > ZZRUN_SCREEN_MAX || h > ZZRUN_SCREEN_MAX)
        return 0;
    if (format != ZZ_FMT_RGB565 && format != ZZ_FMT_BGRA8888)
        return 0;
    if (!P96Base || !IntuitionBase || !GfxBase)
        return 0;
    pub = LockPubScreen(NULL);
    if (pub) {
        if (pub->DefaultTitle && !pub_name[0]) {
            pub_name_len = (ULONG)strlen((char *)pub->DefaultTitle);
            if (pub_name_len >= sizeof(pub_name))
                pub_name_len = sizeof(pub_name) - 1;
            memcpy(pub_name, pub->DefaultTitle, pub_name_len);
            pub_name[pub_name_len] = 0;
        }
        UnlockPubScreen(NULL, pub);
    }
    depth = format == ZZ_FMT_BGRA8888 ? 32 : 16;
    rgb = format == ZZ_FMT_BGRA8888 ? RGBFB_B8G8R8A8 : RGBFB_R5G6B5;
    mode = mode_for(w, h, format, forced_mode);
    if (mode == (ULONG)INVALID_ID)
        return 0;
    heartbeat();
    screen = p96OpenScreenTags(
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
    if (!screen)
        return 0;
    if ((ULONG)p96GetBitMapAttr(screen->RastPort.BitMap, P96BMA_RGBFORMAT) != rgb) {
        close_display();
        return 0;
    }
    if (!bitmap_arm(screen->RastPort.BitMap, &front, &pitch)) {
        close_display();
        return 0;
    }
    back_bm = p96AllocBitMap(w, h, depth, BMF_DISPLAYABLE | BMF_CLEAR,
                             screen->RastPort.BitMap, (RGBFTYPE)rgb);
    if (back_bm && bitmap_arm(back_bm, &back, &pitch)) {
        buf_front = AllocScreenBuffer(screen, NULL, SB_SCREEN_BITMAP);
        buf_back = AllocScreenBuffer(screen, back_bm, 0);
        if (buf_front && buf_back)
            have_two = 1;
    }
    if (!have_two) {
        if (buf_back && screen)
            FreeScreenBuffer(screen, buf_back);
        if (buf_front && screen)
            FreeScreenBuffer(screen, buf_front);
        buf_back = NULL;
        buf_front = NULL;
        if (back_bm) {
            p96FreeBitMap(back_bm);
            back_bm = NULL;
        }
        back = 0;
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
        WA_IDCMP, IDCMP_RAWKEY | IDCMP_MOUSEMOVE | IDCMP_MOUSEBUTTONS |
                  IDCMP_CLOSEWINDOW,
        TAG_DONE);
    close_win = OpenWindowTags(NULL,
        WA_CustomScreen, (ULONG)screen,
        WA_Left, 0, WA_Top, 0,
        WA_Width, 80,
        WA_Height, screen->BarHeight + 4 > 14 ? screen->BarHeight + 4 : 14,
        WA_CloseGadget, TRUE,
        WA_DragBar, TRUE,
        WA_DepthGadget, FALSE,
        WA_Title, (ULONG)"ZZRun",
        WA_Activate, FALSE,
        WA_IDCMP, IDCMP_CLOSEWINDOW,
        TAG_DONE);
    if (!input_win || !close_win) {
        close_display();
        return 0;
    }
    card_front = front;
    card_back = back;
    bound_w = w;
    bound_h = h;
    bound_pitch = pitch;
    bound_fmt = format;
    life_put(ZZAPP_LIFE_CARD_FRONT_OFF, front);
    life_put(ZZAPP_LIFE_CARD_BACK_OFF, back);
    memset(payload, 0, sizeof(payload));
    zzapp_put_be32(payload + ZZAPP_BIND_WIDTH, w);
    zzapp_put_be32(payload + ZZAPP_BIND_HEIGHT, h);
    zzapp_put_be32(payload + ZZAPP_BIND_PITCH, pitch);
    zzapp_put_be32(payload + ZZAPP_BIND_FORMAT, format);
    zzapp_put_be32(payload + ZZAPP_BIND_CARD_FRONT, front);
    zzapp_put_be32(payload + ZZAPP_BIND_CARD_BACK, back);
    status = mailbox(ZZAPP_OP_DISPLAY_BIND, payload, ZZAPP_BIND_REQ_BYTES, &reply);
    if (status != ZZ9K_STATUS_OK) {
        printf("ZZRun: display bind failed (%s)\n", status_name(status));
        close_display();
        return 0;
    }
    if (life_get(ZZAPP_LIFE_FLAGS_OFF) & ZZAPP_F_DISPLAY_UNBOUND) {
        puts("ZZRun: display request stayed set after bind");
        close_display();
        return 0;
    }
    showing = 0;
    return 1;
}

static void ack_flip(void)
{
    uint32_t gen, flags;
    if (!window || !screen)
        return;
    gen = life_get(ZZAPP_LIFE_FLIP_GEN_OFF);
    if (gen == flip_acked)
        return;
    flags = life_get(ZZAPP_LIFE_FLAGS_OFF);
    if (have_two && buf_front && buf_back) {
        showing ^= 1;
        ChangeScreenBuffer(screen, showing ? buf_back : buf_front);
    }
    if (flags & ZZAPP_F_VSYNC)
        WaitTOF();
    life_put(ZZAPP_LIFE_FLIP_ACK_OFF, gen);
    flip_acked = gen;
}

static int fill_argv(void)
{
    uint8_t *base = window + ZZAPP_ARGV_OFF;
    uint32_t at = ZZAPP_ARGV_STR_OFF;
    int i;
    memset(base, 0, ZZAPP_ARGV_BYTES);
    zzapp_put_be32(base + ZZAPP_ARGV_ARGC_OFF, (uint32_t)app_argc);
    for (i = 0; i < app_argc; i++) {
        uint32_t n = (uint32_t)strlen(arg_store[i]);
        if (n > 127 || at + n + 1u > ZZAPP_ARGV_BYTES)
            return 0;
        zzapp_put_be32(base + ZZAPP_ARGV_OFFS_OFF + (uint32_t)i * 4u,
                       ZZAPP_ARGV_OFF + at);
        memcpy(base + at, arg_store[i], n + 1u);
        at += n + 1u;
    }
    win_cache(ZZAPP_ARGV_OFF, ZZAPP_ARGV_BYTES);
    return 1;
}

static int load_elf(BPTR fh, LONG size)
{
    uint8_t payload[ZZAPP_CHUNK_REQ_BYTES];
    ZZ9KMailboxEntry reply;
    LONG offset = 0;
    int status = op_empty(ZZAPP_OP_LOAD_BEGIN, &reply);
    if (status != ZZ9K_STATUS_OK) {
        printf("ZZRun: load begin failed (%s)\n", status_name(status));
        return 0;
    }
    while (offset < size) {
        LONG chunk = size - offset;
        LONG n;
        if (chunk > (LONG)ZZAPP_CHUNK_MAX)
            chunk = (LONG)ZZAPP_CHUNK_MAX;
        if (ctrl_c_set()) {
            puts("ZZRun: stopped during load");
            op_empty(ZZAPP_OP_STOP, &reply);
            return 0;
        }
        n = Read(fh, window + ZZAPP_STAGING_OFF, chunk);
        if (n != chunk) {
            puts("ZZRun: short read while loading the ELF");
            op_empty(ZZAPP_OP_STOP, &reply);
            return 0;
        }
        win_cache(ZZAPP_STAGING_OFF, (uint32_t)n);
        memset(payload, 0, sizeof(payload));
        zzapp_put_be32(payload + ZZAPP_CHUNK_FILE_OFF, (uint32_t)offset);
        zzapp_put_be32(payload + ZZAPP_CHUNK_LENGTH, (uint32_t)n);
        zzapp_put_be32(payload + ZZAPP_CHUNK_STAGING, ZZAPP_STAGING_OFF);
        status = mailbox(ZZAPP_OP_LOAD_CHUNK, payload, ZZAPP_CHUNK_REQ_BYTES, &reply);
        if (status != ZZ9K_STATUS_OK) {
            printf("ZZRun: chunk at %ld failed (%s)\n", (long)offset, status_name(status));
            op_empty(ZZAPP_OP_STOP, &reply);
            return 0;
        }
        offset += n;
    }
    status = op_empty(ZZAPP_OP_LOAD_END, &reply);
    if (status != ZZ9K_STATUS_OK) {
        printf("ZZRun: ELF rejected (%s)\n", status_name(status));
        op_empty(ZZAPP_OP_STOP, &reply);
        return 0;
    }
    return 1;
}

static void report_fault(uint32_t code, int32_t exit_code, uint32_t pc,
                         uint32_t sp, uint32_t lr, uint32_t far, uint32_t fsr,
                         uint32_t cpsr, uint32_t generation)
{
    char line[160];
    printf("ZZRun: %s\n", fault_name(code));
    printf("  pc=%08lx sp=%08lx lr=%08lx\n",
           (unsigned long)pc, (unsigned long)sp, (unsigned long)lr);
    printf("  far=%08lx fsr=%08lx cpsr=%08lx\n",
           (unsigned long)far, (unsigned long)fsr, (unsigned long)cpsr);
    printf("  exit=%ld generation=%lu\n", (long)exit_code,
           (unsigned long)generation);
    if (_WBenchMsg && IntuitionBase) {
        snprintf(line, sizeof(line),
                 "%s\\npc=%08lx far=%08lx\\nexit=%ld",
                 fault_name(code), (unsigned long)pc, (unsigned long)far,
                 (long)exit_code);
        tell(line);
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
    uint32_t last_gen = 0;
    uint32_t fault = ZZAPP_FAULT_NONE;
    int32_t exit_code = 0;
    ZZ9KMailboxEntry reply;
    if (window)
        last_gen = life_get(ZZAPP_LIFE_GENERATION_OFF);
    timer_arm();
    for (;;) {
        ULONG mask = SIGBREAKF_CTRL_C;
        ULONG signals;
        uint32_t state, flags;
        if (timer_port)
            mask |= 1UL << timer_port->mp_SigBit;
        if (input_win && input_win->UserPort)
            mask |= 1UL << input_win->UserPort->mp_SigBit;
        if (close_win && close_win->UserPort)
            mask |= 1UL << close_win->UserPort->mp_SigBit;
        signals = Wait(mask);
        if (signals & (1UL << timer_port->mp_SigBit))
            timer_reap();
        heartbeat();
        if ((signals & SIGBREAKF_CTRL_C) || ctrl_c_set()) {
            SetSignal(0, SIGBREAKF_CTRL_C);
            request_stop();
        }
        drain_window(input_win);
        drain_window(close_win);
        poll_joysticks();
        serve_files();
        print_log();
        if (!window)
            break;
        state = life_get(ZZAPP_LIFE_STATE_OFF);
        flags = life_get(ZZAPP_LIFE_FLAGS_OFF);
        fault = life_get(ZZAPP_LIFE_FAULT_OFF);
        if ((flags & ZZAPP_F_DISPLAY_UNBOUND) && !screen) {
            if (!open_display(life_get(ZZAPP_LIFE_WIDTH_OFF),
                              life_get(ZZAPP_LIFE_HEIGHT_OFF),
                              life_get(ZZAPP_LIFE_FORMAT_OFF),
                              forced_mode)) {
                puts("ZZRun: cannot open the requested screen");
                request_stop();
            }
        }
        ack_flip();
        if (state == ZZAPP_ST_FAULTED || fault == ZZAPP_FAULT_WATCHDOG_RESTART) {
            saw_fault = 1;
            break;
        }
        if (entered && (state == ZZAPP_ST_IDLE ||
                        (state == ZZAPP_ST_STOPPING && stop_asked &&
                         now_ms() - stop_ms >= ZZRUN_GRACE_MS))) {
            if (!stop_asked || now_ms() - stop_ms >= ZZRUN_GRACE_MS)
                break;
        }
        if (stop_asked && now_ms() - stop_ms >= ZZRUN_GRACE_MS + ZZRUN_GRACE_MS)
            break;
        if (life_get(ZZAPP_LIFE_GENERATION_OFF) != last_gen &&
            state != ZZAPP_ST_RUNNING && state != ZZAPP_ST_STARTING &&
            state != ZZAPP_ST_LOADING)
            break;
        timer_arm();
    }
    timer_reap();
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
            printf("ZZRun: exit %ld (%s)\n", (long)exit_code,
                   state_name(zzapp_get_be32(p + ZZAPP_STATUS_STATE)));
        return exit_from_app(exit_code, fault, stop_asked);
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
        return exit_from_app(exit_code, fault, stop_asked);
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
    ZZ9KMailboxEntry reply;
    timer_reap();
    if (window && !stop_asked &&
        life_get(ZZAPP_LIFE_STATE_OFF) != ZZAPP_ST_IDLE)
        op_empty(ZZAPP_OP_STOP, &reply);
    for (i = 0; i < ZZAPP_FD_COUNT; i++) {
        close_fd_index(i);
        close_list_index(i);
    }
    close_display();
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
    if (LowLevelBase) {
        CloseLibrary(LowLevelBase);
        LowLevelBase = NULL;
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
    uint32_t abi_major, abi_minor, caps, state;
    uint32_t arm_address, host_offset, win_size;

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
                           &forced_mode, &have_mode)) {
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
            if (!add_arg(argv[i])) {
                cleanup();
                return 10;
            }
        }
    }
    if (!open_root(root_text)) {
        cleanup();
        return 20;
    }
    if (!zz9000_find_board(&board) || !board.address) {
        tell("ZZ9000 board not found.");
        cleanup();
        return 20;
    }
    printf("ZZRun %s\nboard %08lx Zorro %lu\n", ZZRUN_VERSION,
           (unsigned long)board.address, (unsigned long)board.zorro_version);
    ZZ9KBase = OpenLibrary((CONST_STRPTR)"zz9k.library", 0);
    if (!ZZ9KBase) {
        tell("zz9k.library is not installed.");
        cleanup();
        return 20;
    }
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
    printf("runtime abi %lu.%lu state %s caps %08lx\n",
           (unsigned long)abi_major, (unsigned long)abi_minor,
           state_name(state), (unsigned long)caps);
    if (board.zorro_version != 3 || !(caps & ZZAPP_CAP_Z3) || abi_major != 0) {
        tell("ZZRun requires a Zorro III board and app-runtime firmware.");
        cleanup();
        return 20;
    }
    status = op_empty(ZZAPP_OP_ALLOC_WINDOW, &reply);
    if (status != ZZ9K_STATUS_OK || reply.payload_len < ZZAPP_ALLOC_REPLY_BYTES) {
        printf("ZZRun: window allocation failed (%s)\n", status_name(status));
        cleanup();
        return 20;
    }
    arm_address = zzapp_get_be32(reply.payload.inline_data + ZZAPP_ALLOC_ARM_ADDRESS);
    win_size = zzapp_get_be32(reply.payload.inline_data + ZZAPP_ALLOC_SIZE);
    host_offset = zzapp_get_be32(reply.payload.inline_data + ZZAPP_ALLOC_HOST_OFFSET);
    if (win_size < ZZAPP_WINDOW_BYTES ||
        host_offset > board.board_size ||
        win_size > board.board_size - host_offset) {
        puts("ZZRun: rings window is outside the board");
        op_empty(ZZAPP_OP_STOP, &reply);
        cleanup();
        return 20;
    }
    (void)arm_address;
    window = (uint8_t *)(board.address + host_offset);
    window_bytes = win_size;
    win_cache(0, 256);
    if (life_get(ZZAPP_LIFE_MAGIC_OFF) != ZZAPP_LIFE_MAGIC ||
        life_get(ZZAPP_LIFE_VERSION_OFF) != ZZAPP_LIFE_VERSION) {
        puts("ZZRun: rings window magic does not match");
        op_empty(ZZAPP_OP_STOP, &reply);
        cleanup();
        return 20;
    }
    P96Base = OpenLibrary((CONST_STRPTR)"Picasso96API.library", 2);
    LowLevelBase = OpenLibrary((CONST_STRPTR)"lowlevel.library", 0);
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
    if (!fill_argv()) {
        tell("Arguments do not fit in the argv area.");
        op_empty(ZZAPP_OP_STOP, &reply);
        cleanup();
        return 10;
    }
    fh = Open((STRPTR)elf_path, MODE_OLDFILE);
    if (!fh) {
        printf("ZZRun: cannot open %s\n", elf_path);
        op_empty(ZZAPP_OP_STOP, &reply);
        cleanup();
        return 20;
    }
    Seek(fh, 0, OFFSET_END);
    size = Seek(fh, 0, OFFSET_BEGINNING);
    if (size <= 0 || (ULONG)size > ZZRUN_IMAGE_LIMIT) {
        printf("ZZRun: %s is empty or larger than the 16 MiB image limit\n",
               elf_path);
        Close(fh);
        op_empty(ZZAPP_OP_STOP, &reply);
        cleanup();
        return 20;
    }
    printf("loading %s (%ld bytes)\n", elf_path, (long)size);
    if (!load_elf(fh, size)) {
        Close(fh);
        cleanup();
        return ctrl_c_set() ? 5 : 20;
    }
    Close(fh);
    fh = 0;
    memset(payload, 0, sizeof(payload));
    zzapp_put_be32(payload + ZZAPP_START_ARGC, (uint32_t)app_argc);
    status = mailbox(ZZAPP_OP_START, payload, ZZAPP_START_REQ_BYTES, &reply);
    if (status != ZZ9K_STATUS_OK) {
        printf("ZZRun: start failed (%s)\n", status_name(status));
        op_empty(ZZAPP_OP_STOP, &reply);
        cleanup();
        return 20;
    }
    rc = run_loop(have_mode ? forced_mode : 0);
    cleanup();
    return rc;
}
