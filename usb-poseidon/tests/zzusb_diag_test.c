#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "zzusb_engine.h"

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "%s:%d: check failed: %s\n", \
            __FILE__, __LINE__, #expr); return EXIT_FAILURE; \
} } while (0)

int main(void)
{
    struct zzusb_driver_diag_snapshot snapshot;
    struct zzusb_engine_request request;
    uint32_t first_generation;

    zzusb_engine_diag_reset();
    for (uint32_t i = 1; i <= 200u; i++) {
        zzusb_engine_diag_count(ZZUSB_DRIVER_COUNT_REQUEST);
        zzusb_engine_diag_record((uint16_t)((i % 12u) + 1u),
                                 (uint16_t)(i & 0xffu), i, 11u,
                                 (uint16_t)(i & 0x7fu),
                                 (uint8_t)(i & 0x0fu),
                                 (uint8_t)(i & 1u),
                                 (uint16_t)(i >> 4),
                                 (uint16_t)(i & 7u),
                                 i * 3u, i * 5u);
    }
    CHECK(zzusb_engine_diag_high_water(3u));
    CHECK(zzusb_engine_diag_high_water(12u));
    CHECK(!zzusb_engine_diag_high_water(6u));

    CHECK(zzusb_engine_diag_snapshot(&snapshot, 0x31fu, 11u, 4u));
    CHECK(snapshot.magic == ZZUSB_DRIVER_DIAG_MAGIC);
    CHECK(snapshot.version == ZZUSB_DRIVER_DIAG_VERSION);
    CHECK(!(snapshot.generation & 1u));
    first_generation = snapshot.generation;
    CHECK(snapshot.event_count == ZZUSB_DRIVER_DIAG_EVENT_COUNT);
    CHECK(snapshot.next_sequence == 200u);
    CHECK(snapshot.lost_events == 136u);
    CHECK(snapshot.capabilities == 0x31fu);
    CHECK(snapshot.controller_epoch == 11u);
    CHECK(snapshot.counters[ZZUSB_DRIVER_COUNT_REQUEST] == 200u);
    CHECK(snapshot.counters[ZZUSB_DRIVER_COUNT_QUEUE_HIGH_WATER] == 12u);
    CHECK(snapshot.events[0].sequence == 137u);
    CHECK(snapshot.events[63].sequence == 200u);

    zzusb_engine_diag_reset();
    zzusb_engine_init(&request);
    CHECK(zzusb_engine_queue(&request));
    CHECK(zzusb_engine_dispatch(&request));
    CHECK(zzusb_engine_begin(&request, 77u, 12u));
    CHECK(zzusb_engine_timeout(&request));
    CHECK(!zzusb_engine_complete(&request, 76u, 12u,
                                 ZZUSB_ENGINE_STATUS_OK));
    CHECK(zzusb_engine_complete(&request, 77u, 12u,
                                ZZUSB_ENGINE_STATUS_TIMEOUT));
    CHECK(zzusb_engine_diag_snapshot(&snapshot, 0x31fu, 12u, 4u));
    CHECK(!(snapshot.generation & 1u));
    CHECK(snapshot.generation > first_generation);
    CHECK(snapshot.counters[ZZUSB_DRIVER_COUNT_REQUEST] == 1u);
    CHECK(snapshot.counters[ZZUSB_DRIVER_COUNT_TIMEOUT] == 1u);
    CHECK(snapshot.counters[ZZUSB_DRIVER_COUNT_LATE_COMPLETION] == 1u);
    CHECK(snapshot.counters[ZZUSB_DRIVER_COUNT_COMPLETION] == 1u);

    puts("USB driver diagnostic ring contract satisfied");
    return EXIT_SUCCESS;
}
