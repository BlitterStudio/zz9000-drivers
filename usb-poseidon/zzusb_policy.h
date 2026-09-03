/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef ZZUSB_POLICY_H
#define ZZUSB_POLICY_H

#include <stdint.h>

struct zzusb_worker_timer {
    uint32_t delay_us;
    uint8_t pending;
};

void zzusb_worker_timer_init(struct zzusb_worker_timer *timer);
int zzusb_worker_timer_arm(struct zzusb_worker_timer *timer,
                           uint32_t delay_us);
uint32_t zzusb_worker_timer_expire(struct zzusb_worker_timer *timer,
                                   int timer_signaled);
void zzusb_worker_timer_cancel(struct zzusb_worker_timer *timer);
uint32_t zzusb_worker_timer_elapsed(uint64_t started_us, uint64_t now_us,
                                    uint32_t delay_us);
int zzusb_completion_needs_reply(int quick);
int zzusb_bulk_resume_window(uint32_t requested, uint32_t completed,
                             uint32_t *offset, uint32_t *remaining);
int zzusb_mailbox_timer_available(int worker_request, int worker_mask,
                                  int foreground_request,
                                  int foreground_mask,
                                  int foreground_owned);
int zzusb_sync_command_timer_available(int foreground_opened,
                                       int foreground_owned);
int zzusb_quickio_available(int poll_task, int worker_request,
                            int worker_mask);
int zzusb_negotiation_complete(int complete, int attempt_result);
int zzusb_sideband_publish_available(int quarantined,
                                     int maintenance_quarantined);

int zzusb_is_audio_rate_set_cur(uint8_t request_type, uint8_t request,
                                uint16_t value, uint16_t length,
                                uint32_t data_length);
int zzusb_audio_rate_matches(const void *requested, const void *current,
                             uint32_t length);
int zzusb_audio_rate_input_index(uint16_t index, int known_input,
                                 uint16_t *input_index);

#endif
