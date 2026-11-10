/*
 * Decisions ZZRun makes from launcher state. No AmigaOS types, so a host
 * test can exercise them without DOS or Exec.
 *
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ZZRUN_POLICY_H
#define ZZRUN_POLICY_H

#include <string.h>
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

#define ZZRUN_ST_CLAIMED 7u
#define ZZRUN_MIB_BYTES (1024u * 1024u)

struct zzrun_claim_reply {
    uint32_t arm_address;
    uint32_t size;
    uint32_t host_offset;
    uint32_t slot;
    uint32_t generation;
};

struct zzrun_list_reply {
    uint32_t state;
    uint32_t generation;
    uint32_t committed_bytes;
    uint32_t limit_bytes;
    uint32_t cpu_ms;
    uint32_t service_ms;
    uint8_t block_reason;
    uint8_t terminal_reason;
    uint16_t flags;
    char name[21];
};

struct zzrun_slots_query {
    uint32_t slots;
    uint32_t free_slots;
    uint32_t commit_free;
    uint32_t commit_total;
};

static inline uint32_t zzrun_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static inline void zzrun_put_be32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

/* Extended requests preserve the legacy fields, zero unused bytes, and carry
 * the owning slot and generation at the wire selector offsets. */
static inline uint16_t zzrun_request_build(
    uint8_t out[ZZAPP_MAILBOX_PAYLOAD_BYTES], const uint8_t *legacy,
    uint16_t legacy_bytes, int slots, uint32_t slot, uint32_t generation)
{
    if (!out || (legacy_bytes && !legacy) ||
        legacy_bytes > ZZAPP_MAILBOX_PAYLOAD_BYTES)
        return 0;
    if (!slots) {
        if (legacy_bytes)
            memcpy(out, legacy, legacy_bytes);
        return legacy_bytes;
    }
    if (legacy_bytes > 32u)
        return 0;
    memset(out, 0, ZZAPP_MAILBOX_PAYLOAD_BYTES);
    if (legacy_bytes)
        memcpy(out, legacy, legacy_bytes);
    zzrun_put_be32(out + ZZAPP_SEL_SLOT, slot);
    zzrun_put_be32(out + ZZAPP_SEL_GENERATION, generation);
    return ZZAPP_MAILBOX_PAYLOAD_BYTES;
}

static inline int zzrun_claim_reply_decode(const uint8_t *payload,
                                           uint16_t bytes, int slots,
                                           struct zzrun_claim_reply *out)
{
    if (!payload || !out || bytes < ZZAPP_ALLOC_REPLY_BYTES)
        return 0;
    out->arm_address = zzrun_be32(payload + ZZAPP_ALLOC_ARM_ADDRESS);
    out->size = zzrun_be32(payload + ZZAPP_ALLOC_SIZE);
    out->host_offset = zzrun_be32(payload + ZZAPP_ALLOC_HOST_OFFSET);
    out->slot = 0;
    out->generation = 0;
    if (!slots)
        return 1;
    if (bytes < ZZAPP_ALLOC_EXT_REPLY_BYTES)
        return 0;
    out->slot = zzrun_be32(payload + ZZAPP_ALLOC_SLOT);
    out->generation = zzrun_be32(payload + ZZAPP_ALLOC_GENERATION);
    return out->slot < ZZAPP_SLOT_COUNT;
}

static inline int zzrun_slots_query_decode(const uint8_t *payload,
                                           uint16_t bytes,
                                           struct zzrun_slots_query *out)
{
    if (!payload || !out || bytes < ZZAPP_QUERY_EXT_REPLY_BYTES)
        return 0;
    out->slots = zzrun_be32(payload + ZZAPP_QUERY_SLOTS);
    out->free_slots = zzrun_be32(payload + ZZAPP_QUERY_FREE_SLOTS);
    out->commit_free = zzrun_be32(payload + ZZAPP_QUERY_COMMIT_FREE);
    out->commit_total = zzrun_be32(payload + ZZAPP_QUERY_COMMIT_TOTAL);
    return out->slots != 0 && out->slots <= ZZAPP_SLOT_COUNT;
}

/* A terminal state belongs to the launcher that owns its generation. Only a
 * changed generation means another launcher replaced that app. */
static inline int zzrun_slot_replaced(int slots, uint32_t owned_slot,
                                      uint32_t owned_generation,
                                      uint32_t observed_slot,
                                      uint32_t observed_generation,
                                      uint32_t observed_state)
{
    (void)observed_state;
    if (!slots)
        return observed_generation != owned_generation;
    if (observed_slot != owned_slot)
        return 0;
    return observed_generation != owned_generation;
}

static inline int zzrun_parse_maxmem(const char *text, uint32_t *mib_out)
{
    uint32_t value = 0;
    const unsigned char *p = (const unsigned char *)text;

    if (!p || !*p || !mib_out)
        return 0;
    for (; *p; ++p) {
        uint32_t digit;

        if (*p < '0' || *p > '9')
            return 0;
        digit = (uint32_t)(*p - '0');
        if (value > (UINT32_MAX - digit) / 10u)
            return 0;
        value = value * 10u + digit;
    }
    if (value == 0)
        return 0;
    *mib_out = value;
    return 1;
}

static inline int zzrun_list_reply_decode(const uint8_t *payload,
                                          uint16_t bytes,
                                          struct zzrun_list_reply *out)
{
    uint32_t i;

    if (!payload || !out || bytes < ZZAPP_LIST_REPLY_BYTES)
        return 0;
    out->state = zzrun_be32(payload + ZZAPP_LIST_STATE);
    out->generation = zzrun_be32(payload + ZZAPP_LIST_GENERATION);
    out->committed_bytes = zzrun_be32(payload + ZZAPP_LIST_COMMITTED);
    out->limit_bytes = zzrun_be32(payload + ZZAPP_LIST_LIMIT);
    out->cpu_ms = zzrun_be32(payload + ZZAPP_LIST_CPU_MS);
    out->service_ms = zzrun_be32(payload + ZZAPP_LIST_SERVICE_MS);
    out->block_reason = payload[ZZAPP_LIST_REASONS];
    out->terminal_reason = payload[ZZAPP_LIST_REASONS + 1u];
    out->flags = (uint16_t)(((uint16_t)payload[ZZAPP_LIST_REASONS + 2u] << 8) |
                            payload[ZZAPP_LIST_REASONS + 3u]);
    for (i = 0; i < 20u && payload[ZZAPP_LIST_NAME + i]; ++i)
        out->name[i] = (char)payload[ZZAPP_LIST_NAME + i];
    out->name[i] = '\0';
    return 1;
}

static inline const char *zzrun_slot_state_name(uint32_t state)
{
    switch (state) {
    case ZZRUN_ST_IDLE: return "idle";
    case ZZRUN_ST_CLAIMED: return "claimed";
    case ZZRUN_ST_LOADING: return "loading";
    case ZZRUN_ST_STARTING: return "starting";
    case ZZRUN_ST_RUNNING: return "running";
    case ZZRUN_ST_PAUSED: return "paused";
    case ZZRUN_ST_STOPPING: return "stopping";
    case ZZRUN_ST_FAULTED: return "faulted";
    default: return "unknown";
    }
}

static inline const char *zzrun_terminal_reason_name(uint8_t reason)
{
    switch (reason) {
    case 0u: return "none";
    case 1u: return "exit";
    case 2u: return "forced stop";
    case 3u: return "user fault";
    case 4u: return "stall watchdog";
    case 5u: return "stuck-call watchdog";
    case 6u: return "lease loss";
    case 7u: return "Amiga reset";
    case 8u: return "core restart";
    default: return "unknown";
    }
}

/* Launcher diagnostics share the asynchronous app-output writer. Keep space
 * for status/error text when a stalled destination has filled diagnostics. */
#define ZZRUN_LAUNCHER_OUTPUT_BYTES 8192u
#define ZZRUN_LAUNCHER_STATUS_RESERVE_BYTES 2048u

#define ZZRUN_OUTPUT_NONE 0
#define ZZRUN_OUTPUT_LAUNCHER 1
#define ZZRUN_OUTPUT_APP 2

static inline int zzrun_launcher_output_accept(uint32_t used, uint32_t bytes,
                                               int diagnostic)
{
    uint32_t limit = diagnostic ? ZZRUN_LAUNCHER_OUTPUT_BYTES -
                                  ZZRUN_LAUNCHER_STATUS_RESERVE_BYTES :
                                  ZZRUN_LAUNCHER_OUTPUT_BYTES;

    return used <= limit && bytes <= limit - used;
}

/* A launcher message already staged before a drain precedes app bytes that
 * have not yet been submitted to the shared asynchronous writer. */
static inline int zzrun_output_next_source(uint32_t launcher_bytes,
                                           uint32_t app_bytes)
{
    if (launcher_bytes)
        return ZZRUN_OUTPUT_LAUNCHER;
    return app_bytes ? ZZRUN_OUTPUT_APP : ZZRUN_OUTPUT_NONE;
}

#if ZZRUN_LAUNCHER_STATUS_RESERVE_BYTES >= ZZRUN_LAUNCHER_OUTPUT_BYTES
#error "launcher status reserve must leave diagnostic space"
#endif

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

/* Deferred load/start operations share one firmware queue. A BUSY result is
 * therefore retryable contention, unlike every other mailbox error. */
#define ZZRUN_BUSY_RETRY_MS 3000u

static inline int zzrun_deferred_should_retry(int status, int busy_status,
                                              uint32_t started_ms,
                                              uint32_t now_ms)
{
    return status == busy_status &&
           now_ms - started_ms < ZZRUN_BUSY_RETRY_MS;
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

/* Amiga raw key C is 0x33 (0x31 Z, 0x32 X, 0x33 C; 0x20 is A). The key still
 * goes to the app; this decides only whether ZZRun also requests its
 * documented stop. */
static inline int zzrun_rawkey_requests_stop(uint16_t code, uint16_t qualifiers,
                                             uint16_t control_qualifier)
{
    return (qualifiers & control_qualifier) != 0 && code == 0x33u;
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

/* Each successful ChangeScreenBuffer has one SafeMessage before the old
 * buffer and its reply port may be released. */
static inline uint32_t zzrun_screen_safe_started(uint32_t pending)
{
    return pending + 1u;
}

static inline uint32_t zzrun_screen_safe_completed(uint32_t pending)
{
    return pending ? pending - 1u : 0u;
}

/* AmigaDOS keywords are case-insensitive: "ZZRun status" lists slots
 * instead of trying to load a file called "status". */
static inline int zzrun_is_status_keyword(const char *arg)
{
    static const char keyword[] = "STATUS";
    unsigned i;

    if (!arg)
        return 0;
    for (i = 0; keyword[i]; i++) {
        char c = arg[i];

        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');
        if (c != keyword[i])
            return 0;
    }
    return arg[i] == '\0';
}

#endif /* ZZRUN_POLICY_H */
