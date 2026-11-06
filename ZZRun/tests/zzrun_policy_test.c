#include <assert.h>
#include <stdio.h>
#include <string.h>

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

static void test_late_file_reply_does_not_match_new_request(void)
{
    assert(zzrun_file_reply_is_current(8u, 8u));
    assert(!zzrun_file_reply_is_current(8u, 9u));
    assert(!zzrun_file_reply_is_current(8u, 10u));
}

int main(void)
{
    test_output_failure_does_not_block_exit();
    test_ctrl_c_ends_even_if_output_remains();
    test_clean_exit_still_drains();
    test_keyboard_read_gated_on_running();
    test_noninteractive_read_is_not_want_gated();
    test_audio_epoch_discards_before_accept();
    test_stop_payload_carries_generation();
    test_finish_prompt_is_not_stdout();
    test_ahi_open_failure_exits_20();
    test_busy_load_begin_claims_nothing();
    test_late_file_reply_does_not_match_new_request();
    puts("zzrun_policy_test: ok");
    return 0;
}
