/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "zzusb_iso.h"

#include <string.h>

static uint16_t read_be16(const uint8_t *bytes)
{
    return (uint16_t)(((uint16_t)bytes[0] << 8) | bytes[1]);
}

static uint32_t read_be32(const uint8_t *bytes)
{
    return ((uint32_t)bytes[0] << 24) |
           ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8) |
           bytes[3];
}

static void write_be16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)(value >> 8);
    bytes[1] = (uint8_t)value;
}

static void write_be32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)(value >> 24);
    bytes[1] = (uint8_t)(value >> 16);
    bytes[2] = (uint8_t)(value >> 8);
    bytes[3] = (uint8_t)value;
}

uint16_t zzusb_iso_payload_size(uint16_t encoded_max_packet)
{
    uint16_t base = encoded_max_packet & 0x07ffU;
    unsigned multiplier = 1U + ((encoded_max_packet >> 11) & 3U);

    if (!base || base > 1024U || multiplier > 3U ||
        (encoded_max_packet & 0xe000U) != 0)
        return 0;
    return (uint16_t)(base * multiplier);
}

unsigned zzusb_iso_plan_simple(uint32_t total_length,
                               uint16_t encoded_max_packet,
                               uint16_t *packet_lengths,
                               unsigned capacity)
{
    uint16_t payload = zzusb_iso_payload_size(encoded_max_packet);
    uint32_t remaining = total_length;
    unsigned count;

    if (!packet_lengths || !capacity || !payload ||
        total_length > ZZUSB_ISO_DATA_MAX)
        return 0;
    count = total_length ?
            (unsigned)((total_length + payload - 1U) / payload) : 1U;
    if (count > capacity || count > ZZUSB_ISO_MAX_PACKETS)
        return 0;
    for (unsigned index = 0; index < count; index++) {
        uint16_t length = remaining > payload ? payload :
                          (uint16_t)remaining;

        packet_lengths[index] = length;
        remaining -= length;
    }
    return count;
}

unsigned zzusb_iso_plan_realtime(uint16_t encoded_max_packet,
                                 uint16_t interval, int high_speed,
                                 uint16_t *packet_lengths,
                                 unsigned capacity,
                                 uint32_t *duration_microframes)
{
    uint16_t payload = zzusb_iso_payload_size(encoded_max_packet);
    uint32_t step;
    unsigned count;
    unsigned time_limit;

    if (!packet_lengths || !duration_microframes || !capacity || !payload ||
        !interval || payload > ZZUSB_ISO_DATA_MAX)
        return 0;
    if (high_speed) {
        if (interval > 16U)
            return 0;
        step = 1UL << (interval - 1U);
    } else {
        if (interval > 16U)
            return 0;
        step = (uint32_t)(1UL << (interval - 1U)) * 8U;
    }
    time_limit = step > 32768U ? 1U : (unsigned)(32768U / step);
    if (!time_limit)
        time_limit = 1U;
    count = step < 32U ? (unsigned)((32U + step - 1U) / step) : 1U;
    if (count > ZZUSB_ISO_DATA_MAX / payload)
        count = ZZUSB_ISO_DATA_MAX / payload;
    if (count > ZZUSB_ISO_MAX_PACKETS)
        count = ZZUSB_ISO_MAX_PACKETS;
    if (count > capacity)
        count = capacity;
    if (count > time_limit)
        count = time_limit;
    if (!count)
        return 0;
    for (unsigned index = 0; index < count; index++)
        packet_lengths[index] = payload;
    *duration_microframes = count * step;
    return count;
}

unsigned zzusb_iso_limit_packet_count(const uint16_t *packet_lengths,
                                      unsigned packet_count,
                                      unsigned wire_capacity)
{
    uint32_t total_data = 0;
    unsigned count;

    if (!packet_lengths || packet_count > ZZUSB_ISO_MAX_PACKETS)
        return 0;
    for (count = 0; count < packet_count; count++) {
        unsigned next = count + 1U;

        total_data += packet_lengths[count];
        if (ZZUSB_ISO_HEADER_SIZE + next * ZZUSB_ISO_PACKET_SIZE +
                total_data > wire_capacity)
            break;
    }
    return count;
}

unsigned zzusb_iso_build_queue(uint8_t *wire, unsigned capacity,
                               uint32_t batch_id, uint16_t flags,
                               uint16_t start_frame,
                               uint8_t start_microframe,
                               const uint16_t *packet_lengths,
                               unsigned packet_count,
                               const uint8_t *payload, int direction_in)
{
    uint32_t total_data = 0;
    unsigned metadata_size;
    unsigned wire_size;

    if (!wire || !batch_id || !packet_lengths || !packet_count ||
        packet_count > ZZUSB_ISO_MAX_PACKETS || start_microframe > 7U)
        return 0;
    for (unsigned index = 0; index < packet_count; index++) {
        if (total_data + packet_lengths[index] > ZZUSB_ISO_DATA_MAX)
            return 0;
        total_data += packet_lengths[index];
    }
    metadata_size = ZZUSB_ISO_HEADER_SIZE +
                    packet_count * ZZUSB_ISO_PACKET_SIZE;
    wire_size = metadata_size + (direction_in ? 0U : total_data);
    if (wire_size > capacity || (!direction_in && total_data && !payload))
        return 0;

    memset(wire, 0, wire_size);
    write_be32(wire + ZZUSB_ISO_HDR_OFF_MAGIC, ZZUSB_ISO_MAGIC);
    write_be16(wire + ZZUSB_ISO_HDR_OFF_VERSION, ZZUSB_ISO_VERSION);
    write_be16(wire + ZZUSB_ISO_HDR_OFF_FLAGS, flags);
    write_be32(wire + ZZUSB_ISO_HDR_OFF_BATCH_ID, batch_id);
    write_be16(wire + ZZUSB_ISO_HDR_OFF_START,
               (uint16_t)(start_frame & 0x07ffU));
    write_be16(wire + ZZUSB_ISO_HDR_OFF_COUNT, (uint16_t)packet_count);
    write_be32(wire + ZZUSB_ISO_HDR_OFF_DATA_LEN, total_data);
    wire[ZZUSB_ISO_HDR_OFF_START_UFRAME] = start_microframe;

    total_data = 0;
    for (unsigned index = 0; index < packet_count; index++) {
        uint8_t *entry = wire + ZZUSB_ISO_HEADER_SIZE +
                         index * ZZUSB_ISO_PACKET_SIZE;

        write_be16(entry + ZZUSB_ISO_PKT_OFF_REQUESTED,
                   packet_lengths[index]);
        write_be32(entry + ZZUSB_ISO_PKT_OFF_DATA, total_data);
        total_data += packet_lengths[index];
    }
    if (!direction_in && total_data)
        memcpy(wire + metadata_size, payload, total_data);
    return wire_size;
}

int zzusb_iso_parse_reap(const uint8_t *wire, unsigned wire_length,
                         int direction_in,
                         struct zzusb_iso_batch_result *batch,
                         struct zzusb_iso_packet_result *packets,
                         unsigned packet_capacity)
{
    uint16_t packet_count;
    uint32_t total_data;
    uint32_t expected_offset = 0;
    unsigned metadata_size;
    unsigned expected_size;

    if (!wire || !batch || !packets ||
        wire_length < ZZUSB_ISO_HEADER_SIZE ||
        read_be32(wire + ZZUSB_ISO_HDR_OFF_MAGIC) != ZZUSB_ISO_MAGIC ||
        read_be16(wire + ZZUSB_ISO_HDR_OFF_VERSION) != ZZUSB_ISO_VERSION)
        return 0;
    packet_count = read_be16(wire + ZZUSB_ISO_HDR_OFF_COUNT);
    total_data = read_be32(wire + ZZUSB_ISO_HDR_OFF_DATA_LEN);
    if (!packet_count || packet_count > ZZUSB_ISO_MAX_PACKETS ||
        packet_count > packet_capacity || total_data > ZZUSB_ISO_DATA_MAX ||
        wire[ZZUSB_ISO_HDR_OFF_START_UFRAME] > 7U)
        return 0;
    metadata_size = ZZUSB_ISO_HEADER_SIZE +
                    packet_count * ZZUSB_ISO_PACKET_SIZE;
    expected_size = metadata_size + (direction_in ? total_data : 0U);
    if (wire_length != expected_size)
        return 0;

    for (unsigned index = 0; index < packet_count; index++) {
        const uint8_t *entry = wire + ZZUSB_ISO_HEADER_SIZE +
                               index * ZZUSB_ISO_PACKET_SIZE;
        struct zzusb_iso_packet_result *packet = &packets[index];

        packet->requested = read_be16(entry + ZZUSB_ISO_PKT_OFF_REQUESTED);
        packet->actual = read_be16(entry + ZZUSB_ISO_PKT_OFF_ACTUAL);
        packet->status = (uint8_t)read_be16(
            entry + ZZUSB_ISO_PKT_OFF_STATUS);
        packet->frame = read_be16(entry + ZZUSB_ISO_PKT_OFF_FRAME) & 0x07ffU;
        packet->offset = read_be32(entry + ZZUSB_ISO_PKT_OFF_DATA);
        packet->microframe = entry[ZZUSB_ISO_PKT_OFF_UFRAME];
        if (packet->offset != expected_offset ||
            packet->actual > packet->requested ||
            packet->offset + packet->requested > total_data ||
            packet->microframe > 7U)
            return 0;
        expected_offset += packet->requested;
    }
    if (expected_offset != total_data)
        return 0;

    batch->batch_id = read_be32(wire + ZZUSB_ISO_HDR_OFF_BATCH_ID);
    batch->total_data = total_data;
    batch->start_frame = read_be16(wire + ZZUSB_ISO_HDR_OFF_START) & 0x07ffU;
    batch->start_microframe = wire[ZZUSB_ISO_HDR_OFF_START_UFRAME];
    batch->packet_count = (uint8_t)packet_count;
    batch->metadata_size = metadata_size;
    return batch->batch_id != 0;
}

int zzusb_iso_layout_matches(
    const struct zzusb_iso_batch_result *batch,
    const struct zzusb_iso_packet_result *packets,
    const uint16_t *packet_lengths, unsigned packet_count)
{
    if (!batch || !packets || !packet_lengths ||
        batch->packet_count != packet_count)
        return 0;
    for (unsigned index = 0; index < packet_count; index++)
        if (packets[index].requested != packet_lengths[index])
            return 0;
    return 1;
}

uint16_t zzusb_iso_status_flags(uint8_t packet_status)
{
    switch (packet_status) {
    case ZZUSB_ISO_PACKET_OK:
    case ZZUSB_ISO_PACKET_SHORT:
        return 0;
    case ZZUSB_ISO_PACKET_MISSED:
        return ZZUSB_RT_FLAG_MISSED | ZZUSB_RT_FLAG_PACKET_ERROR;
    case ZZUSB_ISO_PACKET_UNDERRUN:
        return ZZUSB_RT_FLAG_UNDERRUN | ZZUSB_RT_FLAG_PACKET_ERROR;
    case ZZUSB_ISO_PACKET_OVERRUN:
    case ZZUSB_ISO_PACKET_BABBLE:
        return ZZUSB_RT_FLAG_OVERRUN | ZZUSB_RT_FLAG_PACKET_ERROR;
    default:
        return ZZUSB_RT_FLAG_PACKET_ERROR;
    }
}

void zzusb_rt_init(struct zzusb_rt_lifecycle *lifecycle)
{
    if (lifecycle)
        memset(lifecycle, 0, sizeof(*lifecycle));
}

int zzusb_rt_add(struct zzusb_rt_lifecycle *lifecycle)
{
    if (!lifecycle || lifecycle->state != ZZUSB_RT_FREE)
        return 0;
    lifecycle->state = ZZUSB_RT_ADDED;
    lifecycle->next_batch_id = 1;
    return 1;
}

int zzusb_rt_start(struct zzusb_rt_lifecycle *lifecycle)
{
    if (!lifecycle ||
        (lifecycle->state != ZZUSB_RT_ADDED &&
         lifecycle->state != ZZUSB_RT_STOPPED))
        return 0;
    memset(lifecycle->queued_ids, 0, sizeof(lifecycle->queued_ids));
    lifecycle->in_flight = 0;
    lifecycle->state = ZZUSB_RT_RUNNING;
    return 1;
}

uint32_t zzusb_rt_queue(struct zzusb_rt_lifecycle *lifecycle)
{
    uint32_t batch_id;

    if (!lifecycle || lifecycle->state != ZZUSB_RT_RUNNING ||
        lifecycle->in_flight >= ZZUSB_ISO_PIPELINE_DEPTH)
        return 0;
    batch_id = lifecycle->next_batch_id++;
    if (!batch_id) {
        batch_id = 1;
        lifecycle->next_batch_id = 2;
    }
    lifecycle->queued_ids[lifecycle->in_flight++] = batch_id;
    return batch_id;
}

int zzusb_rt_complete(struct zzusb_rt_lifecycle *lifecycle,
                      uint32_t batch_id)
{
    unsigned index;

    if (!lifecycle || !batch_id ||
        lifecycle->state != ZZUSB_RT_RUNNING)
        return 0;
    for (index = 0; index < lifecycle->in_flight; index++)
        if (lifecycle->queued_ids[index] == batch_id)
            break;
    if (index == lifecycle->in_flight)
        return 0;
    for (; index + 1U < lifecycle->in_flight; index++)
        lifecycle->queued_ids[index] = lifecycle->queued_ids[index + 1U];
    lifecycle->queued_ids[--lifecycle->in_flight] = 0;
    return 1;
}

int zzusb_rt_begin_stop(struct zzusb_rt_lifecycle *lifecycle)
{
    if (!lifecycle || lifecycle->state != ZZUSB_RT_RUNNING)
        return 0;
    lifecycle->state = ZZUSB_RT_STOPPING;
    return 1;
}

int zzusb_rt_finish_stop(struct zzusb_rt_lifecycle *lifecycle)
{
    if (!lifecycle || lifecycle->state != ZZUSB_RT_STOPPING)
        return 0;
    memset(lifecycle->queued_ids, 0, sizeof(lifecycle->queued_ids));
    lifecycle->in_flight = 0;
    lifecycle->state = ZZUSB_RT_STOPPED;
    return 1;
}

int zzusb_rt_remove(struct zzusb_rt_lifecycle *lifecycle)
{
    if (!lifecycle ||
        (lifecycle->state != ZZUSB_RT_ADDED &&
         lifecycle->state != ZZUSB_RT_STOPPED))
        return 0;
    zzusb_rt_init(lifecycle);
    return 1;
}
