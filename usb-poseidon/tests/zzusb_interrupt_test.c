#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "../zzusb_interrupt.h"

int main(void)
{
    unsigned replies = 0;
    enum zzusb_interrupt_action action;

    assert(!zzusb_event_interrupt_pending(0));
    assert(!zzusb_event_interrupt_pending(1U | 2U | 4U));
    assert(zzusb_event_interrupt_pending(ZZUSB_EVENT_INTERRUPT_BIT));
    assert(ZZUSB_EVENT_ACK_VALUE == ((1U << 3) | (1U << 8)));
    assert(!zzusb_interrupt_rearm_on_replace(0, 0));
    assert(!zzusb_interrupt_rearm_on_replace(1, 1));
    assert(zzusb_interrupt_rearm_on_replace(1, 0));

    action = zzusb_interrupt_classify(0xfcU, 0, 1, 0);
    assert(action == ZZUSB_INTERRUPT_KEEP_PENDING);
    action = zzusb_interrupt_classify(0x00U, 0, 1, 0);
    assert(action == ZZUSB_INTERRUPT_KEEP_PENDING);
    action = zzusb_interrupt_classify(0x00U, 2, 1, 1);
    assert(action == ZZUSB_INTERRUPT_KEEP_PENDING);

    action = zzusb_interrupt_classify(0x00U, 8, 1, 0);
    if (action == ZZUSB_INTERRUPT_COMPLETE)
        replies++;
    assert(replies == 1);

    action = zzusb_interrupt_classify(0xfdU, 0, 1, 0);
    if (action == ZZUSB_INTERRUPT_FAIL)
        replies++;
    assert(replies == 2);

    puts("USB interrupt event, no-data, and exact completion contract satisfied");
    return 0;
}
