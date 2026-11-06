/*
 * Decisions ZZRun makes from launcher state. No AmigaOS types, so a host
 * test can exercise them without DOS or Exec.
 *
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ZZRUN_POLICY_H
#define ZZRUN_POLICY_H

#include <stdint.h>

#define ZZRUN_POLICY_GRACE_MS 500u

/* Must match ZZAPP_ST_* / ZZAPP_FAULT_WATCHDOG_RESTART. ZZRun.c asserts it. */
#define ZZRUN_ST_IDLE 0u
#define ZZRUN_ST_LOADING 1u
#define ZZRUN_ST_STARTING 2u
#define ZZRUN_ST_RUNNING 3u
#define ZZRUN_ST_PAUSED 4u
#define ZZRUN_ST_STOPPING 5u
#define ZZRUN_ST_FAULTED 6u
#define ZZRUN_FAULT_WATCHDOG_RESTART 6u

#define ZZRUN_STOP_REQ_BYTES 4u

/* console_read_finish: never stdout. */
#define ZZRUN_FINISH_NONE 0
#define ZZRUN_FINISH_RETURN_ON_READ_HANDLE 1
#define ZZRUN_FINISH_WAIT_ON_USER_CONSOLE 2

struct zzrun_exit_in {
    uint32_t state;
    uint32_t fault;
    uint32_t elapsed_ms;
    uint32_t generation;
    uint32_t last_generation;
    int entered;
    int stop_asked;
    int user_stop;
    int output_pending;
    int output_failed;
};

/* A failed drain must not keep the launcher alive. Pending output still
 * delays a clean exit so the app's text can be written. */
static inline int zzrun_output_blocks_exit(int output_pending, int output_failed)
{
    return output_pending && !output_failed;
}

static inline int zzrun_run_should_exit(const struct zzrun_exit_in *in)
{
    int blocked;

    if (!in)
        return 0;
    /* The generation is the ownership token. A different value belongs to
     * another launcher even when its app is already live. */
    if (in->generation != in->last_generation)
        return 1;
    blocked = zzrun_output_blocks_exit(in->output_pending, in->output_failed);
    /* Ctrl-C ends the process once both grace periods have elapsed, even
     * when console output is still queued. The faulted path must not skip
     * this: that was the hang. */
    if (in->user_stop && in->elapsed_ms >= ZZRUN_POLICY_GRACE_MS * 2u)
        return 1;
    if ((in->state == ZZRUN_ST_FAULTED ||
         in->fault == ZZRUN_FAULT_WATCHDOG_RESTART) &&
        !blocked)
        return 1;
    if (in->entered &&
        (in->state == ZZRUN_ST_IDLE ||
         (in->state == ZZRUN_ST_STOPPING && in->stop_asked &&
          in->elapsed_ms >= ZZRUN_POLICY_GRACE_MS)) &&
        (!in->stop_asked || in->elapsed_ms >= ZZRUN_POLICY_GRACE_MS) &&
        !blocked)
        return 1;
    if (in->stop_asked && in->elapsed_ms >= ZZRUN_POLICY_GRACE_MS * 2u &&
        !blocked)
        return 1;
    return 0;
}

/* Ring head/tail are the caller's earlier samples. WANT is sampled after
 * them; forced unwind leaves WANT set, so state and stop are required. */
static inline int zzrun_keyboard_read_allowed(uint32_t state, int owns,
                                              int stop_asked, uint32_t head,
                                              uint32_t tail, uint32_t want,
                                              int read_pending, int at_eof)
{
    if (read_pending || at_eof || stop_asked || !owns)
        return 0;
    if (state != ZZRUN_ST_RUNNING)
        return 0;
    if (head != tail)
        return 0;
    return want == 1u;
}

static inline int zzrun_async_read_allowed(uint32_t state, int owns,
                                           int stop_asked, uint32_t head,
                                           uint32_t tail, uint32_t cap,
                                           int read_pending, int at_eof)
{
    uint32_t used;

    if (read_pending || at_eof || stop_asked || !owns)
        return 0;
    if (state != ZZRUN_ST_RUNNING)
        return 0;
    used = head - tail;
    if (used > cap)
        return 0;
    return (cap - used) != 0u;
}

static inline int zzrun_audio_epoch_replaced(int have_epoch, uint32_t accepted,
                                             uint32_t observed)
{
    return have_epoch && observed != accepted;
}

static inline void zzrun_stop_payload(uint8_t out[ZZRUN_STOP_REQ_BYTES],
                                      uint32_t generation)
{
    out[0] = (uint8_t)(generation >> 24);
    out[1] = (uint8_t)(generation >> 16);
    out[2] = (uint8_t)(generation >> 8);
    out[3] = (uint8_t)generation;
}

static inline int zzrun_finish_prompt(int pending, int interactive_read)
{
    if (!pending)
        return ZZRUN_FINISH_NONE;
    if (interactive_read)
        return ZZRUN_FINISH_RETURN_ON_READ_HANDLE;
    return ZZRUN_FINISH_WAIT_ON_USER_CONSOLE;
}

/* README returns 20 for a launcher error even when the app exits 0.
 * An AHI open failure on the AUDIO=AHI route is a launcher error, as are
 * a display failure and a console that cannot be opened or written. */
static inline int zzrun_process_exit(int app_exit, int display_failed,
                                     int console_failed, int ahi_open_failed)
{
    if (display_failed || console_failed || ahi_open_failed)
        return 20;
    return app_exit;
}

/* A queued APP_LOAD_BEGIN that comes back busy did not publish a
 * generation: the shared word may already belong to the launcher that won
 * the race. Claim nothing then. Other submitted outcomes retain the
 * existing cleanup behaviour. A reply generation, when firmware includes
 * one, is the value this request published; otherwise adopt the observed
 * non-idle generation that replaced the one sampled before the request. */
static inline uint32_t zzrun_load_generation_to_claim(
    int submitted, int begin_busy, uint32_t prior, uint32_t observed,
    int observed_idle, int reply_has_generation, uint32_t reply_generation)
{
    if (!submitted || begin_busy)
        return 0;
    if (reply_has_generation)
        return reply_generation;
    if (observed_idle || observed == 0u || observed == prior)
        return 0;
    return observed;
}

/* A request captured by the launcher remains usable only while both the
 * request sequence and the owning app generation still match the window. */
static inline int zzrun_file_request_is_current(uint32_t request_seq,
                                                uint32_t request_generation,
                                                uint32_t live_seq,
                                                uint32_t live_generation)
{
    return request_seq == live_seq && request_generation == live_generation;
}

/* argv accepts an empty NUL-terminated string; only oversized strings are
 * invalid. */
static inline int zzrun_arg_length_allowed(uint32_t length)
{
    return length <= 127u;
}

/* Terminal app states leave the run loop even with a write in flight; the
 * cleanup path waits for that packet without starving service work first. */
static inline int zzrun_console_output_blocks_run_exit(uint32_t state,
                                                        int pending,
                                                        int failed)
{
    if (state == ZZRUN_ST_IDLE || state == ZZRUN_ST_FAULTED)
        return 0;
    return pending && !failed;
}

/* A DOS write may advance the consumer tail only by the completed byte
 * count. A short completion is an output failure after that partial advance. */
static inline uint32_t zzrun_console_write_advance(uint32_t requested,
                                                    int32_t written)
{
    if (written <= 0 || (uint32_t)written > requested)
        return 0;
    return (uint32_t)written;
}

static inline int zzrun_console_write_failed(uint32_t requested,
                                             int32_t written)
{
    return written <= 0 || (uint32_t)written != requested;
}

#endif /* ZZRUN_POLICY_H */
