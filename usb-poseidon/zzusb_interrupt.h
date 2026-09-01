/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef ZZUSB_INTERRUPT_H
#define ZZUSB_INTERRUPT_H

#include <stdint.h>

#define ZZUSB_EVENT_INTERRUPT_BIT (1U << 4)
#define ZZUSB_EVENT_ACK_VALUE     ((1U << 3) | (1U << 8))

static inline int zzusb_event_interrupt_pending(uint16_t status)
{
    return (status & ZZUSB_EVENT_INTERRUPT_BIT) != 0;
}

enum zzusb_interrupt_action {
    ZZUSB_INTERRUPT_KEEP_PENDING = 0,
    ZZUSB_INTERRUPT_COMPLETE,
    ZZUSB_INTERRUPT_FAIL
};

static inline enum zzusb_interrupt_action zzusb_interrupt_classify(
    uint16_t status, uint32_t actual, int is_in, int zero_report)
{
    if (status == 0xfcU)
        return ZZUSB_INTERRUPT_KEEP_PENDING;
    if (status != 0x00U)
        return ZZUSB_INTERRUPT_FAIL;
    if (is_in && (actual == 0 || zero_report))
        return ZZUSB_INTERRUPT_KEEP_PENDING;
    return ZZUSB_INTERRUPT_COMPLETE;
}

static inline int zzusb_interrupt_rearm_on_replace(
    int armed, int is_in, int parameters_changed)
{
    return armed && (!is_in || parameters_changed);
}

#endif
