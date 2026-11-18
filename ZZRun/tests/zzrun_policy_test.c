#include <assert.h>
#include <stdio.h>
#include <string.h>

#ifndef ZZAPP_CAP_SLOTS
#define ZZAPP_CAP_SLOTS (1u << 5)
#define ZZAPP_CAP_LOW_MEMORY (1u << 6)
#define ZZAPP_SLOT_COUNT 4u
#define ZZAPP_MAILBOX_PAYLOAD_BYTES 48u
#define ZZAPP_SEL_SLOT 40u
#define ZZAPP_SEL_GENERATION 44u
#define ZZAPP_LOAD_MEM_LIMIT_MIB 0u
#define ZZAPP_CAP_HOST_LIMIT (1u << 8)
#define ZZAPP_LOAD_HOST_VISIBLE_MIB 4u
#define ZZAPP_QUERY_SLOTS 16u
#define ZZAPP_QUERY_FREE_SLOTS 20u
#define ZZAPP_QUERY_COMMIT_FREE 24u
#define ZZAPP_QUERY_COMMIT_TOTAL 28u
#define ZZAPP_QUERY_EXT_REPLY_BYTES 32u
#define ZZAPP_ALLOC_ARM_ADDRESS 0u
#define ZZAPP_ALLOC_SIZE 4u
#define ZZAPP_ALLOC_HOST_OFFSET 8u
#define ZZAPP_ALLOC_REPLY_BYTES 12u
#define ZZAPP_ALLOC_SLOT 12u
#define ZZAPP_ALLOC_GENERATION 16u
#define ZZAPP_ALLOC_EXT_REPLY_BYTES 20u
#define ZZAPP_LIST_STATE 0u
#define ZZAPP_LIST_GENERATION 4u
#define ZZAPP_LIST_COMMITTED 8u
#define ZZAPP_LIST_LIMIT 12u
#define ZZAPP_LIST_CPU_MS 16u
#define ZZAPP_LIST_SERVICE_MS 20u
#define ZZAPP_LIST_REASONS 24u
#define ZZAPP_LIST_NAME 28u
#define ZZAPP_LIST_REPLY_BYTES 48u
#define ZZAPP_F_LAUNCHER_HEARTBEAT (1u << 18)
#endif

#include "zzrun_policy.h"

static struct zzrun_exit_in exit_in(uint32_t state, uint32_t fault,
                                    uint32_t elapsed, int stop_asked,
                                    int user_stop, int pending, int failed)
{
    struct zzrun_exit_in in;

    memset(&in, 0, sizeof(in));
    in.state = state;
    in.fault = fault;
    in.elapsed_ms = elapsed;
    in.generation = 7u;
    in.last_generation = 7u;
    in.entered = 1;
    in.stop_asked = stop_asked;
    in.user_stop = user_stop;
    in.output_pending = pending;
    in.output_failed = failed;
    return in;
}

static void test_output_failure_does_not_block_exit(void)
{
    struct zzrun_exit_in faulted =
        exit_in(ZZRUN_ST_FAULTED, 0u, 0u, 0, 0, 1, 1);
    struct zzrun_exit_in idle = exit_in(ZZRUN_ST_IDLE, 0u, 0u, 0, 0, 1, 1);
    struct zzrun_exit_in running =
        exit_in(ZZRUN_ST_RUNNING, 0u, 1000u, 1, 0, 1, 1);
    struct zzrun_exit_in draining =
        exit_in(ZZRUN_ST_FAULTED, 0u, 0u, 0, 0, 1, 0);

    assert(zzrun_output_blocks_exit(1, 1) == 0);
    assert(zzrun_output_blocks_exit(1, 0) == 1);
    assert(zzrun_run_should_exit(&faulted) == 1);
    assert(zzrun_run_should_exit(&idle) == 1);
    assert(zzrun_run_should_exit(&running) == 1);
    assert(zzrun_run_should_exit(&draining) == 0);
}

static void test_ctrl_c_ends_even_if_output_remains(void)
{
    struct zzrun_exit_in early =
        exit_in(ZZRUN_ST_FAULTED, 0u, 999u, 1, 1, 1, 0);
    struct zzrun_exit_in due =
        exit_in(ZZRUN_ST_FAULTED, 0u, 1000u, 1, 1, 1, 0);
    struct zzrun_exit_in running =
        exit_in(ZZRUN_ST_RUNNING, 0u, 1000u, 1, 1, 1, 0);
    struct zzrun_exit_in not_yet =
        exit_in(ZZRUN_ST_RUNNING, 0u, 999u, 1, 1, 1, 0);

    assert(zzrun_run_should_exit(&early) == 0);
    assert(zzrun_run_should_exit(&due) == 1);
    assert(zzrun_run_should_exit(&running) == 1);
    assert(zzrun_run_should_exit(&not_yet) == 0);
}

static void test_clean_exit_still_drains(void)
{
    struct zzrun_exit_in idle = exit_in(ZZRUN_ST_IDLE, 0u, 0u, 0, 0, 1, 0);
    struct zzrun_exit_in done = exit_in(ZZRUN_ST_IDLE, 0u, 0u, 0, 0, 0, 0);
    struct zzrun_exit_in watchdog =
        exit_in(ZZRUN_ST_RUNNING, ZZRUN_FAULT_WATCHDOG_RESTART, 0u, 0, 0, 0, 0);

    assert(zzrun_run_should_exit(&idle) == 0);
    assert(zzrun_run_should_exit(&done) == 1);
    assert(zzrun_run_should_exit(&watchdog) == 1);
}

static void test_generation_change_ends_even_if_new_app_is_live(void)
{
    struct zzrun_exit_in running =
        exit_in(ZZRUN_ST_RUNNING, 0u, 0u, 0, 0, 1, 0);
    struct zzrun_exit_in starting =
        exit_in(ZZRUN_ST_STARTING, 0u, 0u, 0, 0, 0, 0);
    struct zzrun_exit_in loading =
        exit_in(ZZRUN_ST_LOADING, 0u, 0u, 0, 0, 0, 0);

    running.generation = 8u;
    starting.generation = 8u;
    loading.generation = 8u;
    assert(zzrun_run_should_exit(&running) == 1);
    assert(zzrun_run_should_exit(&starting) == 1);
    assert(zzrun_run_should_exit(&loading) == 1);
}

static void test_keyboard_read_gated_on_running(void)
{
    assert(zzrun_keyboard_read_allowed(ZZRUN_ST_RUNNING, 1, 0, 4u, 4u, 1u, 0,
                                       0) == 1);
    assert(zzrun_keyboard_read_allowed(ZZRUN_ST_FAULTED, 1, 0, 4u, 4u, 1u, 0,
                                       0) == 0);
    assert(zzrun_keyboard_read_allowed(ZZRUN_ST_STOPPING, 1, 0, 4u, 4u, 1u, 0,
                                       0) == 0);
    assert(zzrun_keyboard_read_allowed(ZZRUN_ST_IDLE, 1, 0, 4u, 4u, 1u, 0, 0) ==
           0);
    assert(zzrun_keyboard_read_allowed(ZZRUN_ST_RUNNING, 1, 1, 4u, 4u, 1u, 0,
                                       0) == 0);
    assert(zzrun_keyboard_read_allowed(ZZRUN_ST_RUNNING, 0, 0, 4u, 4u, 1u, 0,
                                       0) == 0);
    assert(zzrun_keyboard_read_allowed(ZZRUN_ST_RUNNING, 1, 0, 8u, 4u, 1u, 0,
                                       0) == 0);
    assert(zzrun_keyboard_read_allowed(ZZRUN_ST_RUNNING, 1, 0, 4u, 4u, 0u, 0,
                                       0) == 0);
    assert(zzrun_keyboard_read_allowed(ZZRUN_ST_RUNNING, 1, 0, 4u, 4u, 1u, 1,
                                       0) == 0);
}

static void test_noninteractive_read_is_not_want_gated(void)
{
    assert(zzrun_async_read_allowed(ZZRUN_ST_RUNNING, 1, 0, 0u, 0u, 16u, 0, 0) ==
           1);
    assert(zzrun_async_read_allowed(ZZRUN_ST_RUNNING, 1, 0, 16u, 0u, 16u, 0,
                                    0) == 0);
    assert(zzrun_async_read_allowed(ZZRUN_ST_FAULTED, 1, 0, 0u, 0u, 16u, 0, 0) ==
           0);
    assert(zzrun_async_read_allowed(ZZRUN_ST_RUNNING, 1, 1, 0u, 0u, 16u, 0, 0) ==
           0);
}

static void test_audio_epoch_discards_before_accept(void)
{
    assert(zzrun_audio_epoch_replaced(0, 0u, 1u) == 0);
    assert(zzrun_audio_epoch_replaced(1, 1u, 1u) == 0);
    assert(zzrun_audio_epoch_replaced(1, 1u, 2u) == 1);
    assert(zzrun_audio_epoch_replaced(1, 0xffffffffu, 0u) == 1);
}

static void test_stop_payload_carries_generation(void)
{
    uint8_t payload[ZZRUN_STOP_REQ_BYTES];

    memset(payload, 0x5a, sizeof(payload));
    zzrun_stop_payload(payload, 0x01020304u);
    assert(payload[0] == 0x01);
    assert(payload[1] == 0x02);
    assert(payload[2] == 0x03);
    assert(payload[3] == 0x04);
    assert(ZZRUN_STOP_REQ_BYTES == 4u);
}

static void test_finish_prompt_is_not_stdout(void)
{
    assert(zzrun_finish_prompt(0, 1) == ZZRUN_FINISH_NONE);
    assert(zzrun_finish_prompt(1, 1) == ZZRUN_FINISH_RETURN_ON_READ_HANDLE);
    assert(zzrun_finish_prompt(1, 0) == ZZRUN_FINISH_WAIT_ON_USER_CONSOLE);
}

static void test_ahi_open_failure_exits_20(void)
{
    assert(zzrun_process_exit(0, 0, 0, 1) == 20);
    assert(zzrun_process_exit(0, 0, 0, 0) == 0);
    assert(zzrun_process_exit(7, 0, 0, 1) == 20);
    assert(zzrun_process_exit(0, 1, 0, 0) == 20);
    assert(zzrun_process_exit(0, 0, 1, 0) == 20);
}

static void test_busy_load_begin_claims_nothing(void)
{
    assert(zzrun_load_generation_to_claim(1, 1, 7u, 8u, 0, 0, 0u) == 0u);
    assert(zzrun_load_generation_to_claim(1, 1, 7u, 8u, 0, 1, 8u) == 0u);
    assert(zzrun_load_generation_to_claim(1, 0, 7u, 8u, 0, 0, 0u) == 8u);
    assert(zzrun_load_generation_to_claim(1, 0, 7u, 9u, 0, 1, 8u) == 8u);
    assert(zzrun_load_generation_to_claim(1, 0, 7u, 7u, 0, 0, 0u) == 0u);
    assert(zzrun_load_generation_to_claim(1, 0, 7u, 8u, 1, 0, 0u) == 0u);
    assert(zzrun_load_generation_to_claim(0, 0, 7u, 8u, 0, 0, 0u) == 0u);
}

static void test_busy_deferred_operations_retry_only_until_deadline(void)
{
    const int busy = -4;
    const int io_error = -1;

    assert(zzrun_deferred_should_retry(busy, busy, 100u, 100u));
    assert(zzrun_deferred_should_retry(busy, busy, 100u,
                                       100u + ZZRUN_BUSY_RETRY_MS - 1u));
    assert(!zzrun_deferred_should_retry(busy, busy, 100u,
                                        100u + ZZRUN_BUSY_RETRY_MS));
    assert(!zzrun_deferred_should_retry(io_error, busy, 100u, 101u));
}

static void test_stale_file_request_cannot_publish_or_claim(void)
{
    assert(zzrun_file_request_is_current(8u, 3u, 8u, 3u));
    assert(!zzrun_file_request_is_current(8u, 3u, 9u, 3u));
    assert(!zzrun_file_request_is_current(8u, 3u, 8u, 4u));
    assert(!zzrun_file_service_generation_owned(0u, 0u));
}

/* serve_files() captures the live generation once, then every open/read/seek/
 * close reply is fenced against that same value. */
static void test_owning_file_service_accepts_open_read_seek_and_close(void)
{
    uint32_t generation = 0x31u;

    assert(zzrun_file_service_generation_owned(generation, generation));
    assert(zzrun_file_request_is_current(2u, generation, 2u, generation));
    assert(zzrun_file_request_is_current(4u, generation, 4u, generation));
    assert(zzrun_file_request_is_current(6u, generation, 6u, generation));
    assert(zzrun_file_request_is_current(8u, generation, 8u, generation));
    assert(!zzrun_file_service_generation_owned(generation, generation + 1u));
}

static void test_empty_argument_is_allowed(void)
{
    assert(zzrun_arg_length_allowed(0u));
    assert(zzrun_arg_length_allowed(127u));
    assert(!zzrun_arg_length_allowed(128u));
}

static void test_async_console_write_advances_only_on_completion(void)
{
    assert(zzrun_console_write_advance(64u, 64) == 64u);
    assert(zzrun_console_write_advance(64u, 17) == 17u);
    assert(zzrun_console_write_advance(64u, 0) == 0u);
    assert(zzrun_console_write_advance(64u, 65) == 0u);
    assert(!zzrun_console_write_failed(64u, 64));
    assert(zzrun_console_write_failed(64u, 17));
    assert(zzrun_console_write_failed(64u, 0));
}

/* The just-completed write advanced the launcher-owned tail from 218 to
 * 234. A Zorro read still returning 218 must not re-send 218..250. */
static void test_owned_cursor_ignores_stale_readback(void)
{
    struct zzrun_owned_cursor cursor = { 0 };

    assert(zzrun_owned_cursor_adopt(&cursor, 7u, 0xfffffff0u) == 0xfffffff0u);
    assert(zzrun_owned_cursor_advance(&cursor, 32u) == 16u);
    assert(zzrun_owned_cursor_adopt(&cursor, 7u, 0xfffffff0u) == 16u);
    assert(zzrun_owned_cursor_adopt(&cursor, 8u, 3u) == 3u);
}

static void test_terminal_output_drains_before_exit(void)
{
    struct zzrun_exit_in idle =
        exit_in(ZZRUN_ST_IDLE, 0u, 0u, 0, 0, 1, 0);
    struct zzrun_exit_in faulted =
        exit_in(ZZRUN_ST_FAULTED, 0u, 0u, 0, 0, 1, 0);
    struct zzrun_exit_in failed =
        exit_in(ZZRUN_ST_IDLE, 0u, 0u, 0, 0, 1, 1);

    assert(zzrun_run_should_exit(&idle) == 0);
    assert(zzrun_run_should_exit(&faulted) == 0);
    assert(zzrun_run_should_exit(&failed) == 1);
}

static void test_raw_ctrl_c_requests_stop(void)
{
    assert(zzrun_rawkey_requests_stop(0x33u, 0x0008u, 0x0008u));
    assert(!zzrun_rawkey_requests_stop(0x20u, 0x0008u, 0x0008u)); /* Ctrl-A */
    assert(!zzrun_rawkey_requests_stop(0x22u, 0x0008u, 0x0008u)); /* Ctrl-D */
    assert(!zzrun_rawkey_requests_stop(0x33u, 0u, 0x0008u));
}

static void test_screen_buffer_safe_messages_are_all_reaped(void)
{
    uint32_t pending = 0;

    pending = zzrun_screen_safe_started(pending);
    pending = zzrun_screen_safe_started(pending);
    pending = zzrun_screen_safe_completed(pending);
    assert(pending == 1u);
    pending = zzrun_screen_safe_completed(pending);
    assert(pending == 0u);
}

static void test_launcher_output_reserves_status_room(void)
{
    uint32_t diagnostic_limit = ZZRUN_LAUNCHER_OUTPUT_BYTES -
                                ZZRUN_LAUNCHER_STATUS_RESERVE_BYTES;

    assert(zzrun_launcher_output_accept(diagnostic_limit - 1u, 1u, 1));
    assert(!zzrun_launcher_output_accept(diagnostic_limit, 1u, 1));
    assert(zzrun_launcher_output_accept(diagnostic_limit, 1u, 0));
    assert(!zzrun_launcher_output_accept(ZZRUN_LAUNCHER_OUTPUT_BYTES, 1u, 0));
}

static void test_launcher_output_orders_queued_messages_before_app_output(void)
{
    assert(zzrun_output_next_source(0u, 0u) == ZZRUN_OUTPUT_NONE);
    assert(zzrun_output_next_source(12u, 0u) == ZZRUN_OUTPUT_LAUNCHER);
    assert(zzrun_output_next_source(0u, 12u) == ZZRUN_OUTPUT_APP);
    assert(zzrun_output_next_source(12u, 12u) == ZZRUN_OUTPUT_LAUNCHER);
}

static void put_be32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

static uint32_t get_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static void test_slot_claim_selects_every_following_request(void)
{
    uint8_t claim[ZZAPP_ALLOC_EXT_REPLY_BYTES] = { 0 };
    uint8_t legacy[] = { 1u, 2u, 3u, 4u };
    uint8_t request[ZZAPP_MAILBOX_PAYLOAD_BYTES];
    struct zzrun_claim_reply parsed;

    put_be32(claim + ZZAPP_ALLOC_ARM_ADDRESS, 0x01234567u);
    put_be32(claim + ZZAPP_ALLOC_SIZE, 0x00094000u);
    put_be32(claim + ZZAPP_ALLOC_HOST_OFFSET, 0x00100000u);
    put_be32(claim + ZZAPP_ALLOC_SLOT, 2u);
    put_be32(claim + ZZAPP_ALLOC_GENERATION, 0x10203040u);
    assert(zzrun_claim_reply_decode(claim, sizeof(claim), 1, &parsed));
    assert(parsed.slot == 2u);
    assert(parsed.generation == 0x10203040u);
    assert(zzrun_request_build(request, legacy, sizeof(legacy), 1, parsed.slot,
                               parsed.generation) ==
           ZZAPP_MAILBOX_PAYLOAD_BYTES);
    assert(memcmp(request, legacy, sizeof(legacy)) == 0);
    assert(get_be32(request + ZZAPP_SEL_SLOT) == 2u);
    assert(get_be32(request + ZZAPP_SEL_GENERATION) == 0x10203040u);
}

static void test_slot_replacement_requires_a_changed_generation(void)
{
    assert(!zzrun_slot_replaced(1, 2u, 0x10203040u, 2u, 0x10203040u,
                                ZZRUN_ST_IDLE));
    assert(!zzrun_slot_replaced(1, 2u, 0x10203040u, 2u, 0x10203040u,
                                ZZRUN_ST_FAULTED));
    assert(zzrun_slot_replaced(1, 2u, 0x10203040u, 2u, 0x10203041u,
                               ZZRUN_ST_IDLE));
    assert(!zzrun_slot_replaced(1, 2u, 0x10203040u, 1u, 0x10203041u,
                                ZZRUN_ST_RUNNING));
    assert(zzrun_slot_replaced(0, 0u, 0x10203040u, 0u, 0x10203041u,
                               ZZRUN_ST_RUNNING));
}

static void test_low_memory_counter_reports_changes_when_advertised(void)
{
    struct zzrun_low_memory_state state = { 0 };

    assert(!zzrun_low_memory_changed(&state, 0u, 1u, 1u));
    assert(!zzrun_low_memory_changed(&state, ZZAPP_CAP_LOW_MEMORY, 1u, 0u));
    assert(zzrun_low_memory_changed(&state, ZZAPP_CAP_LOW_MEMORY, 1u, 1u));
    assert(!zzrun_low_memory_changed(&state, ZZAPP_CAP_LOW_MEMORY, 1u, 1u));
    assert(zzrun_low_memory_changed(&state, ZZAPP_CAP_LOW_MEMORY, 1u, 2u));
}

static void test_low_memory_counter_handles_wrap(void)
{
    struct zzrun_low_memory_state state = { 0 };

    assert(zzrun_low_memory_changed(&state, ZZAPP_CAP_LOW_MEMORY, 1u,
                                    UINT32_MAX));
    assert(zzrun_low_memory_changed(&state, ZZAPP_CAP_LOW_MEMORY, 1u, 0u));
}

static void test_low_memory_counter_resets_for_a_new_generation(void)
{
    struct zzrun_low_memory_state state = { 0 };

    assert(zzrun_low_memory_changed(&state, ZZAPP_CAP_LOW_MEMORY, 1u, 1u));
    assert(!zzrun_low_memory_changed(&state, ZZAPP_CAP_LOW_MEMORY, 2u, 0u));
    assert(zzrun_low_memory_changed(&state, ZZAPP_CAP_LOW_MEMORY, 2u, 1u));
}

static void test_legacy_request_shape_is_unchanged_without_slots(void)
{
    uint8_t legacy[] = { 0xaa, 0xbb, 0xcc, 0xdd };
    uint8_t request[ZZAPP_MAILBOX_PAYLOAD_BYTES];

    memset(request, 0x5a, sizeof(request));
    assert(zzrun_request_build(request, legacy, sizeof(legacy), 0, 2u,
                               0x10203040u) == sizeof(legacy));
    assert(memcmp(request, legacy, sizeof(legacy)) == 0);
    assert(request[sizeof(legacy)] == 0x5a);
}

static void test_maxmem_requires_positive_decimal_mib(void)
{
    uint32_t mib = 0;

    assert(zzrun_parse_maxmem("64", &mib) && mib == 64u);
    assert(zzrun_parse_maxmem("4294967295", &mib) && mib == UINT32_MAX);
    assert(!zzrun_parse_maxmem("0", &mib));
    assert(!zzrun_parse_maxmem("", &mib));
    assert(!zzrun_parse_maxmem("64MiB", &mib));
    assert(!zzrun_parse_maxmem("4294967296", &mib));
}

static void test_hostmem_requires_positive_decimal_mib(void)
{
    uint32_t mib = 7u;

    assert(zzrun_parse_hostmem("8", &mib) && mib == 8u);
    assert(zzrun_parse_hostmem("62", &mib) && mib == 62u);
    assert(zzrun_parse_hostmem("4095", &mib) && mib == 4095u);
    mib = 7u;
    assert(!zzrun_parse_hostmem("4096", &mib) && mib == 7u);
    assert(!zzrun_parse_hostmem("4294967296", &mib));
    assert(!zzrun_parse_hostmem("0", &mib));
    assert(!zzrun_parse_hostmem("", &mib));
    assert(!zzrun_parse_hostmem("-1", &mib));
    assert(!zzrun_parse_hostmem("8MiB", &mib));
    assert(!zzrun_parse_hostmem(" 8", &mib));
    assert(!zzrun_parse_hostmem(NULL, &mib));
    assert(!zzrun_parse_hostmem("8", NULL));
}

static void test_hostmem_is_rejected_without_the_host_limit_capability(void)
{
    const uint32_t slots = ZZAPP_CAP_SLOTS;
    const uint32_t both = ZZAPP_CAP_SLOTS | ZZAPP_CAP_HOST_LIMIT;

    assert(zzrun_hostmem_decision(0u, 8u) == ZZRUN_HOSTMEM_REJECT);
    assert(zzrun_hostmem_decision(slots, 8u) == ZZRUN_HOSTMEM_REJECT);
    assert(zzrun_hostmem_decision(ZZAPP_CAP_HOST_LIMIT, 8u) ==
           ZZRUN_HOSTMEM_REJECT);
    assert(zzrun_hostmem_decision(both, 8u) == ZZRUN_HOSTMEM_SEND);
    assert(zzrun_hostmem_decision(both, 4095u) == ZZRUN_HOSTMEM_SEND);
    assert(zzrun_hostmem_decision(0u, 0u) == ZZRUN_HOSTMEM_OMIT);
    assert(zzrun_hostmem_decision(both, 0u) == ZZRUN_HOSTMEM_OMIT);
}

static void test_load_begin_payload_places_both_limits(void)
{
    uint8_t out[ZZRUN_LOAD_BEGIN_BYTES];

    assert(ZZAPP_LOAD_HOST_VISIBLE_MIB == 4u);
    memset(out, 0x5a, sizeof(out));
    assert(zzrun_load_begin_payload(out, 1, 256u, 1, 32u) ==
           ZZRUN_LOAD_BEGIN_BYTES);
    assert(zzrun_be32(out + ZZAPP_LOAD_MEM_LIMIT_MIB) == 256u);
    assert(zzrun_be32(out + ZZAPP_LOAD_HOST_VISIBLE_MIB) == 32u);

    /* HOSTMEM alone leaves the memory cap at 0, meaning none. */
    memset(out, 0x5a, sizeof(out));
    assert(zzrun_load_begin_payload(out, 0, 0u, 1, 8u) ==
           ZZRUN_LOAD_BEGIN_BYTES);
    assert(zzrun_be32(out + ZZAPP_LOAD_MEM_LIMIT_MIB) == 0u);
    assert(zzrun_be32(out + ZZAPP_LOAD_HOST_VISIBLE_MIB) == 8u);

    /* MAXMEM alone keeps the four-byte request the runtime already saw. */
    memset(out, 0x5a, sizeof(out));
    assert(zzrun_load_begin_payload(out, 1, 64u, 0, 0u) == 4u);
    assert(zzrun_be32(out + ZZAPP_LOAD_MEM_LIMIT_MIB) == 64u);

    assert(zzrun_load_begin_payload(out, 0, 0u, 0, 0u) == 0u);
}

static void test_hostmem_no_memory_distinguishes_ceiling_from_other_causes(void)
{
    assert(zzrun_hostmem_failure(63u, 6) == ZZRUN_HOSTMEM_FAIL_TOO_LARGE);
    assert(zzrun_hostmem_failure(4095u, 6) == ZZRUN_HOSTMEM_FAIL_TOO_LARGE);
    /* At or below the ceiling NO_MEMORY may be the staging arena or the
     * shared heap, so it is not blamed on the request alone. */
    assert(zzrun_hostmem_failure(62u, 6) == ZZRUN_HOSTMEM_FAIL_NO_MEMORY);
    assert(zzrun_hostmem_failure(1u, 6) == ZZRUN_HOSTMEM_FAIL_NO_MEMORY);
    assert(zzrun_hostmem_failure(63u, 0) == ZZRUN_HOSTMEM_FAIL_NONE);
    assert(zzrun_hostmem_failure(63u, 3) == ZZRUN_HOSTMEM_FAIL_NONE);
    assert(zzrun_hostmem_failure(0u, 6) == ZZRUN_HOSTMEM_FAIL_NONE);
}

static void test_extended_query_reply_is_required_and_carries_every_cap(void)
{
    uint8_t reply[ZZAPP_QUERY_EXT_REPLY_BYTES];
    struct zzrun_slots_query query;
    const uint32_t caps = 0x1ffu;

    memset(reply, 0, sizeof(reply));
    zzrun_put_be32(reply + 12u, caps);
    zzrun_put_be32(reply + ZZAPP_QUERY_SLOTS, 4u);
    zzrun_put_be32(reply + ZZAPP_QUERY_FREE_SLOTS, 4u);
    assert(zzrun_slots_query_decode(reply, sizeof(reply), &query));
    assert(zzrun_be32(reply + 12u) & ZZAPP_CAP_HOST_LIMIT);
    /* A legacy 16-byte reply has no slot counts: not a usable runtime. */
    assert(!zzrun_slots_query_decode(reply, 16u, &query));
}

static void test_block_reason_names_match_protocol(void)
{
    assert(strcmp(zzrun_block_reason_name(ZZRUN_BLOCK_NONE), "none") == 0);
    assert(strcmp(zzrun_block_reason_name(ZZRUN_BLOCK_FILE), "file") == 0);
    assert(strcmp(zzrun_block_reason_name(ZZRUN_BLOCK_CONSOLE), "console") == 0);
    assert(strcmp(zzrun_block_reason_name(ZZRUN_BLOCK_EVENT), "event") == 0);
    assert(strcmp(zzrun_block_reason_name(ZZRUN_BLOCK_SLEEP), "sleep") == 0);
    assert(strcmp(zzrun_block_reason_name(ZZRUN_BLOCK_VSYNC), "vsync") == 0);
    assert(strcmp(zzrun_block_reason_name(ZZRUN_BLOCK_AUDIO), "audio") == 0);
    assert(strcmp(zzrun_block_reason_name(7u), "unknown") == 0);
    assert(strcmp(zzrun_block_reason_name(255u), "unknown") == 0);
}

static void test_list_reply_decoding(void)
{
    uint8_t reply[ZZAPP_LIST_REPLY_BYTES] = { 0 };
    struct zzrun_list_reply parsed;

    put_be32(reply + ZZAPP_LIST_STATE, ZZRUN_ST_RUNNING);
    put_be32(reply + ZZAPP_LIST_GENERATION, 0x10203040u);
    put_be32(reply + ZZAPP_LIST_COMMITTED, 12u * 1024u * 1024u);
    put_be32(reply + ZZAPP_LIST_LIMIT, 64u * 1024u * 1024u);
    put_be32(reply + ZZAPP_LIST_CPU_MS, 2500u);
    put_be32(reply + ZZAPP_LIST_SERVICE_MS, 17u);
    reply[ZZAPP_LIST_REASONS] = 3u;
    reply[ZZAPP_LIST_REASONS + 1u] = 2u;
    memcpy(reply + ZZAPP_LIST_NAME, "quake", 6u);
    assert(zzrun_list_reply_decode(reply, sizeof(reply), &parsed));
    assert(parsed.state == ZZRUN_ST_RUNNING);
    assert(parsed.generation == 0x10203040u);
    assert(parsed.committed_bytes == 12u * 1024u * 1024u);
    assert(parsed.limit_bytes == 64u * 1024u * 1024u);
    assert(parsed.cpu_ms == 2500u);
    assert(parsed.block_reason == 3u);
    assert(parsed.terminal_reason == 2u);
    assert(strcmp(parsed.name, "quake") == 0);
    assert(strcmp(zzrun_block_reason_name(parsed.block_reason), "event") == 0);
    assert(strcmp(zzrun_terminal_reason_name(parsed.terminal_reason),
                  "forced stop") == 0);
    assert(strcmp(zzrun_terminal_reason_name(9u), "quarantined") == 0);
}

/* The flags word carries AAR values: READ 1, WRITE 2, CREATE 4, TRUNCATE 8,
 * EXCL 16. Every valid combination has one plan; everything else, including
 * unknown bits, is refused before AmigaDOS is touched. */
static void test_open_flags_map_to_one_plan_each(void)
{
    static const struct {
        uint32_t flags;
        enum zzrun_open_plan plan;
    } valid[] = {
        {1u, ZZRUN_OPEN_READ_EXISTING},
        {2u, ZZRUN_OPEN_UPDATE_EXISTING},
        {3u, ZZRUN_OPEN_UPDATE_EXISTING},
        {2u | 8u, ZZRUN_OPEN_TRUNCATE_EXISTING},
        {3u | 8u, ZZRUN_OPEN_TRUNCATE_EXISTING},
        {2u | 4u, ZZRUN_OPEN_CREATE_KEEP},
        {3u | 4u, ZZRUN_OPEN_CREATE_KEEP},
        {2u | 4u | 8u, ZZRUN_OPEN_CREATE_REPLACE},
        {3u | 4u | 8u, ZZRUN_OPEN_CREATE_REPLACE},
        {2u | 4u | 16u, ZZRUN_OPEN_CREATE_EXCLUSIVE},
        {3u | 4u | 16u, ZZRUN_OPEN_CREATE_EXCLUSIVE},
        {2u | 4u | 8u | 16u, ZZRUN_OPEN_CREATE_EXCLUSIVE},
        {3u | 4u | 8u | 16u, ZZRUN_OPEN_CREATE_EXCLUSIVE},
    };
    uint32_t flags;
    size_t i;

    assert(ZZAPP_FILE_READ == 1u && ZZAPP_FILE_WRITE == 2u &&
           ZZAPP_FILE_CREATE == 4u && ZZAPP_FILE_TRUNCATE == 8u &&
           ZZAPP_FILE_EXCL == 16u);
    for (flags = 0; flags < 256u; flags++) {
        enum zzrun_open_plan expect = ZZRUN_OPEN_INVALID;

        for (i = 0; i < sizeof(valid) / sizeof(valid[0]); i++)
            if (valid[i].flags == flags)
                expect = valid[i].plan;
        assert(zzrun_open_plan_for_flags(flags) == expect);
    }
    assert(zzrun_open_plan_for_flags(0x80000000u | 3u) ==
           ZZRUN_OPEN_INVALID);
    assert(zzrun_open_plan_for_flags(0xffffffffu) == ZZRUN_OPEN_INVALID);
}

static void test_open_flag_refusals_are_the_invalid_combinations(void)
{
    /* Neither READ nor WRITE. */
    assert(zzrun_open_plan_for_flags(0u) == ZZRUN_OPEN_INVALID);
    assert(zzrun_open_plan_for_flags(ZZAPP_FILE_CREATE) ==
           ZZRUN_OPEN_INVALID);
    /* CREATE or TRUNCATE without WRITE. */
    assert(zzrun_open_plan_for_flags(ZZAPP_FILE_READ | ZZAPP_FILE_CREATE) ==
           ZZRUN_OPEN_INVALID);
    assert(zzrun_open_plan_for_flags(ZZAPP_FILE_READ | ZZAPP_FILE_TRUNCATE) ==
           ZZRUN_OPEN_INVALID);
    /* EXCL without CREATE. */
    assert(zzrun_open_plan_for_flags(ZZAPP_FILE_WRITE | ZZAPP_FILE_EXCL) ==
           ZZRUN_OPEN_INVALID);
    assert(zzrun_open_plan_for_flags(ZZAPP_FILE_READ | ZZAPP_FILE_WRITE |
                                     ZZAPP_FILE_TRUNCATE |
                                     ZZAPP_FILE_EXCL) == ZZRUN_OPEN_INVALID);
    /* Unknown bit. */
    assert(zzrun_open_plan_for_flags(ZZAPP_FILE_READ | 32u) ==
           ZZRUN_OPEN_INVALID);
    /* The old CREATE (create or replace) is CREATE|TRUNCATE now. */
    assert(zzrun_open_plan_for_flags(ZZAPP_FILE_WRITE | ZZAPP_FILE_CREATE |
                                     ZZAPP_FILE_TRUNCATE) ==
           ZZRUN_OPEN_CREATE_REPLACE);
}

/* Kit 66: "zzrun status" tried to load a file called "status". */
static void test_status_keyword_is_case_insensitive(void)
{
    assert(zzrun_is_status_keyword("STATUS"));
    assert(zzrun_is_status_keyword("status"));
    assert(zzrun_is_status_keyword("Status"));
    assert(!zzrun_is_status_keyword("status.elf"));
    assert(!zzrun_is_status_keyword("STATU"));
    assert(!zzrun_is_status_keyword(""));
}
static void test_launcher_lease_is_advertised(void)
{
    const uint32_t flags = 0x1234u;

    assert(zzrun_launcher_lease_flags(flags) ==
           (flags | ZZAPP_F_LAUNCHER_HEARTBEAT));
}
static void test_lease_renewal_never_crosses_generations(void)
{
    assert(zzrun_lease_renew_allowed(7u, 7u, 7u));
    assert(!zzrun_lease_renew_allowed(7u, 8u, 8u));
    assert(!zzrun_lease_renew_allowed(7u, 7u, 8u));
    assert(!zzrun_lease_renew_allowed(0u, 0u, 0u));
}
static void test_dos_packet_transport_and_error_are_distinct(void)
{
    assert(zzrun_dos_boolean_result(1, 1));
    assert(!zzrun_dos_boolean_result(1, 0));
    assert(!zzrun_dos_boolean_result(0, 1));
    assert(zzrun_dos_error_code(1, 42, -1) == 42);
    assert(zzrun_dos_error_code(0, 42, -1) == -1);
}
static void test_legacy_claim_passes_the_pre_load_gate(void)
{
    assert(!zzrun_lease_gate_stops(0u, 0));
    assert(!zzrun_lease_gate_stops(0u, 1));
    assert(zzrun_lease_gate_stops(7u, 0));
    assert(!zzrun_lease_gate_stops(7u, 1));
}
static void test_packet_wait_outlives_abandonment(void)
{
    assert(zzrun_packet_abandoned(1, 1));
    assert(zzrun_packet_abandoned(0, 0));
    assert(!zzrun_packet_abandoned(0, 1));
    assert(!zzrun_packet_keep_waiting(1));
    assert(zzrun_packet_keep_waiting(0));
}
static void test_bstr_encoding_matches_dos_packets(void)
{
    uint8_t buf[16];

    assert(zzrun_bstr_encode("Work:foo", buf, sizeof(buf)) == 10u);
    assert(buf[0] == 8);
    assert(memcmp(buf + 1, "Work:foo", 8) == 0);
    assert(buf[9] == 0);
    assert(zzrun_bstr_encode("", buf, sizeof(buf)) == 2u);
    assert(buf[0] == 0);
    assert(zzrun_bstr_encode("ab", buf, sizeof(buf)) == 4u);
    assert(buf[3] == 0);
    assert(zzrun_bstr_encode("abc", buf, 2u) == 0u);
    assert(strcmp(zzrun_dos_packet_path("Apps:demo.elf"), "demo.elf") == 0);
    assert(strcmp(zzrun_dos_packet_path("Work:Apps/demo.elf"),
                  "Apps/demo.elf") == 0);
    assert(strcmp(zzrun_dos_packet_path("relative/demo.elf"),
                  "relative/demo.elf") == 0);
    assert(zzrun_dos_packet_path(NULL) == NULL);
}

static void test_pending_packet_ctrl_c_still_requests_stop(void)
{
    assert(zzrun_ctrl_c_must_request_stop(1));
    assert(!zzrun_ctrl_c_must_request_stop(0));
}

static void test_load_break_consumed_by_packet_returns_warn(void)
{
    /* dos_packet_async() clears the signal and records user_stop. */
    assert(zzrun_load_failure_exit(0, 1) == 5);
    assert(zzrun_load_failure_exit(1, 0) == 5);
    assert(zzrun_load_failure_exit(1, 1) == 5);
    assert(zzrun_load_failure_exit(0, 0) == 20);
    assert(zzrun_user_break(0, 1));
    assert(zzrun_user_break(1, 0));
    assert(!zzrun_user_break(0, 0));
}


struct fake_dos_path {
    struct zzrun_dos_path_candidate candidates[2];
    struct zzrun_dos_path_reply replies[2];
    uintptr_t handlers[2];
    uint32_t directories[2];
    uintptr_t bstr_addresses[2];
    uint32_t bstr_bptrs[2];
    int advances[3];
    uint32_t candidate_count;
    uint32_t candidate_index;
    uint32_t resolve_count;
    uint32_t send_count;
    uint32_t bstr_bytes;
    uint8_t bstr[258];
};

static int fake_dos_resolve(void *context, int advance,
                            struct zzrun_dos_path_candidate *candidate)
{
    struct fake_dos_path *path = context;

    path->advances[path->resolve_count++] = advance;
    if (advance)
        path->candidate_index++;
    else
        path->candidate_index = 0;
    if (path->candidate_index >= path->candidate_count)
        return 0;
    *candidate = path->candidates[path->candidate_index];
    return 1;
}

static void fake_dos_send(void *context,
                          const struct zzrun_dos_path_candidate *candidate,
                          const uint8_t *name_bstr, uint32_t name_bstr_bytes,
                          uint32_t name_bstr_bptr,
                          struct zzrun_dos_path_reply *reply)
{
    struct fake_dos_path *path = context;
    uint32_t index = path->send_count++;

    assert(index < 2u);
    assert(name_bstr_bytes <= sizeof(path->bstr));
    assert(((uintptr_t)name_bstr & 3u) == 0u);
    assert(name_bstr_bptr == (uint32_t)((uintptr_t)name_bstr >> 2));
    path->handlers[index] = candidate->handler;
    path->directories[index] = candidate->directory;
    path->bstr_addresses[index] = (uintptr_t)name_bstr;
    path->bstr_bptrs[index] = name_bstr_bptr;
    path->bstr_bytes = name_bstr_bytes;
    memcpy(path->bstr, name_bstr, name_bstr_bytes);
    *reply = path->replies[index];
}


static void test_dos_path_dispatch_uses_assign_locks_and_retries(void)
{
    const int32_t not_found = 404;
    union {
        uintptr_t alignment;
        uint8_t bytes[260];
    } name_bstr;
    struct fake_dos_path path;
    int attempted;
    int usable;
    memset(&path, 0, sizeof(path));
    path.candidate_count = 1u;
    path.candidates[0].handler = 0x11110000u;
    path.candidates[0].directory = 0x1000u;
    path.replies[0].completed = 1;
    path.replies[0].result = 1;
    assert(zzrun_dos_dispatch_path("Apps:demo.elf", name_bstr.bytes, 258u,
                                   not_found, fake_dos_resolve, &path,
                                   fake_dos_send, &path, &attempted, &usable));
    assert(attempted && usable);
    assert(path.send_count == 1u);
    assert(path.handlers[0] == 0x11110000u);
    assert(path.directories[0] == 0x1000u);
    assert(path.bstr_bytes == 10u);
    assert(path.bstr[0] == 8u);
    assert(memcmp(path.bstr + 1, "demo.elf", 8u) == 0);
    assert(path.bstr[9] == 0u);

    memset(&path, 0, sizeof(path));
    path.candidate_count = 2u;
    path.candidates[0].handler = 0x11110000u;
    path.candidates[0].directory = 0x1000u;
    path.candidates[1].handler = 0x22220000u;
    path.candidates[1].directory = 0x2000u;
    path.replies[0].completed = 1;
    path.replies[0].error = not_found;
    path.replies[1].completed = 1;
    path.replies[1].result = 1;
    assert(zzrun_dos_dispatch_path("Apps:demo.elf", name_bstr.bytes, 258u,
                                   not_found, fake_dos_resolve, &path,
                                   fake_dos_send, &path, &attempted, &usable));
    assert(attempted && usable);
    assert(path.send_count == 2u);
    assert(path.handlers[0] == 0x11110000u);
    assert(path.handlers[1] == 0x22220000u);
    assert(path.directories[0] == 0x1000u);
    assert(path.directories[1] == 0x2000u);
    assert(path.advances[0] == 0);
    assert(path.advances[1] == 1);

    memset(&path, 0, sizeof(path));
    path.candidate_count = 2u;
    path.candidates[0].directory = 0x1000u;
    path.replies[0].completed = 1;
    path.replies[0].error = 5;
    assert(!zzrun_dos_dispatch_path("Apps:demo.elf", name_bstr.bytes, 258u,
                                    not_found, fake_dos_resolve, &path,
                                    fake_dos_send, &path, &attempted, &usable));
    assert(attempted && usable);
    assert(path.send_count == 1u);

    memset(&path, 0, sizeof(path));
    path.candidate_count = 2u;
    path.candidates[0].directory = 0x1000u;
    path.candidates[1].directory = 0x2000u;
    path.replies[0].completed = 1;
    path.replies[0].error = not_found;
    path.replies[1].completed = 1;
    path.replies[1].abandoned = 1;
    path.replies[1].result = 1;
    assert(!zzrun_dos_dispatch_path("Apps:demo.elf", name_bstr.bytes, 258u,
                                    not_found, fake_dos_resolve, &path,
                                    fake_dos_send, &path, &attempted, &usable));
    assert(attempted && !usable);
    assert(path.send_count == 2u);

    memset(&path, 0, sizeof(path));
    path.candidate_count = 1u;
    path.candidates[0].handler = 0x33330000u;
    path.candidates[0].directory = 0x3000u;
    path.replies[0].completed = 1;
    path.replies[0].result = 1;
    assert(zzrun_dos_dispatch_path("relative/demo.elf", name_bstr.bytes,
                                   258u, not_found, fake_dos_resolve, &path,
                                   fake_dos_send, &path, &attempted, &usable));
    assert(attempted && usable);
    assert(path.handlers[0] == 0x33330000u);
    assert(path.directories[0] == 0x3000u);
    assert(path.bstr[0] == strlen("relative/demo.elf"));
    assert(memcmp(path.bstr + 1, "relative/demo.elf",
                  strlen("relative/demo.elf")) == 0);
    assert(path.bstr_addresses[0] == (uintptr_t)name_bstr.bytes);
    assert(path.bstr_bptrs[0] ==
           (uint32_t)((uintptr_t)name_bstr.bytes >> 2));

    memset(&path, 0, sizeof(path));
    path.candidate_count = 1u;
    path.replies[0].completed = 1;
    path.replies[0].result = 1;
    assert(!zzrun_dos_dispatch_path("Apps:demo.elf", name_bstr.bytes + 1,
                                    258u, not_found, fake_dos_resolve, &path,
                                    fake_dos_send, &path, &attempted, &usable));
    assert(path.send_count == 0u);
}

struct fake_release {
    int delivered;
    int32_t result;
    int sends;
    int freed;
    int fallback;
    int forgotten;
};

static int fake_send_release(void *context, int32_t *result)
{
    struct fake_release *release = context;

    release->sends++;
    *result = release->result;
    return release->delivered;
}

static void fake_free_local(void *context)
{
    ((struct fake_release *)context)->freed++;
}

static void fake_fallback_release(void *context)
{
    ((struct fake_release *)context)->fallback++;
}

static void fake_forget_slot(void *context)
{
    ((struct fake_release *)context)->forgotten++;
}

static struct zzrun_dos_release_ops fake_release_ops(
    struct fake_release *release)
{
    struct zzrun_dos_release_ops ops;

    ops.context = release;
    ops.send_release = fake_send_release;
    ops.free_local = fake_free_local;
    ops.fallback_release = fake_fallback_release;
    ops.forget_slot = fake_forget_slot;
    return ops;
}

static void test_async_release_preserves_or_frees_resources_by_delivery(void)
{
    struct fake_release release;
    struct zzrun_dos_release_ops ops;
    int delivered;

    memset(&release, 0, sizeof(release));
    release.delivered = 1;
    release.result = 1;
    ops = fake_release_ops(&release);
    assert(zzrun_dos_close_slot(&ops, &delivered));
    assert(delivered && release.sends == 1 && release.freed == 1 &&
           release.forgotten == 1 && release.fallback == 0);

    memset(&release, 0, sizeof(release));
    release.delivered = 1;
    ops = fake_release_ops(&release);
    assert(!zzrun_dos_close_slot(&ops, &delivered));
    assert(delivered && release.freed == 1 && release.forgotten == 1 &&
           release.fallback == 0);

    memset(&release, 0, sizeof(release));
    ops = fake_release_ops(&release);
    assert(!zzrun_dos_close_slot(&ops, &delivered));
    assert(!delivered && release.freed == 0 && release.forgotten == 0 &&
           release.fallback == 0);
    zzrun_dos_end_or_close(&ops);
    assert(release.sends == 2 && release.freed == 0 && release.fallback == 1);

    memset(&release, 0, sizeof(release));
    release.delivered = 1;
    ops = fake_release_ops(&release);
    zzrun_dos_release_abandoned(&ops, 1);
    assert(release.sends == 1 && release.freed == 1 && release.fallback == 0);

    memset(&release, 0, sizeof(release));
    ops = fake_release_ops(&release);
    zzrun_dos_release_abandoned(&ops, 1);
    assert(release.sends == 1 && release.freed == 0 && release.fallback == 1);
    zzrun_dos_release_abandoned(&ops, 0);
    assert(release.sends == 1);
}

struct fake_writer {
    uint32_t current_generation;
    unsigned reads;
};

static uint32_t fake_read_generation(void *context)
{
    struct fake_writer *writer = context;

    writer->reads++;
    return writer->current_generation;
}

static void test_lost_generation_cannot_write_shared_window(void)
{
    struct fake_writer state;
    struct zzrun_shared_writer writer;
    uint8_t window[20];
    uint8_t before[20];
    static const uint8_t input[] = { 'c', 't', 'r', 'l', '-', 'c' };

    memset(window, 0xa5, sizeof(window));
    memcpy(before, window, sizeof(window));
    memset(&state, 0, sizeof(state));
    writer.context = &state;
    writer.owned_generation = 9u;
    writer.attached = 0;
    writer.read_current_generation = fake_read_generation;
    assert(!zzrun_shared_write_be32(&writer, window, 1u));
    assert(state.reads == 0u);
    assert(memcmp(window, before, sizeof(window)) == 0);

    writer.attached = 1;
    assert(!zzrun_shared_write_be32(&writer, window + 0, 1u)); /* stop */
    assert(!zzrun_shared_write_be32(&writer, window + 4, 2u)); /* file */
    assert(!zzrun_shared_write_bytes(&writer, window + 8, input, sizeof(input)));
    assert(!zzrun_shared_write_be32(&writer, window + 16, 3u)); /* AHI */
    assert(memcmp(window, before, sizeof(window)) == 0);
    assert(state.reads == 4u);

    state.current_generation = 9u;
    assert(zzrun_shared_write_be32(&writer, window + 0, 0x12345678u));
    assert(window[0] == 0x12u && window[1] == 0x34u &&
           window[2] == 0x56u && window[3] == 0x78u);
    assert(zzrun_shared_write_be32(&writer, window + 4, 2u)); /* event/log */
    assert(zzrun_shared_write_bytes(&writer, window + 8, input, sizeof(input)));
    assert(memcmp(window + 8, input, sizeof(input)) == 0);
    assert(zzrun_shared_write_be32(&writer, window + 16, 3u)); /* display */

    writer.owned_generation = 0u;
    state.current_generation = 0u;
    state.reads = 0u;
    assert(!zzrun_shared_write_be32(&writer, window, 4u));
    assert(state.reads == 0u);
}



















int main(void)
{
    test_output_failure_does_not_block_exit();
    test_ctrl_c_ends_even_if_output_remains();
    test_clean_exit_still_drains();
    test_generation_change_ends_even_if_new_app_is_live();
    test_keyboard_read_gated_on_running();
    test_noninteractive_read_is_not_want_gated();
    test_audio_epoch_discards_before_accept();
    test_stop_payload_carries_generation();
    test_finish_prompt_is_not_stdout();
    test_busy_load_begin_claims_nothing();
    test_ahi_open_failure_exits_20();
    test_busy_deferred_operations_retry_only_until_deadline();
    test_stale_file_request_cannot_publish_or_claim();
    test_owning_file_service_accepts_open_read_seek_and_close();
    test_empty_argument_is_allowed();
    test_async_console_write_advances_only_on_completion();
    test_owned_cursor_ignores_stale_readback();
    test_terminal_output_drains_before_exit();
    test_raw_ctrl_c_requests_stop();
    test_screen_buffer_safe_messages_are_all_reaped();
    test_launcher_output_reserves_status_room();
    test_launcher_output_orders_queued_messages_before_app_output();
    test_slot_replacement_requires_a_changed_generation();
    test_slot_claim_selects_every_following_request();
    test_low_memory_counter_reports_changes_when_advertised();
    test_low_memory_counter_handles_wrap();
    test_low_memory_counter_resets_for_a_new_generation();
    test_legacy_request_shape_is_unchanged_without_slots();
    test_maxmem_requires_positive_decimal_mib();
    test_hostmem_requires_positive_decimal_mib();
    test_hostmem_is_rejected_without_the_host_limit_capability();
    test_load_begin_payload_places_both_limits();
    test_hostmem_no_memory_distinguishes_ceiling_from_other_causes();
    test_extended_query_reply_is_required_and_carries_every_cap();
    test_block_reason_names_match_protocol();
    test_list_reply_decoding();
    test_open_flags_map_to_one_plan_each();
    test_open_flag_refusals_are_the_invalid_combinations();
    test_status_keyword_is_case_insensitive();
    test_launcher_lease_is_advertised();
    test_lease_renewal_never_crosses_generations();
    test_dos_packet_transport_and_error_are_distinct();
    test_legacy_claim_passes_the_pre_load_gate();
    test_packet_wait_outlives_abandonment();
    test_dos_path_dispatch_uses_assign_locks_and_retries();
    test_bstr_encoding_matches_dos_packets();
    test_pending_packet_ctrl_c_still_requests_stop();
    test_load_break_consumed_by_packet_returns_warn();
    test_async_release_preserves_or_frees_resources_by_delivery();
    test_lost_generation_cannot_write_shared_window();
    puts("zzrun_policy_test: ok");
    return 0;
}
