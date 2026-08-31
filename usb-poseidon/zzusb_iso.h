/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef ZZUSB_ISO_H
#define ZZUSB_ISO_H

#include <stddef.h>
#include <stdint.h>

#define ZZUSB_ISO_MAGIC              0x5a49534fUL
#define ZZUSB_ISO_VERSION            1U
#define ZZUSB_ISO_HEADER_SIZE        32U
#define ZZUSB_ISO_PACKET_SIZE        16U
#define ZZUSB_ISO_MAX_PACKETS        32U
#define ZZUSB_ISO_MAX_BATCHES        8U
#define ZZUSB_ISO_DATA_MAX           15840U
#define ZZUSB_ISO_PIPELINE_DEPTH     4U

#define ZZUSB_ISO_FLAG_ASAP          0x0001U

#define ZZUSB_ISO_HDR_OFF_MAGIC       0U
#define ZZUSB_ISO_HDR_OFF_VERSION     4U
#define ZZUSB_ISO_HDR_OFF_FLAGS       6U
#define ZZUSB_ISO_HDR_OFF_BATCH_ID    8U
#define ZZUSB_ISO_HDR_OFF_START      12U
#define ZZUSB_ISO_HDR_OFF_COUNT      14U
#define ZZUSB_ISO_HDR_OFF_DATA_LEN   16U
#define ZZUSB_ISO_HDR_OFF_START_UFRAME 20U

#define ZZUSB_ISO_PKT_OFF_REQUESTED   0U
#define ZZUSB_ISO_PKT_OFF_ACTUAL      2U
#define ZZUSB_ISO_PKT_OFF_STATUS      4U
#define ZZUSB_ISO_PKT_OFF_FRAME       6U
#define ZZUSB_ISO_PKT_OFF_DATA        8U
#define ZZUSB_ISO_PKT_OFF_UFRAME     12U

#define ZZUSB_ISO_PACKET_OK         0U
#define ZZUSB_ISO_PACKET_PENDING    1U
#define ZZUSB_ISO_PACKET_SHORT      2U
#define ZZUSB_ISO_PACKET_MISSED     3U
#define ZZUSB_ISO_PACKET_UNDERRUN   4U
#define ZZUSB_ISO_PACKET_OVERRUN    5U
#define ZZUSB_ISO_PACKET_CANCELLED  6U
#define ZZUSB_ISO_PACKET_OFFLINE    7U
#define ZZUSB_ISO_PACKET_XACT       8U
#define ZZUSB_ISO_PACKET_BABBLE     9U

#define ZZUSB_RT_FLAG_MISSED        0x1000U
#define ZZUSB_RT_FLAG_OVERRUN       0x2000U
#define ZZUSB_RT_FLAG_UNDERRUN      0x4000U
#define ZZUSB_RT_FLAG_PACKET_ERROR  0x8000U

struct zzusb_iso_packet_result {
    uint32_t offset;
    uint16_t requested;
    uint16_t actual;
    uint16_t frame;
    uint8_t microframe;
    uint8_t status;
};

struct zzusb_iso_batch_result {
    uint32_t batch_id;
    uint32_t total_data;
    uint16_t start_frame;
    uint8_t start_microframe;
    uint8_t packet_count;
    unsigned metadata_size;
};

enum zzusb_rt_state {
    ZZUSB_RT_FREE = 0,
    ZZUSB_RT_ADDED,
    ZZUSB_RT_RUNNING,
    ZZUSB_RT_STOPPING,
    ZZUSB_RT_STOPPED
};

struct zzusb_rt_lifecycle {
    uint32_t queued_ids[ZZUSB_ISO_PIPELINE_DEPTH];
    uint32_t next_batch_id;
    uint8_t state;
    uint8_t in_flight;
};

uint16_t zzusb_iso_payload_size(uint16_t encoded_max_packet);
unsigned zzusb_iso_plan_simple(uint32_t total_length,
                               uint16_t encoded_max_packet,
                               uint16_t *packet_lengths,
                               unsigned capacity);
unsigned zzusb_iso_plan_realtime(uint16_t encoded_max_packet,
                                 uint16_t interval, int high_speed,
                                 uint16_t *packet_lengths,
                                 unsigned capacity,
                                 uint16_t *duration_microframes);
unsigned zzusb_iso_limit_packet_count(const uint16_t *packet_lengths,
                                      unsigned packet_count,
                                      unsigned wire_capacity);
unsigned zzusb_iso_build_queue(uint8_t *wire, unsigned capacity,
                               uint32_t batch_id, uint16_t flags,
                               uint16_t start_frame,
                               uint8_t start_microframe,
                               const uint16_t *packet_lengths,
                               unsigned packet_count,
                               const uint8_t *payload, int direction_in);
int zzusb_iso_parse_reap(const uint8_t *wire, unsigned wire_length,
                         int direction_in,
                         struct zzusb_iso_batch_result *batch,
                         struct zzusb_iso_packet_result *packets,
                         unsigned packet_capacity);
uint16_t zzusb_iso_status_flags(uint8_t packet_status);

void zzusb_rt_init(struct zzusb_rt_lifecycle *lifecycle);
int zzusb_rt_add(struct zzusb_rt_lifecycle *lifecycle);
int zzusb_rt_start(struct zzusb_rt_lifecycle *lifecycle);
uint32_t zzusb_rt_queue(struct zzusb_rt_lifecycle *lifecycle);
int zzusb_rt_complete(struct zzusb_rt_lifecycle *lifecycle,
                      uint32_t batch_id);
int zzusb_rt_begin_stop(struct zzusb_rt_lifecycle *lifecycle);
int zzusb_rt_finish_stop(struct zzusb_rt_lifecycle *lifecycle);
int zzusb_rt_remove(struct zzusb_rt_lifecycle *lifecycle);

#endif
