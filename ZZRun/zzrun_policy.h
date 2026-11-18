/*
 * Decisions ZZRun makes from launcher state. No AmigaOS types, so a host
 * test can exercise them without DOS or Exec.
 *
 * ZZRun serves its app over the AAR queue transport (aar/transport.h plus
 * the ZZ9000 private wire, zzapp_proto.h). The window of the earlier
 * launcher protocol is gone, and with it the generation fences.
 *
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ZZRUN_POLICY_H
#define ZZRUN_POLICY_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef ZZAPP_PROTO_NO_WINDOW
#define ZZAPP_PROTO_NO_WINDOW 1
#endif
#include "aar/transport.h"
#include "zzapp_proto.h"

#define ZZRUN_POLICY_GRACE_MS 500u

/* Lifecycle states (AAR_CB_STATE). State 4 of the old launcher protocol
 * (paused) no longer exists. */
#define ZZRUN_ST_IDLE AAR_STATE_IDLE
#define ZZRUN_ST_LOADING AAR_STATE_LOADING
#define ZZRUN_ST_STARTING AAR_STATE_STARTING
#define ZZRUN_ST_RUNNING AAR_STATE_RUNNING
#define ZZRUN_ST_STOPPING AAR_STATE_STOPPING
#define ZZRUN_ST_FAULTED AAR_STATE_FAULTED
#define ZZRUN_ST_CLAIMED AAR_STATE_CLAIMED
#define ZZRUN_MIB_BYTES (1024u * 1024u)

/* One claim as LIST reports it (ZZAPP_LIST_* of the private wire). */
struct zzrun_list_reply {
    uint32_t state;
    uint32_t handle; /* 0: no further claim */
    uint32_t committed_bytes;
    uint32_t index;
    uint32_t cpu_ms;
    uint32_t service_ms;
    uint8_t block_reason;
    uint8_t end_reason;
    uint16_t flags;
    char name[ZZAPP_LIST_NAME_BYTES + 1];
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

/* MAXMEM and HOSTMEM are decimal MiB, at least 1. */
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

/* HOSTMEM is the shared (host-visible) memory limit in MiB. The client
 * writes it to AAR_CB_SHARED_MIB before the load; the platform checks it at
 * LOAD_BEGIN. The field takes 0..4095; a launch without HOSTMEM writes 0 and
 * keeps the platform default. */
#define ZZRUN_HOSTMEM_MAX_MIB 4095u

static inline int zzrun_parse_hostmem(const char *text, uint32_t *mib_out)
{
    uint32_t value;

    if (!zzrun_parse_maxmem(text, &value) || value > ZZRUN_HOSTMEM_MAX_MIB ||
        !mib_out)
        return 0;
    *mib_out = value;
    return 1;
}

/* A claim handle named on the command line: decimal, $hex or 0x hex, never
 * zero. */
static inline int zzrun_parse_handle(const char *text, uint32_t *handle_out)
{
    const unsigned char *p = (const unsigned char *)text;
    uint32_t base = 10u;
    uint32_t value = 0;
    int digits = 0;

    if (!p || !handle_out)
        return 0;
    if (p[0] == '$') {
        base = 16u;
        p++;
    } else if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        base = 16u;
        p += 2;
    }
    for (; *p; ++p) {
        uint32_t digit;

        if (*p >= '0' && *p <= '9')
            digit = (uint32_t)(*p - '0');
        else if (base == 16u && *p >= 'a' && *p <= 'f')
            digit = (uint32_t)(*p - 'a') + 10u;
        else if (base == 16u && *p >= 'A' && *p <= 'F')
            digit = (uint32_t)(*p - 'A') + 10u;
        else
            return 0;
        if (value > (UINT32_MAX - digit) / base)
            return 0;
        value = value * base + digit;
        digits++;
    }
    if (!digits || value == 0)
        return 0;
    *handle_out = value;
    return 1;
}

/* A refused CLAIM, LOAD_BEGIN or LOAD_END carries an AAR_REASON_* code. */
static inline const char *zzrun_refusal_text(uint32_t reason)
{
    switch (reason) {
    case AAR_REASON_MEMORY:
        return "not enough card memory for the app record and stack";
    case AAR_REASON_TABLES:
        return "no address-space translation tables left";
    case AAR_REASON_HOSTHEAP:
        return "not enough shared card memory for the queues and buffers";
    case AAR_REASON_LIMIT:
        return "a platform limit was reached (HOSTMEM or MAXMEM too large?)";
    default:
        return "refused by the platform";
    }
}

/* The capabilities ZZRun passes at LOAD_BEGIN (ZZAPP_CLIENT_*). It serves
 * CLUT8 screens, acknowledges pan flips by completion flag, accepts
 * card-owned flips, and renews its lease from the main loop. */
static inline uint32_t zzrun_client_caps(int audio_ahi)
{
    return ZZAPP_CLIENT_LEASE_EXTENDED | ZZAPP_CLIENT_CLUT8 |
           ZZAPP_CLIENT_SCANOUT_ACK | ZZAPP_CLIENT_LATEST |
           (audio_ahi ? ZZAPP_CLIENT_AUDIO_AHI : 0u);
}

static inline int zzrun_list_reply_decode(const uint8_t *payload,
                                          uint16_t bytes,
                                          struct zzrun_list_reply *out)
{
    uint32_t i;

    if (!payload || !out || bytes < ZZAPP_LIST_REPLY_BYTES)
        return 0;
    out->state = zzrun_be32(payload + ZZAPP_LIST_STATE);
    out->handle = zzrun_be32(payload + ZZAPP_LIST_HANDLE);
    out->committed_bytes = zzrun_be32(payload + ZZAPP_LIST_COMMITTED);
    out->index = zzrun_be32(payload + ZZAPP_LIST_INDEX);
    out->cpu_ms = zzrun_be32(payload + ZZAPP_LIST_CPU_MS);
    out->service_ms = zzrun_be32(payload + ZZAPP_LIST_SERVICE_MS);
    out->block_reason = payload[ZZAPP_LIST_REASONS];
    out->end_reason = payload[ZZAPP_LIST_REASONS + 1u];
    out->flags = (uint16_t)(((uint16_t)payload[ZZAPP_LIST_REASONS + 2u] << 8) |
                            payload[ZZAPP_LIST_REASONS + 3u]);
    for (i = 0; i < ZZAPP_LIST_NAME_BYTES && payload[ZZAPP_LIST_NAME + i]; ++i)
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
    case ZZRUN_ST_STOPPING: return "stopping";
    case ZZRUN_ST_FAULTED: return "faulted";
    default: return "unknown";
    }
}

static inline const char *zzrun_end_reason_name(uint32_t reason)
{
    switch (reason) {
    case AAR_END_NONE: return "none";
    case AAR_END_EXIT: return "exit";
    case AAR_END_FORCED: return "forced stop";
    case AAR_END_USER_FAULT: return "user fault";
    case AAR_END_STALL: return "stall watchdog";
    case AAR_END_STUCK_CALL: return "stuck-call watchdog";
    case AAR_END_LEASE: return "lease loss";
    case AAR_END_PLATFORM_RESET: return "Amiga reset";
    case AAR_END_RUNTIME_RESTART: return "core restart";
    case AAR_END_QUARANTINED: return "quarantined";
    default: return "unknown";
    }
}

static inline const char *zzrun_fault_class_name(uint32_t fault)
{
    switch (fault) {
    case AAR_FAULT_NONE: return "none";
    case AAR_FAULT_DATA: return "data abort";
    case AAR_FAULT_FETCH: return "prefetch abort";
    case AAR_FAULT_ILLEGAL: return "undefined instruction";
    case AAR_FAULT_ALIGN: return "alignment fault";
    case AAR_FAULT_OTHER: return "fault";
    default: return "unknown";
    }
}

/* What LIST reports an app is blocked on (ZZAPP_BLOCK_*). Firmware reports
 * both display-open and flip waits as vsync. */
#define ZZRUN_BLOCK_NONE    ZZAPP_BLOCK_NONE
#define ZZRUN_BLOCK_FILE    ZZAPP_BLOCK_FILE
#define ZZRUN_BLOCK_CONSOLE ZZAPP_BLOCK_CONSOLE
#define ZZRUN_BLOCK_EVENT   ZZAPP_BLOCK_EVENT
#define ZZRUN_BLOCK_SLEEP   ZZAPP_BLOCK_SLEEP
#define ZZRUN_BLOCK_VSYNC   ZZAPP_BLOCK_VSYNC
#define ZZRUN_BLOCK_AUDIO   ZZAPP_BLOCK_AUDIO

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

/* console_read_finish: never stdout. */
#define ZZRUN_FINISH_NONE 0
#define ZZRUN_FINISH_RETURN_ON_READ_HANDLE 1
#define ZZRUN_FINISH_WAIT_ON_USER_CONSOLE 2

struct zzrun_exit_in {
    uint32_t state;
    uint32_t end_reason;
    uint32_t elapsed_ms;
    int lost;          /* the claim's handle went stale: not this launcher's */
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
    /* A stale handle belongs to nobody this launcher can serve. */
    if (in->lost)
        return 1;
    blocked = zzrun_output_blocks_exit(in->output_pending, in->output_failed);
    /* Ctrl-C ends the process once both grace periods have elapsed, even
     * when console output is still queued. The faulted path must not skip
     * this: that was the hang. */
    if (in->user_stop && in->elapsed_ms >= ZZRUN_POLICY_GRACE_MS * 2u)
        return 1;
    if ((in->state == ZZRUN_ST_FAULTED ||
         in->end_reason == AAR_END_RUNTIME_RESTART) &&
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

/* A CONSOLE_READ request is the demand for input. It starts a DOS read only
 * while the app is RUNNING and no stop was asked: a forced unwind can leave
 * the request outstanding after the app is gone. */
static inline int zzrun_console_read_allowed(uint32_t state, int stop_asked)
{
    return state == ZZRUN_ST_RUNNING && !stop_asked;
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

/* The process exit code for an app that ended with end_reason and exit_code
 * (AAR_CB_END_REASON, AAR_CB_EXIT_CODE). Every platform-ended case is a
 * launcher error, a forced end is one unless the user asked for it, and an
 * exit code outside 0..255 is the app's mistake unless the user stopped it. */
static inline int zzrun_app_exit_code(uint32_t end_reason, int32_t exit_code,
                                      int user_stop)
{
    switch (end_reason) {
    case AAR_END_USER_FAULT:
    case AAR_END_STALL:
    case AAR_END_STUCK_CALL:
    case AAR_END_LEASE:
    case AAR_END_PLATFORM_RESET:
    case AAR_END_RUNTIME_RESTART:
    case AAR_END_QUARANTINED:
        return 20;
    case AAR_END_FORCED:
        if (!user_stop)
            return 20;
        break;
    default:
        break;
    }
    if (exit_code < 0 || exit_code > 255)
        return user_stop ? 0 : 20;
    return (int)exit_code;
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

/* A DOS write may advance the consumer only by the completed byte count. A
 * short completion is an output failure after that partial advance. */
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

/* Async DOS packet wait policy: only the reply ends the wait -- ZZRun keeps
 * the packet and its buffers alive until then, even after Ctrl-C or losing
 * the claim; those only mark the result abandoned. */
static inline int zzrun_packet_abandoned(int ctrl_c, int claim_live)
{
    return ctrl_c || !claim_live;
}

static inline int zzrun_packet_keep_waiting(int reply_arrived)
{
    return !reply_arrived;
}

/* Completion and abandonment are independent: handlers can allocate an
 * object before ZZRun observes Ctrl-C or a lost claim. */
static inline int zzrun_packet_result_usable(int completed, int abandoned)
{
    return completed && !abandoned;
}

/* A packet is sent only while its operation is live. A lost claim or a
 * pending Ctrl-C means the result will be discarded, but the handler would
 * still act on the packet first (create, truncate, write), so such a packet
 * stays unsent. Releases (ACTION_END, ACTION_FREE_LOCK) always go out: they
 * give back what an earlier packet acquired. */
static inline int zzrun_packet_may_send(int release, int ownership_ok,
                                        int ctrl_c_pending)
{
    return release || (ownership_ok && !ctrl_c_pending);
}

/* A resource slot must survive a release packet that was never delivered;
 * after delivery DOS owns no resource and its local handle may be freed even
 * when the handler reported an error. The callback sends ACTION_END or
 * ACTION_FREE_LOCK; fallback closes/unlocks after an undelivered packet. */
struct zzrun_dos_release_ops {
    void *context;
    int (*send_release)(void *context, int32_t *result);
    void (*free_local)(void *context);
    void (*fallback_release)(void *context);
    void (*forget_slot)(void *context);
};

int zzrun_dos_close_slot(const struct zzrun_dos_release_ops *ops,
                         int *delivered_out);
void zzrun_dos_end_or_close(const struct zzrun_dos_release_ops *ops);
void zzrun_dos_release_abandoned(const struct zzrun_dos_release_ops *ops,
                                 int acquired);

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
 * current-directory candidate. selected_out (optional) receives the
 * candidate of the last packet sent: the one that decided the outcome. A
 * caller that must act on that same object afterwards (probe, then open)
 * pins its resolver to this candidate instead of resolving the name again,
 * which would restart at the first directory of a multi-directory assign. */
static inline int zzrun_dos_dispatch_path(
    const char *name, uint8_t *name_bstr, uint32_t name_bstr_capacity,
    int32_t object_not_found, zzrun_dos_path_resolve_fn resolve,
    void *resolve_context, zzrun_dos_path_send_fn send, void *send_context,
    int *attempted_out, int *usable_out,
    struct zzrun_dos_path_candidate *selected_out)
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
        if (selected_out)
            *selected_out = candidate;
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

/* A break counts as a user stop when it is still pending or when a DOS
 * packet wait already consumed it and recorded user_stop. */
static inline int zzrun_user_break(int ctrl_c_pending, int user_stop)
{
    return ctrl_c_pending != 0 || user_stop != 0;
}

/* Exit code after a failed ELF load: 5 (RETURN_WARN) when the user stopped
 * it with Ctrl-C, 20 for any other load failure. */
static inline int zzrun_load_failure_exit(int ctrl_c_pending, int user_stop)
{
    return zzrun_user_break(ctrl_c_pending, user_stop) ? 5 : 20;
}

/* AmigaDOS keywords are case-insensitive: "ZZRun status" lists claims
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

/* How ZZRun opens a file for one valid flags word (AAR_FILE_*). Zero is not
 * a plan: the flags are invalid and ZZRun answers EINVAL before it touches
 * AmigaDOS. */
enum zzrun_open_plan {
    ZZRUN_OPEN_INVALID = 0,
    /* READ only: MODE_OLDFILE, the file must exist. */
    ZZRUN_OPEN_READ_EXISTING,
    /* WRITE without CREATE: the file must exist (Lock probe, because
     * MODE_READWRITE creates a missing file); open it MODE_READWRITE. */
    ZZRUN_OPEN_UPDATE_EXISTING,
    /* TRUNCATE without CREATE: as above, then SetFileSize(0). Handlers that
     * lack ACTION_SET_FILE_SIZE get MODE_NEWFILE after the probe. */
    ZZRUN_OPEN_TRUNCATE_EXISTING,
    /* CREATE only: MODE_READWRITE on the existing file, else MODE_NEWFILE.
     * An existing file keeps its contents. */
    ZZRUN_OPEN_CREATE_KEEP,
    /* CREATE|TRUNCATE: MODE_NEWFILE (create or replace). */
    ZZRUN_OPEN_CREATE_REPLACE,
    /* CREATE|EXCL, with or without TRUNCATE: Lock probe, EEXIST if the file
     * exists, else MODE_NEWFILE. */
    ZZRUN_OPEN_CREATE_EXCLUSIVE
};

static inline enum zzrun_open_plan zzrun_open_plan_for_flags(uint32_t flags)
{
    const uint32_t known = AAR_FILE_READ | AAR_FILE_WRITE | AAR_FILE_CREATE |
                           AAR_FILE_TRUNCATE | AAR_FILE_EXCL;
    int create = (flags & AAR_FILE_CREATE) != 0;
    int truncate = (flags & AAR_FILE_TRUNCATE) != 0;
    int write = (flags & AAR_FILE_WRITE) != 0;

    if (flags & ~known)
        return ZZRUN_OPEN_INVALID;
    if (!(flags & (AAR_FILE_READ | AAR_FILE_WRITE)))
        return ZZRUN_OPEN_INVALID;
    if ((create || truncate) && !write)
        return ZZRUN_OPEN_INVALID;
    if ((flags & AAR_FILE_EXCL) && !create)
        return ZZRUN_OPEN_INVALID;
    if (flags & AAR_FILE_EXCL)
        return ZZRUN_OPEN_CREATE_EXCLUSIVE;
    if (create)
        return truncate ? ZZRUN_OPEN_CREATE_REPLACE : ZZRUN_OPEN_CREATE_KEEP;
    if (truncate)
        return ZZRUN_OPEN_TRUNCATE_EXISTING;
    return write ? ZZRUN_OPEN_UPDATE_EXISTING : ZZRUN_OPEN_READ_EXISTING;
}

/* Outcome of the existence probe, a Lock(name, ACCESS_READ). A refused lock
 * with ERROR_OBJECT_IN_USE still proves the object exists: the handler holds
 * it for someone else. Any other failure is the caller's error. */
enum zzrun_probe_result {
    ZZRUN_PROBE_ERROR = 0,
    ZZRUN_PROBE_EXISTS,
    ZZRUN_PROBE_MISSING
};

static inline enum zzrun_probe_result zzrun_probe_result_for_lock(
    int locked, int32_t io_error, int32_t object_not_found,
    int32_t object_in_use)
{
    if (locked)
        return ZZRUN_PROBE_EXISTS;
    if (io_error == object_not_found)
        return ZZRUN_PROBE_MISSING;
    if (io_error == object_in_use)
        return ZZRUN_PROBE_EXISTS;
    return ZZRUN_PROBE_ERROR;
}

/* Which plans probe the target before the open packet. UPDATE, TRUNCATE and
 * EXCL decide on the answer. CREATE (keep or replace) acts on an existing
 * file whichever directory of a multi-directory assign holds it, so an
 * absolute path probes to find that directory; a relative path already has
 * one fixed parent lock. After an EXISTS answer every later packet of the
 * open (update, truncate, NEWFILE fallback) goes to the probe's candidate;
 * a MISSING answer leaves the name unresolved for the creating open. */
static inline int zzrun_plan_probes_target(enum zzrun_open_plan plan,
                                           int absolute)
{
    switch (plan) {
    case ZZRUN_OPEN_UPDATE_EXISTING:
    case ZZRUN_OPEN_TRUNCATE_EXISTING:
    case ZZRUN_OPEN_CREATE_EXCLUSIVE:
        return 1;
    case ZZRUN_OPEN_CREATE_KEEP:
    case ZZRUN_OPEN_CREATE_REPLACE:
        return absolute != 0;
    default:
        return 0;
    }
}

/* What a failed Lock of a relative name's final component means for one
 * plan. The lock also proves the leaf stays under the ROOT when it succeeds,
 * so a refusal ends the open unless it already answers the plan's question:
 * a missing leaf is what a create plan wants, and an exclusive create must
 * report a leaf that exists but is held by another program (the handler
 * refuses the lock with ERROR_OBJECT_IN_USE) as EEXIST, the same answer an
 * absolute name gets from its probe. Nothing is opened on the EXISTS answer,
 * so no open bypasses the ROOT check. */
enum zzrun_leaf_action {
    ZZRUN_LEAF_REFUSE = 0,
    ZZRUN_LEAF_PROCEED,
    ZZRUN_LEAF_EXISTS
};

static inline enum zzrun_leaf_action zzrun_leaf_lock_failure(
    enum zzrun_open_plan plan, int32_t io_error, int32_t object_not_found,
    int32_t object_in_use)
{
    int create = plan == ZZRUN_OPEN_CREATE_KEEP ||
                 plan == ZZRUN_OPEN_CREATE_REPLACE ||
                 plan == ZZRUN_OPEN_CREATE_EXCLUSIVE;

    switch (zzrun_probe_result_for_lock(0, io_error, object_not_found,
                                        object_in_use)) {
    case ZZRUN_PROBE_MISSING:
        return create ? ZZRUN_LEAF_PROCEED : ZZRUN_LEAF_REFUSE;
    case ZZRUN_PROBE_EXISTS:
        return plan == ZZRUN_OPEN_CREATE_EXCLUSIVE ? ZZRUN_LEAF_EXISTS :
                                                     ZZRUN_LEAF_REFUSE;
    default:
        return ZZRUN_LEAF_REFUSE;
    }
}

/* A step that creates, truncates or replaces a file runs only while the
 * request is live (the claim is still served) and no stop was asked.
 * user_stop records a Ctrl-C that an earlier packet wait consumed, so a
 * cancelled request stays cancelled for the steps that follow it. */
static inline int zzrun_open_step_allowed(int request_current,
                                          int ctrl_c_pending, int user_stop)
{
    return request_current && !zzrun_user_break(ctrl_c_pending, user_stop);
}

/* RELEASE <handle> ends a claim an administrator names. It is for claims that
 * are finished: an app that ended or faulted, a claim the card revoked, or one
 * whose launcher lost its lease. A claim whose app still runs, or is being
 * claimed or loaded by a live launcher, is refused. */
static inline int zzrun_release_allowed(uint32_t state, uint32_t end_reason)
{
    if (end_reason == AAR_END_QUARANTINED || end_reason == AAR_END_LEASE)
        return 1;
    return state == ZZRUN_ST_IDLE || state == ZZRUN_ST_FAULTED;
}

#endif /* ZZRUN_POLICY_H */
