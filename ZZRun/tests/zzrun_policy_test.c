#include <assert.h>
#include <stdio.h>
#include <string.h>

#ifndef ZZAPP_CAP_SLOTS
#define ZZAPP_CAP_SLOTS (1u << 5)
#define ZZAPP_SLOT_COUNT 4u
#define ZZAPP_MAILBOX_PAYLOAD_BYTES 48u
#define ZZAPP_SEL_SLOT 40u
#define ZZAPP_SEL_GENERATION 44u
#define ZZAPP_LOAD_MEM_LIMIT_MIB 0u
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
    assert(strcmp(zzrun_terminal_reason_name(parsed.terminal_reason),
                  "forced stop") == 0);
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
    test_empty_argument_is_allowed();
    test_async_console_write_advances_only_on_completion();
    test_terminal_output_drains_before_exit();
    test_raw_ctrl_c_requests_stop();
    test_screen_buffer_safe_messages_are_all_reaped();
    test_launcher_output_reserves_status_room();
    test_launcher_output_orders_queued_messages_before_app_output();
    test_slot_replacement_requires_a_changed_generation();
    test_slot_claim_selects_every_following_request();
    test_legacy_request_shape_is_unchanged_without_slots();
    test_maxmem_requires_positive_decimal_mib();
    test_list_reply_decoding();
    test_status_keyword_is_case_insensitive();
    puts("zzrun_policy_test: ok");
    return 0;
}
