#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "zzusb_policy.h"

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "%s:%d: check failed: %s\n", \
                __FILE__, __LINE__, #expr); \
        return EXIT_FAILURE; \
    } \
} while (0)

int main(void)
{
    struct zzusb_worker_timer timer;
    const uint8_t requested_rate[3] = { 0x44, 0xac, 0x00 };
    const uint8_t current_rate[3] = { 0x44, 0xac, 0x00 };
    const uint8_t other_rate[3] = { 0x80, 0xbb, 0x00 };
    uint32_t bulk_offset;
    uint16_t input_index;
    uint32_t bulk_remaining;

    zzusb_worker_timer_init(&timer);
    CHECK(zzusb_worker_timer_arm(&timer, 20000));
    CHECK(!zzusb_worker_timer_arm(&timer, 1000));
    CHECK(zzusb_worker_timer_expire(&timer, 0) == 0);
    CHECK(timer.pending);
    CHECK(zzusb_worker_timer_expire(&timer, 1) == 20000);
    CHECK(!timer.pending);
    CHECK(zzusb_worker_timer_arm(&timer, 100000));
    zzusb_worker_timer_cancel(&timer);
    CHECK(!timer.pending);
    CHECK(timer.delay_us == 0);
    CHECK(zzusb_completion_needs_reply(0));
    CHECK(!zzusb_completion_needs_reply(1));

    CHECK(zzusb_bulk_resume_window(65536, 16384, &bulk_offset,
                                   &bulk_remaining));
    CHECK(bulk_offset == 16384);
    CHECK(bulk_remaining == 49152);
    CHECK(!zzusb_bulk_resume_window(16384, 16385, &bulk_offset,
                                    &bulk_remaining));
    CHECK(zzusb_mailbox_timer_available(1, 1, 0, 0, 0));
    CHECK(!zzusb_mailbox_timer_available(1, 0, 0, 0, 0));
    CHECK(zzusb_mailbox_timer_available(0, 0, 1, 1, 1));
    CHECK(!zzusb_mailbox_timer_available(0, 0, 1, 1, 0));
    CHECK(!zzusb_mailbox_timer_available(0, 0, 1, 0, 1));
    CHECK(zzusb_sync_command_timer_available(1, 1));
    CHECK(!zzusb_sync_command_timer_available(0, 1));
    CHECK(!zzusb_sync_command_timer_available(1, 0));
    CHECK(zzusb_quickio_available(1, 1, 1));
    CHECK(!zzusb_quickio_available(0, 1, 1));
    CHECK(!zzusb_quickio_available(1, 0, 1));
    CHECK(!zzusb_quickio_available(1, 1, 0));
    CHECK(zzusb_worker_timer_elapsed(1000, 91000, 100000) == 90000);
    CHECK(zzusb_worker_timer_elapsed(1000, 201000, 100000) == 100000);
    CHECK(zzusb_worker_timer_elapsed(0, 91000, 100000) == 0);
    CHECK(zzusb_worker_timer_elapsed(91000, 1000, 100000) == 0);
    CHECK(zzusb_worker_timer_remaining(8000, 7000) == 1000);
    CHECK(zzusb_worker_timer_remaining(8000, 8000) == 0);
    CHECK(zzusb_worker_timer_remaining(8000, 9000) == 0);
    zzusb_worker_timer_init(&timer);
    CHECK(zzusb_worker_timer_arm(&timer, 100000));
    CHECK(zzusb_worker_timer_account_elapsed(&timer, 95000) == 95000);
    CHECK(timer.pending && timer.delay_us == 5000);
    CHECK(zzusb_worker_timer_account_elapsed(&timer, 5000) == 0);
    CHECK(timer.pending && timer.delay_us == 5000);
    CHECK(zzusb_periodic_endpoint_matches(
        5, 0x82, 0x80, 5, 2, 0x80));
    CHECK(!zzusb_periodic_endpoint_matches(
        5, 0x82, 0x80, 5, 2, 0));
    CHECK(!zzusb_periodic_endpoint_matches(
        5, 0x82, 0x80, 6, 2, 0x80));
    CHECK(!zzusb_negotiation_complete(0, -1));
    CHECK(zzusb_negotiation_complete(0, 0));
    CHECK(zzusb_negotiation_complete(0, 1));
    CHECK(zzusb_negotiation_complete(1, -1));
    CHECK(zzusb_negotiation_complete(1, 0));
    CHECK(zzusb_sideband_publish_available(0, 0));
    CHECK(!zzusb_sideband_publish_available(1, 0));
    CHECK(!zzusb_sideband_publish_available(0, 1));
    CHECK(!zzusb_sideband_publish_available(1, 1));
    CHECK(zzusb_is_audio_rate_set_cur(0x22, 0x01, 0x0100, 3, 3));
    CHECK(!zzusb_is_audio_rate_set_cur(0x22, 0x01, 0x0200, 3, 3));
    CHECK(!zzusb_is_audio_rate_set_cur(0x22, 0x01, 0x0100, 4, 4));
    CHECK(!zzusb_is_audio_rate_set_cur(0x21, 0x01, 0x0100, 3, 3));
    CHECK(zzusb_audio_rate_matches(requested_rate, current_rate, 3));
    CHECK(!zzusb_audio_rate_matches(requested_rate, other_rate, 3));
    CHECK(!zzusb_audio_rate_matches(requested_rate, current_rate, 2));
    CHECK(zzusb_audio_rate_input_index(0x0002, 1, &input_index));
    CHECK(input_index == 0x0082);
    CHECK(!zzusb_audio_rate_input_index(0x0002, 0, &input_index));
    CHECK(!zzusb_audio_rate_input_index(0x0082, 1, &input_index));
    CHECK(!zzusb_audio_rate_input_index(0x0010, 1, &input_index));
    CHECK(!zzusb_audio_rate_input_index(0, 1, &input_index));

    puts("USB worker timer and audio-control policies satisfied");
    return EXIT_SUCCESS;
}
