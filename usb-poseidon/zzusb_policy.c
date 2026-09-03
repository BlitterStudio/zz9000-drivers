/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "zzusb_policy.h"

#include <string.h>

void zzusb_worker_timer_init(struct zzusb_worker_timer *timer)
{
    if (timer) {
        timer->delay_us = 0;
        timer->pending = 0;
    }
}

int zzusb_worker_timer_arm(struct zzusb_worker_timer *timer,
                           uint32_t delay_us)
{
    if (!timer || timer->pending || delay_us == 0)
        return 0;
    timer->delay_us = delay_us;
    timer->pending = 1;
    return 1;
}

uint32_t zzusb_worker_timer_expire(struct zzusb_worker_timer *timer,
                                   int timer_signaled)
{
    uint32_t elapsed_us;

    if (!timer || !timer->pending || !timer_signaled)
        return 0;
    elapsed_us = timer->delay_us;
    timer->delay_us = 0;
    timer->pending = 0;
    return elapsed_us;
}

void zzusb_worker_timer_cancel(struct zzusb_worker_timer *timer)
{
    if (timer) {
        timer->delay_us = 0;
        timer->pending = 0;
    }
}

int zzusb_completion_needs_reply(int quick)
{
    return !quick;
}

int zzusb_bulk_resume_window(uint32_t requested, uint32_t completed,
                             uint32_t *offset, uint32_t *remaining)
{
    if (!offset || !remaining || completed > requested)
        return 0;
    *offset = completed;
    *remaining = requested - completed;
    return 1;
}

int zzusb_mailbox_timer_available(int worker_request, int worker_mask,
                                  int foreground_request,
                                  int foreground_mask,
                                  int foreground_owned)
{
    return (worker_request && worker_mask) ||
           (foreground_request && foreground_mask && foreground_owned);
}
int zzusb_sync_command_timer_available(int foreground_opened,
                                       int foreground_owned)
{
    return foreground_opened && foreground_owned;
}


int zzusb_is_audio_rate_set_cur(uint8_t request_type, uint8_t request,
                                uint16_t value, uint16_t length,
                                uint32_t data_length)
{
    return request_type == 0x22 && request == 0x01 &&
           (value >> 8) == 0x01 && length == 3 && data_length == 3;
}

int zzusb_audio_rate_matches(const void *requested, const void *current,
                             uint32_t length)
{
    return requested && current && length == 3 &&
           memcmp(requested, current, 3) == 0;
}
