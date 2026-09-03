/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "zzusb_iso.h"

static uint16_t get_be16(const uint8_t *bytes)
{
    return (uint16_t)(((uint16_t)bytes[0] << 8) | bytes[1]);
}

static uint32_t get_be32(const uint8_t *bytes)
{
    return ((uint32_t)bytes[0] << 24) |
           ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8) |
           bytes[3];
}

static void put_be16_test(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)(value >> 8);
    bytes[1] = (uint8_t)value;
}

static void test_packet_plans(void)
{
    uint16_t lengths[ZZUSB_ISO_MAX_PACKETS];
    uint32_t duration;
    unsigned count;
    assert(zzusb_iso_topology_supported(1, 0));
    assert(zzusb_iso_topology_supported(0, 1));
    assert(!zzusb_iso_topology_supported(0, 0));


    assert(zzusb_iso_payload_size(64) == 64);
    assert(zzusb_iso_payload_size(0x13ff) == 3069);
    assert(zzusb_iso_payload_size(0) == 0);
    assert(zzusb_iso_payload_size(0x1400) == 3072);
    assert(zzusb_iso_payload_size(0x0401) == 0);
    assert(zzusb_iso_payload_size(0x1c00) == 0);
    assert(zzusb_iso_payload_size(0x2400) == 0);

    count = zzusb_iso_plan_simple(15840, 1024, lengths,
                                  ZZUSB_ISO_MAX_PACKETS);
    assert(count == 16);
    for (unsigned index = 0; index < 15; index++)
        assert(lengths[index] == 1024);
    assert(lengths[15] == 480);
    assert(zzusb_iso_plan_simple(15841, 1024, lengths,
                                 ZZUSB_ISO_MAX_PACKETS) == 0);
    assert(zzusb_iso_plan_simple(0, 64, lengths,
                                 ZZUSB_ISO_MAX_PACKETS) == 1);
    assert(lengths[0] == 0);

    count = zzusb_iso_plan_realtime(192, 8, 1, lengths,
                                    ZZUSB_ISO_MAX_PACKETS, &duration);
    assert(count == 4);
    assert(duration == 32);
    count = zzusb_iso_plan_realtime(1024, 1, 1, lengths,
                                    ZZUSB_ISO_MAX_PACKETS, &duration);
    assert(count == 15);
    count = zzusb_iso_limit_packet_count(
        lengths, count, 3968);
    assert(count == 3);
    assert(duration == 15);
    count = zzusb_iso_plan_realtime(192, 1, 0, lengths,
                                    ZZUSB_ISO_MAX_PACKETS, &duration);
    assert(count == 4);
    assert(duration == 32);
    count = zzusb_iso_plan_realtime(192, 4, 0, lengths,
                                    ZZUSB_ISO_MAX_PACKETS, &duration);
    assert(count == 1);
    assert(duration == 32);
    count = zzusb_iso_plan_realtime(192, 32768, 0, lengths,
                                    ZZUSB_ISO_MAX_PACKETS, &duration);
    assert(count == 1);
    assert(duration == 262144);
    assert(zzusb_iso_plan_realtime(64, 3, 1, lengths,
                                    ZZUSB_ISO_MAX_PACKETS, &duration) == 0);
    assert(zzusb_iso_plan_realtime(64, 3, 0, lengths,
                                    ZZUSB_ISO_MAX_PACKETS, &duration) == 0);
}

static void test_wire_round_trip(void)
{
    uint8_t wire[256];
    const uint8_t payload[6] = { 1, 2, 3, 4, 5, 6 };
    const uint16_t lengths[3] = { 4, 0, 2 };
    struct zzusb_iso_batch_result batch;
    struct zzusb_iso_packet_result packets[3];
    unsigned metadata_size = ZZUSB_ISO_HEADER_SIZE +
                             3U * ZZUSB_ISO_PACKET_SIZE;
    unsigned wire_size;

    wire_size = zzusb_iso_build_queue(wire, sizeof(wire), 0x10203040,
                                      0, 2047, 7, lengths, 3,
                                      payload, 0);
    assert(wire_size == metadata_size + sizeof(payload));
    assert(get_be32(wire + ZZUSB_ISO_HDR_OFF_MAGIC) == ZZUSB_ISO_MAGIC);
    assert(get_be32(wire + ZZUSB_ISO_HDR_OFF_BATCH_ID) == 0x10203040);
    assert(get_be16(wire + ZZUSB_ISO_HDR_OFF_START) == 2047);
    assert(wire[ZZUSB_ISO_HDR_OFF_START_UFRAME] == 7);
    assert(get_be32(wire + ZZUSB_ISO_HEADER_SIZE +
                    2U * ZZUSB_ISO_PACKET_SIZE +
                    ZZUSB_ISO_PKT_OFF_DATA) == 4);
    assert(memcmp(wire + metadata_size, payload, sizeof(payload)) == 0);

    wire_size = zzusb_iso_build_queue(wire, sizeof(wire), 7,
                                      ZZUSB_ISO_FLAG_ASAP, 0, 0,
                                      lengths, 3, NULL, 1);
    assert(wire_size == metadata_size);
    for (unsigned index = 0; index < 3; index++) {
        uint8_t *entry = wire + ZZUSB_ISO_HEADER_SIZE +
                         index * ZZUSB_ISO_PACKET_SIZE;
        put_be16_test(entry + ZZUSB_ISO_PKT_OFF_ACTUAL, lengths[index]);
        put_be16_test(entry + ZZUSB_ISO_PKT_OFF_STATUS,
                      index == 1 ? ZZUSB_ISO_PACKET_SHORT :
                                   ZZUSB_ISO_PACKET_OK);
        put_be16_test(entry + ZZUSB_ISO_PKT_OFF_FRAME,
                      (uint16_t)((2047U + index) & 0x07ffU));
        entry[ZZUSB_ISO_PKT_OFF_UFRAME] = (uint8_t)index;
    }
    memcpy(wire + metadata_size, payload, sizeof(payload));
    assert(zzusb_iso_parse_reap(wire, metadata_size + sizeof(payload), 1,
                                &batch, packets, 3));
    assert(batch.batch_id == 7);
    assert(batch.packet_count == 3);
    assert(batch.total_data == 6);
    assert(packets[0].frame == 2047);
    assert(packets[1].frame == 0);
    assert(packets[2].frame == 1);
    assert(packets[2].offset == 4);
    assert(packets[1].status == ZZUSB_ISO_PACKET_SHORT);
    assert(zzusb_iso_layout_matches(
        &batch, packets, lengths, 3));
    {
        const uint16_t wrong_lengths[3] = {2, 1, 3};

        assert(!zzusb_iso_layout_matches(
            &batch, packets, wrong_lengths, 3));
        assert(!zzusb_iso_layout_matches(
            &batch, packets, lengths, 2));
    }

    put_be16_test(wire + ZZUSB_ISO_HEADER_SIZE +
                  ZZUSB_ISO_PKT_OFF_STATUS, 0x0100);
    assert(!zzusb_iso_parse_reap(wire,
                                 metadata_size + sizeof(payload), 1,
                                 &batch, packets, 3));
    put_be16_test(wire + ZZUSB_ISO_HEADER_SIZE +
                  ZZUSB_ISO_PKT_OFF_STATUS, ZZUSB_ISO_PACKET_OK);

    put_be16_test(wire + ZZUSB_ISO_HEADER_SIZE +
                  ZZUSB_ISO_PKT_OFF_ACTUAL, 5);
    assert(!zzusb_iso_parse_reap(wire,
                                 metadata_size + sizeof(payload), 1,
                                 &batch, packets, 3));
}

static void test_realtime_lifecycle(void)
{
    struct zzusb_rt_lifecycle lifecycle;
    uint32_t ids[ZZUSB_ISO_PIPELINE_DEPTH];

    zzusb_rt_init(&lifecycle);
    assert(zzusb_rt_add(&lifecycle));
    assert(!zzusb_rt_add(&lifecycle));
    assert(!zzusb_rt_begin_stop(&lifecycle));
    assert(zzusb_rt_start(&lifecycle));
    assert(!zzusb_rt_start(&lifecycle));
    for (unsigned index = 0; index < ZZUSB_ISO_PIPELINE_DEPTH; index++) {
        ids[index] = zzusb_rt_queue(&lifecycle);
        assert(ids[index] != 0);
    }
    assert(zzusb_rt_queue(&lifecycle) == 0);
    assert(zzusb_rt_complete(&lifecycle, ids[1]));
    assert(!zzusb_rt_complete(&lifecycle, ids[1]));
    assert(lifecycle.in_flight == ZZUSB_ISO_PIPELINE_DEPTH - 1U);
    assert(zzusb_rt_begin_stop(&lifecycle));
    assert(!zzusb_rt_begin_stop(&lifecycle));
    assert(zzusb_rt_queue(&lifecycle) == 0);
    assert(!zzusb_rt_complete(&lifecycle, ids[0]));
    assert(zzusb_rt_finish_stop(&lifecycle));
    assert(lifecycle.in_flight == 0);
    assert(!zzusb_rt_begin_stop(&lifecycle));
    assert(zzusb_rt_start(&lifecycle));
    assert(zzusb_rt_begin_stop(&lifecycle));
    assert(zzusb_rt_finish_stop(&lifecycle));
    assert(zzusb_rt_remove(&lifecycle));
    assert(lifecycle.state == ZZUSB_RT_FREE);
    assert(!zzusb_rt_remove(&lifecycle));
}

static void test_deferred_realtime_remove(void)
{
    struct zzusb_rt_lifecycle lifecycle;
    uint32_t batch;

    zzusb_rt_init(&lifecycle);
    assert(zzusb_rt_add(&lifecycle));
    assert(zzusb_rt_request_remove(&lifecycle));
    assert(lifecycle.state == ZZUSB_RT_FREE);

    assert(zzusb_rt_add(&lifecycle));
    assert(zzusb_rt_start(&lifecycle));
    batch = zzusb_rt_queue(&lifecycle);
    assert(batch != 0);
    assert(zzusb_rt_request_remove(&lifecycle));
    assert(lifecycle.state == ZZUSB_RT_STOPPING);
    assert(lifecycle.remove_pending);
    assert(lifecycle.in_flight == 1);
    assert(zzusb_rt_finish_stop(&lifecycle));
    assert(lifecycle.state == ZZUSB_RT_FREE);
    assert(lifecycle.in_flight == 0);

    assert(zzusb_rt_add(&lifecycle));
    assert(zzusb_rt_start(&lifecycle));
    assert(zzusb_rt_begin_stop(&lifecycle));
    assert(zzusb_rt_request_remove(&lifecycle));
    assert(lifecycle.remove_pending);
    assert(zzusb_rt_finish_stop(&lifecycle));
    assert(lifecycle.state == ZZUSB_RT_FREE);
}

static void test_status_flags(void)
{
    assert(zzusb_iso_status_flags(ZZUSB_ISO_PACKET_OK) == 0);
    assert(zzusb_iso_status_flags(ZZUSB_ISO_PACKET_SHORT) == 0);
    assert((zzusb_iso_status_flags(ZZUSB_ISO_PACKET_MISSED) &
            ZZUSB_RT_FLAG_MISSED) != 0);
    assert((zzusb_iso_status_flags(ZZUSB_ISO_PACKET_UNDERRUN) &
            ZZUSB_RT_FLAG_UNDERRUN) != 0);
    assert((zzusb_iso_status_flags(ZZUSB_ISO_PACKET_OVERRUN) &
            ZZUSB_RT_FLAG_OVERRUN) != 0);
}

int main(void)
{
    test_packet_plans();
    test_wire_round_trip();
    test_realtime_lifecycle();
    test_deferred_realtime_remove();
    test_status_flags();
    puts("zzusb_iso_test: ok");
    return 0;
}
