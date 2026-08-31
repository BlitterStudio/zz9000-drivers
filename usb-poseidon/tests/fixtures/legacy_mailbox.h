#ifndef ZZUSB_TEST_LEGACY_MAILBOX_H
#define ZZUSB_TEST_LEGACY_MAILBOX_H

#include <stdint.h>

#define LEGACY_ZZUSB_STATUS_OK       0x00u
#define LEGACY_ZZUSB_STATUS_PENDING  0x01u
#define LEGACY_ZZUSB_STATUS_TIMEOUT  0xfeu
#define LEGACY_ZZUSB_STATUS_NAK      0xfcu
#define LEGACY_ZZUSB_STATUS_CRC      0xfbu
#define LEGACY_ZZUSB_IDLE_LIMIT      10u

struct LegacyMailbox {
    uint16_t status;
    uint32_t actual_length;
    uint8_t data[64];
};

#endif
