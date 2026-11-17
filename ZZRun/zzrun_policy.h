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

/* The extended QUERY low-memory capability advertises a per-generation
 * lifecycle crossing counter that wraps naturally as a u32. */
struct zzrun_low_memory_state {
    uint32_t generation;
    uint32_t crossings;
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

static inline int zzrun_low_memory_supported(uint32_t caps)
{
    return (caps & ZZAPP_CAP_LOW_MEMORY) != 0;
}

static inline int zzrun_low_memory_changed(struct zzrun_low_memory_state *state,
                                           uint32_t caps,
                                           uint32_t generation,
                                           uint32_t crossings)
{
    if (!zzrun_low_memory_supported(caps))
        return 0;
    if (state->generation != generation) {
        state->generation = generation;
        state->crossings = 0;
    }
    if (state->crossings == crossings)
        return 0;
    state->crossings = crossings;
    return 1;
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

/* An explicit MAXMEM is a cap the launcher must honour. Firmware without
 * slots cannot carry it on APP_LOAD_BEGIN, so that launch is rejected
 * before loading. A launch that does not request a cap keeps the
 * byte-identical legacy request: no cap field at all. */
#define ZZRUN_MAXMEM_OMIT   0
#define ZZRUN_MAXMEM_SEND   1
#define ZZRUN_MAXMEM_REJECT 2

static inline int zzrun_maxmem_decision(int slots_supported, uint32_t maxmem_mib)
{
    if (maxmem_mib == 0u)
        return ZZRUN_MAXMEM_OMIT;
    if (!slots_supported)
        return ZZRUN_MAXMEM_REJECT;
    return ZZRUN_MAXMEM_SEND;
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
    case 9u: return "quarantined";
    default: return "unknown";
    }
}

/* Must match ZZAPP_BLOCK_*. ZZRun.c asserts it. Firmware reports both
 * display-open and flip waits as vsync. */
#define ZZRUN_BLOCK_NONE    0u
#define ZZRUN_BLOCK_FILE    1u
#define ZZRUN_BLOCK_CONSOLE 2u
#define ZZRUN_BLOCK_EVENT   3u
#define ZZRUN_BLOCK_SLEEP   4u
#define ZZRUN_BLOCK_VSYNC   5u
#define ZZRUN_BLOCK_AUDIO   6u

static inline const char *zzrun_block_reason_name(uint8_t reason)
{
    switch (reason) {
    case ZZRUN_BLOCK_NONE: return "none";
    case ZZRUN_BLOCK_FILE: return "file";
    case ZZRUN_BLOCK_CONSOLE: return "console";
    case ZZRUN_BLOCK_EVENT: return "event";
    case ZZRUN_BLOCK_SLEEP: return "sleep";
    case ZZRUN_BLOCK_VSYNC: return "vsync";
    case ZZRUN_BLOCK_AUDIO: return "audio";
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

struct zzrun_owned_cursor {
    uint32_t value;
    uint32_t epoch;
    int valid;
};

/* Adopt a locally-owned ring cursor only when its generation/epoch changes.
 * Thereafter the counterpart word may be stale immediately after publish. */
static inline uint32_t zzrun_owned_cursor_adopt(
    struct zzrun_owned_cursor *cursor, uint32_t epoch, uint32_t observed)
{
    if (!cursor->valid || cursor->epoch != epoch) {
        cursor->value = observed;
        cursor->epoch = epoch;
        cursor->valid = 1;
    }
    return cursor->value;
}

static inline uint32_t zzrun_owned_cursor_advance(
    struct zzrun_owned_cursor *cursor, uint32_t amount)
{
    cursor->value += amount;
    return cursor->value;
}

static inline uint32_t zzrun_console_output_available(uint32_t head,
                                                       uint32_t local_tail)
{
    return head - local_tail;
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
/* A new ZZRun advertises the longer main-loop lease. */
static inline uint32_t zzrun_launcher_lease_flags(uint32_t flags)
{
    return flags | ZZAPP_F_LAUNCHER_HEARTBEAT;
}
/* A renewal captured for one generation must not keep a replacement alive. */
static inline int zzrun_lease_renew_allowed(uint32_t captured_generation,
                                            uint32_t owned_generation,
                                            uint32_t observed_generation)
{
    return captured_generation != 0u &&
           captured_generation == owned_generation &&
           captured_generation == observed_generation;
}

/* A raw DOS packet has two independent outcomes: delivery to the handler and
 * the handler's DOS result. Callers must never treat a transport failure as a
 * successful zero/one DOS result, and must preserve dp_Res2 as IoErr(). */
static inline int zzrun_dos_boolean_result(int transport_ok, int32_t result)
{
    return transport_ok && result != 0;
}

static inline int32_t zzrun_dos_error_code(int transport_ok, int32_t result2,
                                           int32_t transport_error)
{
    return transport_ok ? result2 : transport_error;
}
/* A lease gate before LOAD_BEGIN must not block a legacy claim: its
 * generation is 0 until LOAD_BEGIN publishes one. */
static inline int zzrun_lease_gate_stops(int owned_generation, int heartbeat_ok)
{
    return owned_generation != 0 && !heartbeat_ok;
}

/* Async DOS packet wait policy: only the reply ends the wait -- ZZRun keeps
 * the packet and its buffers alive until then, even after Ctrl-C or losing
 * the generation; those only mark the result abandoned. */
static inline int zzrun_packet_abandoned(int ctrl_c, int heartbeat_ok)
{
    return ctrl_c || !heartbeat_ok;
}

static inline int zzrun_packet_keep_waiting(int reply_arrived)
{
    return !reply_arrived;
}

/* Completion and abandonment are independent: handlers can allocate an
 * object before ZZRun observes Ctrl-C or a replaced generation. */
static inline int zzrun_packet_result_usable(int completed, int abandoned)
{
    return completed && !abandoned;
}

#define ZZRUN_DOS_RELEASE_LOCK 1u
#define ZZRUN_DOS_RELEASE_OPEN 2u

typedef void (*zzrun_dos_release_fn)(void *context, uint32_t release_kind,
                                     uint32_t resource);

/* A reply can acquire a resource after the caller has abandoned its result.
 * Dispatch its matching cleanup exactly once before forgetting that reply. */
static inline int zzrun_packet_release_abandoned_resource(
    int completed, int abandoned, int32_t result, uint32_t release_kind,
    uint32_t resource, zzrun_dos_release_fn release, void *context)
{
    if (!completed || !abandoned || result == 0)
        return 0;
    if (release)
        release(context, release_kind, resource);
    return 1;
}

/* Direct DOS packets receive the path relative to the lock selected by
 * GetDeviceProc(): strip only the device/assign prefix. */
static inline const char *zzrun_dos_packet_path(const char *name)
{
    const char *p;

    if (!name)
        return NULL;
    for (p = name; *p; p++) {
        if (*p == ':')
            return p + 1;
    }
    return name;
}

static inline uint32_t zzrun_bstr_encode(const char *name, uint8_t *out,
                                         uint32_t cap);

struct zzrun_dos_path_candidate {
    uintptr_t handler;
    uint32_t directory;
};

struct zzrun_dos_path_reply {
    int completed;
    int abandoned;
    int32_t result;
    int32_t error;
};

typedef int (*zzrun_dos_path_resolve_fn)(
    void *context, int advance, struct zzrun_dos_path_candidate *candidate);
typedef void (*zzrun_dos_path_send_fn)(
    void *context, const struct zzrun_dos_path_candidate *candidate,
    const uint8_t *name_bstr, uint32_t name_bstr_bytes,
    uint32_t name_bstr_bptr, struct zzrun_dos_path_reply *reply);

/* Send an operation to GetDeviceProc candidates. name_bstr is public,
 * longword-aligned storage that remains valid until the sender replies. A
 * failed lookup only tries the next multi-assign directory for
 * ERROR_OBJECT_NOT_FOUND; a colonless name is sent unchanged to the
 * current-directory candidate. */
static inline int zzrun_dos_dispatch_path(
    const char *name, uint8_t *name_bstr, uint32_t name_bstr_capacity,
    int32_t object_not_found, zzrun_dos_path_resolve_fn resolve,
    void *resolve_context, zzrun_dos_path_send_fn send, void *send_context,
    int *attempted_out, int *usable_out)
{
    const char *path = zzrun_dos_packet_path(name);
    struct zzrun_dos_path_candidate candidate;
    struct zzrun_dos_path_reply reply;
    uint32_t name_bstr_bytes;
    uint32_t name_bstr_bptr;
    int advance = 0;

    if (attempted_out)
        *attempted_out = 0;
    if (usable_out)
        *usable_out = 0;
    if (!path || !name_bstr || ((uintptr_t)name_bstr & 3u) != 0u ||
        !resolve || !send)
        return 0;
    name_bstr_bytes = zzrun_bstr_encode(path, name_bstr, name_bstr_capacity);
    if (name_bstr_bytes == 0u)
        return 0;
    name_bstr_bptr = (uint32_t)((uintptr_t)name_bstr >> 2);
    while (resolve(resolve_context, advance, &candidate)) {
        if (usable_out)
            *usable_out = 0;
        memset(&reply, 0, sizeof(reply));
        send(send_context, &candidate, name_bstr, name_bstr_bytes,
             name_bstr_bptr, &reply);
        if (!reply.completed || reply.abandoned)
            return 0;
        if (attempted_out)
            *attempted_out = 1;
        if (usable_out)
            *usable_out = 1;
        if (reply.result != 0)
            return 1;
        if (reply.error != object_not_found)
            return 0;
        advance = 1;
    }
    return 0;
}

/* DOS packet name arguments are BCPL strings (BSTR): a length byte, the
 * characters, padded to an even total. Returns the encoded byte count, or 0
 * when the name is longer than a BSTR can carry or does not fit the buffer. */
static inline uint32_t zzrun_bstr_encode(const char *name, uint8_t *out,
                                         uint32_t cap)
{
    uint32_t len;
    uint32_t i;
    uint32_t total;

    if (!name || !out)
        return 0;
    len = 0;
    while (name[len]) {
        if (len >= 255u)
            return 0;
        len++;
    }
    total = 1u + len;
    if (total & 1u)
        total++;
    if (cap < total)
        return 0;
    out[0] = (uint8_t)len;
    for (i = 0; i < len; i++)
        out[1u + i] = (uint8_t)name[i];
    if (!(len & 1u))
        out[1u + len] = 0;
    return total;
}

/* A Ctrl-C seen while a DOS packet is pending must still reach the app as a
 * stop request; abandoning only the packet's result loses the stop. */
static inline int zzrun_ctrl_c_must_request_stop(int ctrl_c)
{
    return ctrl_c;
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
