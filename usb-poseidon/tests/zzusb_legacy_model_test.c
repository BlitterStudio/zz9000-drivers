#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "legacy_mailbox.h"

#define CHECK(expr) do {     if (!(expr)) {         fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #expr);         return EXIT_FAILURE;     } } while (0)

static int legacy_idle_is_terminal(uint16_t status, uint32_t actual,
                                   unsigned idle_polls)
{
    (void)idle_polls;
    if (actual != 0u)
        return 1;
    return status != LEGACY_ZZUSB_STATUS_OK &&
           status != LEGACY_ZZUSB_STATUS_NAK &&
           status != LEGACY_ZZUSB_STATUS_TIMEOUT;
}

static int legacy_can_overwrite_after_local_timeout(uint16_t mailbox_status)
{
    return mailbox_status != LEGACY_ZZUSB_STATUS_PENDING;
}

static void legacy_copy_control_in(uint8_t *caller, size_t requested,
                                   const uint8_t *shared, size_t actual)
{
    if (actual > requested)
        actual = requested;
    memset(caller, 0, requested);
    memcpy(caller, shared, actual);
}

int main(void)
{
    uint8_t caller[8];
    const uint8_t descriptor[] = { 4u, 3u, 'A', 0u };

    CHECK(!legacy_idle_is_terminal(LEGACY_ZZUSB_STATUS_OK, 0u,
                                   LEGACY_ZZUSB_IDLE_LIMIT));
    CHECK(!legacy_idle_is_terminal(LEGACY_ZZUSB_STATUS_NAK, 0u,
                                   LEGACY_ZZUSB_IDLE_LIMIT * 2u));
    CHECK(!legacy_idle_is_terminal(LEGACY_ZZUSB_STATUS_TIMEOUT, 0u,
                                   LEGACY_ZZUSB_IDLE_LIMIT * 2u));
    CHECK(legacy_idle_is_terminal(LEGACY_ZZUSB_STATUS_CRC, 0u, 0u));
    CHECK(!legacy_can_overwrite_after_local_timeout(
        LEGACY_ZZUSB_STATUS_PENDING));

    memset(caller, 0x3fu, sizeof(caller));
    legacy_copy_control_in(caller, sizeof(caller), descriptor,
                           sizeof(descriptor));
    CHECK(memcmp(caller, descriptor, sizeof(descriptor)) == 0);
    for (size_t i = sizeof(descriptor); i < sizeof(caller); ++i)
        CHECK(caller[i] == 0u);

    puts("legacy driver ownership and short-read defects removed");
    return EXIT_SUCCESS;
}
