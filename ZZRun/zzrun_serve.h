/*
 * What ZZRun does for an app's service requests: files, console, display,
 * audio, and the events it posts. Each handler takes one request as the
 * transport module hands it over (struct zzrun_req) and produces its
 * completion (struct zzrun_done). Nothing here includes AmigaOS headers: the
 * DOS, Intuition, P96 and AHI calls sit behind the small operation tables
 * below, so a host test can run the handlers against a fake request stream.
 *
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ZZRUN_SERVE_H
#define ZZRUN_SERVE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "zzrun_policy.h"

/* ---- Requests and completions ------------------------------------------ */

/* A request as the module returns it, with its payload buffer already mapped
 * into this process. buf is the mapped address of (buf_token, buf_off) and is
 * NULL when the request names no payload. */
struct zzrun_req {
    uint16_t op;
    uint16_t flags;
    uint32_t id;
    uint32_t arg[AAR_ENTRY_ARGS];
    uint32_t buf_token;
    uint32_t buf_off;
    uint32_t buf_len;
    uint8_t *buf;
};

/* The completion a handler produces: AAR_OK or a negative AAR_E_* in result,
 * the entry flags and out[]. */
struct zzrun_done {
    int32_t result;
    uint16_t flags;
    uint32_t out[AAR_ENTRY_OUTS];
};

/* Completes a request that a handler left deferred. */
struct zzrun_sink {
    void *ctx;
    void (*complete)(void *ctx, const struct zzrun_req *req,
                     const struct zzrun_done *done);
};

#define ZZRUN_SERVE_DONE     0 /* the completion is in *done */
#define ZZRUN_SERVE_DEFERRED 1 /* the handler completes through the sink */

/* ---- DOS error numbers -------------------------------------------------- */

/* AmigaDOS IoErr() values the handlers branch on. ZZRun.c asserts each
 * against dos/dos.h. */
#define ZZRUN_IOERR_NO_FREE_STORE    103
#define ZZRUN_IOERR_OBJECT_IN_USE    202
#define ZZRUN_IOERR_OBJECT_EXISTS    203
#define ZZRUN_IOERR_DIR_NOT_FOUND    204
#define ZZRUN_IOERR_OBJECT_NOT_FOUND 205
#define ZZRUN_IOERR_ACTION_NOT_KNOWN 209
#define ZZRUN_IOERR_OBJECT_TOO_LARGE 207
#define ZZRUN_IOERR_OBJECT_WRONG_TYPE 212
#define ZZRUN_IOERR_DEVICE_NOT_MOUNTED 218
#define ZZRUN_IOERR_SEEK_ERROR       219
#define ZZRUN_IOERR_DISK_FULL        221
#define ZZRUN_IOERR_DELETE_PROTECTED 222
#define ZZRUN_IOERR_WRITE_PROTECTED  223
#define ZZRUN_IOERR_READ_PROTECTED   224
#define ZZRUN_IOERR_NO_MORE_ENTRIES  232
#define ZZRUN_IOERR_BREAK            304

/* An AmigaDOS error as the AAR_E_* code the app sees; 0 maps to AAR_OK. */
int32_t zzrun_errno_from_io(int32_t io_error);

/* Seconds from the Amiga epoch (1978) to the Unix epoch (1970). */
#define ZZRUN_UNIX_DELTA 252460800UL

/* DateStamp fields as Unix seconds, 0 for an unset stamp. */
uint32_t zzrun_amiga_unix(uint32_t days, uint32_t minutes, uint32_t ticks);

/* ---- Files -------------------------------------------------------------- */

#define ZZRUN_FD_COUNT AAR_MIN_FILES
#define ZZRUN_IO_CHUNK 8192u
#define ZZRUN_PATH_BYTES (AAR_PATH_MAX + 1u)

enum zzrun_mode {
    ZZRUN_MODE_OLD = 0,  /* MODE_OLDFILE */
    ZZRUN_MODE_READWRITE, /* MODE_READWRITE */
    ZZRUN_MODE_NEW        /* MODE_NEWFILE */
};

struct zzrun_info {
    int is_dir;
    uint32_t size;
    uint32_t mtime; /* Unix seconds */
    char name[AAR_NAME_MAX + 1u];
};

/*
 * The DOS operations the file handlers use. Locks, file handles and list
 * objects are opaque non-zero values; 0 is "none". Every call that can fail
 * reports *transport_ok = 0 when the DOS packet was never delivered or its
 * result was abandoned, and otherwise the DOS result with the IoErr() value
 * in *io.
 */
struct zzrun_dos {
    void *ctx;
    /* The request is still wanted: the claim is served and no stop was
     * asked in a way that cancels it. */
    int (*live)(void *ctx);
    /* A Ctrl-C is pending, or one was consumed by an earlier packet wait. */
    int (*user_break)(void *ctx);

    uintptr_t (*dup_lock)(void *ctx, uintptr_t lock);
    void (*unlock)(void *ctx, uintptr_t lock);
    uintptr_t (*parent_dir)(void *ctx, uintptr_t lock);
    int (*same_lock)(void *ctx, uintptr_t a, uintptr_t b);
    /* ACCESS_READ lock of name relative to dir. */
    uintptr_t (*lock_rel)(void *ctx, uintptr_t dir, const char *name,
                          int *transport_ok, int32_t *io);
    /* ACCESS_READ lock of an absolute (device:path) name, through every
     * GetDeviceProc candidate. */
    uintptr_t (*lock_abs)(void *ctx, const char *name, int *transport_ok,
                          int32_t *io);
    int (*lock_examine)(void *ctx, uintptr_t lock, struct zzrun_info *info,
                        int *transport_ok, int32_t *io);

    /* One open acts on one target. dir is the parent lock of a relative
     * name, or 0 for an absolute name that GetDeviceProc resolves; the
     * probe pins the resolver to the candidate that holds the file, so
     * every later packet of the open reaches that file. */
    void (*target_begin)(void *ctx, uintptr_t dir, const char *name);
    /* Returns 1 with *exists set, or 0 with *transport_ok and *io. */
    int (*target_probe)(void *ctx, int *exists, int *transport_ok,
                        int32_t *io);
    uintptr_t (*target_open)(void *ctx, enum zzrun_mode mode,
                             int *transport_ok, int32_t *io);
    void (*target_end)(void *ctx);

    /* File handles. Reads and writes return the byte count, or -1. */
    int32_t (*fh_set_size0)(void *ctx, uintptr_t fh, int *transport_ok,
                            int32_t *io);
    int32_t (*fh_read)(void *ctx, uintptr_t fh, uint8_t *dst, uint32_t bytes,
                       int *transport_ok, int32_t *io);
    int32_t (*fh_write)(void *ctx, uintptr_t fh, const uint8_t *src,
                        uint32_t bytes, int *transport_ok, int32_t *io);
    /* 0 = OFFSET_BEGINNING, 1 = OFFSET_CURRENT, 2 = OFFSET_END. Returns the
     * previous position, or -1. */
    int32_t (*fh_seek)(void *ctx, uintptr_t fh, int32_t offset, int whence,
                       int *transport_ok, int32_t *io);
    /* ACTION_END, falling back to Close() when it was never delivered. */
    void (*fh_end_or_close)(void *ctx, uintptr_t fh);
    /* ACTION_END for an open slot. *delivered says whether DOS got it; only
     * then is the handle gone. Returns the handler's boolean result. */
    int (*fh_close)(void *ctx, uintptr_t fh, int *delivered, int32_t *io);

    /* A list takes over lock when list_open succeeds. */
    int (*list_open)(void *ctx, uintptr_t lock, uintptr_t *list,
                     int *transport_ok, int32_t *io);
    int (*list_next)(void *ctx, uintptr_t list, struct zzrun_info *info,
                     int *transport_ok, int32_t *io);
    void (*list_close)(void *ctx, uintptr_t list);
};

struct zzrun_files {
    const struct zzrun_dos *dos;
    uintptr_t root;
    int abs_paths;
    uintptr_t fh[ZZRUN_FD_COUNT];
    uintptr_t list[ZZRUN_FD_COUNT];
    uint8_t io[ZZRUN_IO_CHUNK];
};

void zzrun_files_init(struct zzrun_files *files, const struct zzrun_dos *dos,
                      uintptr_t root, int abs_paths);
/* Closes every open file and list. */
void zzrun_files_close_all(struct zzrun_files *files);
/* Serves one AAR_OP_FILE_* or AAR_OP_LIST_* request. */
void zzrun_files_serve(struct zzrun_files *files, const struct zzrun_req *req,
                       struct zzrun_done *done);

/* ---- Console ------------------------------------------------------------ */

#define ZZRUN_CONSOLE_READ_BYTES  256u
#define ZZRUN_CONSOLE_WRITE_BYTES 1024u

struct zzrun_console_ops {
    void *ctx;
    /* Opens the Workbench console when there is one. 0 = it cannot open. */
    int (*open)(void *ctx);
    /* Starts one asynchronous DOS write of bytes (copied before the call
     * returns). 0 = it could not start. */
    int (*write_start)(void *ctx, const uint8_t *data, uint32_t bytes);
    /* Starts one asynchronous DOS read of at most max bytes. 0 = it could
     * not start. */
    int (*read_start)(void *ctx, uint32_t max);
    /* The app is RUNNING and no stop was asked. */
    int (*read_allowed)(void *ctx);
    /* Output cannot be written: stop the app and say so once. */
    void (*output_error)(void *ctx);
};

struct zzrun_console {
    struct zzrun_console_ops ops;
    const struct zzrun_sink *sink;

    uint8_t launcher[ZZRUN_LAUNCHER_OUTPUT_BYTES];
    uint32_t launcher_head;
    uint32_t launcher_tail;
    uint32_t launcher_used;
    uint32_t launcher_dropped;

    int write_pending;
    int write_source;
    uint32_t write_bytes;
    int app_write_active; /* app_write holds an outstanding CONSOLE_WRITE */
    struct zzrun_req app_write;
    uint32_t app_write_done;

    int read_active; /* read_req holds an outstanding CONSOLE_READ */
    int read_pending; /* its DOS read was started */
    struct zzrun_req read_req;
    uint32_t read_max;
    int eof;
    int failed; /* output could not be written */
};

void zzrun_console_init(struct zzrun_console *c,
                        const struct zzrun_console_ops *ops,
                        const struct zzrun_sink *sink);
/* Queues launcher text. A diagnostic that does not fit is dropped and
 * counted; a plain status message is never dropped for want of the reserve. */
void zzrun_console_launcher_put(struct zzrun_console *c, const uint8_t *data,
                                uint32_t bytes, int diagnostic);
void zzrun_console_launcher_status(struct zzrun_console *c, const char *msg);
/* CONSOLE_WRITE and CONSOLE_READ. Both complete through the sink. */
void zzrun_console_write_request(struct zzrun_console *c,
                                 const struct zzrun_req *req,
                                 struct zzrun_done *done, int *deferred);
void zzrun_console_read_request(struct zzrun_console *c,
                                const struct zzrun_req *req,
                                struct zzrun_done *done, int *deferred);
/* Starts the next write or read that may start. Call it on every pass. */
void zzrun_console_pump(struct zzrun_console *c);
/* The DOS write started by write_start finished with this result. */
void zzrun_console_write_done(struct zzrun_console *c, int32_t written);
/* The DOS read finished: got bytes in data, or <= 0 for end of input. */
void zzrun_console_read_done(struct zzrun_console *c, int32_t got,
                             const uint8_t *data);
/* Output the app or the launcher still has to write. */
int zzrun_console_output_pending(const struct zzrun_console *c);

/* ---- Display ------------------------------------------------------------ */

#define ZZRUN_SCREEN_MAX 4096u
#define ZZRUN_DISPLAY_ID 1u

struct zzrun_display_ops {
    void *ctx;
    /* DISPLAY_OPEN: opens the P96 screen, finds the bitmaps and calls
     * zzrun_display_opened. Fills *done. */
    void (*open)(void *ctx, const struct zzrun_req *req,
                 struct zzrun_done *done);
    /* ChangeScreenBuffer to the back (1) or the front (0) buffer. 0 = the
     * change failed. */
    int (*change_buffer)(void *ctx, int to_back);
    /* Pans the screen to page (page * screen height rows down). */
    void (*pan)(void *ctx, uint32_t page);
    void (*wait_tof)(void *ctx);
    /* LoadRGB32 of a table: (count << 16) | first, then three 32 bit words
     * per colour, then 0. */
    void (*load_rgb32)(void *ctx, const uint32_t *table);
    void (*close)(void *ctx);
};

struct zzrun_display {
    struct zzrun_display_ops ops;
    const struct zzrun_sink *sink;

    int open;
    uint32_t id;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t format;
    uint32_t layout;
    uint32_t flags; /* AAR_DISPLAY_* the app asked for */
    uint32_t front; /* tokens of page 0, 1 and 2 (0 when absent) */
    uint32_t back;
    uint32_t spare;
    int have_two;   /* two P96 screen buffers, flipped by ChangeScreenBuffer */
    int have_pan;   /* one tall screen, flipped by panning */
    uint32_t pan_count;
    uint32_t showing;

    int flip_active; /* a ChangeScreenBuffer flip waits for its SafeMessage */
    struct zzrun_req flip_req;
    int flip_queued; /* one more flip arrived meanwhile */
    struct zzrun_req flip_next;
};

void zzrun_display_init(struct zzrun_display *d,
                        const struct zzrun_display_ops *ops,
                        const struct zzrun_sink *sink);

/* DISPLAY_OPEN arguments: AAR_OK, or AAR_E_INVAL for a size or format the
 * runtime must not have asked for. */
int32_t zzrun_display_open_check(const struct zzrun_req *req);
/* The surface depth in bits and the AAR_LAYOUT_* of one AAR_FMT_*. */
uint32_t zzrun_display_depth(uint32_t format);
uint32_t zzrun_display_layout(uint32_t format);
/* The AAR_DISPCAP_* bits ZZRun grants for the AAR_DISPLAY_* flags asked. */
uint16_t zzrun_display_caps(uint32_t open_flags);
/* Records the screen DISPLAY_OPEN made and fills its completion. front,
 * back and spare are tokens (board offsets) of the bitmap memory: for
 * panning, front is page 0 and the other pages follow at pitch * height. */
void zzrun_display_opened(struct zzrun_display *d, const struct zzrun_req *req,
                          uint32_t pitch, uint32_t front, uint32_t back,
                          uint32_t spare, int have_two, int have_pan,
                          uint32_t pan_count, struct zzrun_done *done);
/* Forgets the screen (the P96 screen is closed elsewhere). */
void zzrun_display_reset(struct zzrun_display *d);
void zzrun_display_flip_request(struct zzrun_display *d,
                                const struct zzrun_req *req,
                                struct zzrun_done *done, int *deferred);
/* A SafeMessage of a ChangeScreenBuffer flip came back. */
void zzrun_display_flip_safe(struct zzrun_display *d);
void zzrun_display_palette_request(struct zzrun_display *d,
                                   const struct zzrun_req *req,
                                   struct zzrun_done *done);
void zzrun_display_close_request(struct zzrun_display *d,
                                 const struct zzrun_req *req,
                                 struct zzrun_done *done);

/* ---- Audio (the AHI route) ---------------------------------------------- */

struct zzrun_audio_ops {
    void *ctx;
    /* Opens AHI at rate and channels and binds the ring (token, bytes,
     * frames) the runtime named. 0 = it cannot. */
    int (*open)(void *ctx, uint32_t rate, uint32_t channels, uint32_t token,
                uint32_t bytes, uint32_t frames);
    void (*close)(void *ctx);
};

struct zzrun_audio {
    struct zzrun_audio_ops ops;
    int open;
    uint32_t rate;
    uint32_t channels;
    uint32_t frames;
    int open_failed;
};

void zzrun_audio_init(struct zzrun_audio *a, const struct zzrun_audio_ops *ops);
void zzrun_audio_open_request(struct zzrun_audio *a, const struct zzrun_req *req,
                              struct zzrun_done *done);
void zzrun_audio_close_request(struct zzrun_audio *a,
                               const struct zzrun_req *req,
                               struct zzrun_done *done);
/* The frames one AHI buffer takes from the ring: what is available, at most
 * 1/50 s of audio and the buffer size. */
uint32_t zzrun_audio_chunk_frames(uint32_t available_frames, uint32_t rate,
                                  uint32_t channels, uint32_t buffer_bytes);

/* ---- Events ------------------------------------------------------------- */

struct zzrun_events_ops {
    void *ctx;
    /* aar_xport_post_event: AAR_OK when the event is posted or merged into
     * a pending mouse event, AAR_E_BUSY when the completion queue is full
     * and the event was dropped (the module counts the drop), another
     * AAR_E_* when the claim is gone. */
    int (*post)(void *ctx, const aar_event *ev);
};

/* The module coalesces mouse motion and keeps event order. What ZZRun adds is
 * the one event that must never be lost: a stop that found the queue full is
 * posted again on every pass until it is delivered. */
struct zzrun_events {
    struct zzrun_events_ops ops;
    int stop_pending;
    aar_event stop;
};

void zzrun_events_init(struct zzrun_events *e, const struct zzrun_events_ops *ops);
/* Returns the result of the post (AAR_OK when posted or kept for retry). */
int zzrun_events_push(struct zzrun_events *e, uint16_t type, uint16_t code,
                      int16_t dx, int16_t dy, uint16_t buttons,
                      uint32_t time_ms);
/* Posts what could not go earlier. Call it on every pass. */
void zzrun_events_flush(struct zzrun_events *e);
int zzrun_events_pending(const struct zzrun_events *e);

/* ---- Dispatch ----------------------------------------------------------- */

struct zzrun_serve {
    struct zzrun_files files;
    struct zzrun_console console;
    struct zzrun_display display;
    struct zzrun_audio audio;
    struct zzrun_sink sink;
};

void zzrun_serve_init(struct zzrun_serve *s, const struct zzrun_sink *sink);
/* Serves one request. ZZRUN_SERVE_DEFERRED: the completion comes later
 * through the sink. An op ZZRun does not serve completes with AAR_E_NOSYS. */
int zzrun_serve_request(struct zzrun_serve *s, const struct zzrun_req *req,
                        struct zzrun_done *done);

#endif /* ZZRUN_SERVE_H */
