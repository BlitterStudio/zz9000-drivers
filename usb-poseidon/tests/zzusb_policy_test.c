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
    CHECK(zzusb_is_audio_rate_set_cur(0x22, 0x01, 0x0100, 3, 3));
    CHECK(!zzusb_is_audio_rate_set_cur(0x22, 0x01, 0x0200, 3, 3));
    CHECK(!zzusb_is_audio_rate_set_cur(0x22, 0x01, 0x0100, 4, 4));
    CHECK(!zzusb_is_audio_rate_set_cur(0x21, 0x01, 0x0100, 3, 3));
    CHECK(zzusb_audio_rate_matches(requested_rate, current_rate, 3));
    CHECK(!zzusb_audio_rate_matches(requested_rate, other_rate, 3));
    CHECK(!zzusb_audio_rate_matches(requested_rate, current_rate, 2));

    puts("USB worker timer and audio-control policies satisfied");
    return EXIT_SUCCESS;
}
