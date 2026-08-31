/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef ZZUSB_ENGINE_H
#define ZZUSB_ENGINE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum zzusb_request_state {
    ZZUSB_REQ_FREE = 0,
    ZZUSB_REQ_QUEUED,
    ZZUSB_REQ_DISPATCHING,
    ZZUSB_REQ_IN_FLIGHT,
    ZZUSB_REQ_RETIRING,
    ZZUSB_REQ_TERMINAL
};

enum zzusb_terminal_result {
    ZZUSB_RESULT_NONE = 0,
    ZZUSB_RESULT_OK,
    ZZUSB_RESULT_IO_ERROR,
    ZZUSB_RESULT_ABORTED,
    ZZUSB_RESULT_TIMEOUT
};

enum zzusb_error_class {
    ZZUSB_ERROR_NONE = 0,
    ZZUSB_ERROR_TIMEOUT,
    ZZUSB_ERROR_NAK,
    ZZUSB_ERROR_STALL,
    ZZUSB_ERROR_OFFLINE,
    ZZUSB_ERROR_CRC,
    ZZUSB_ERROR_BABBLE,
    ZZUSB_ERROR_OVERFLOW,
    ZZUSB_ERROR_UNDERFLOW,
    ZZUSB_ERROR_CANCELLED,
    ZZUSB_ERROR_HOST,
    ZZUSB_ERROR_BAD_PARAMETER,
    ZZUSB_ERROR_NO_MEMORY,
    ZZUSB_ERROR_UNSUPPORTED
};

enum zzusb_transport_status {
    ZZUSB_ENGINE_STATUS_OK          = 0x00,
    ZZUSB_ENGINE_STATUS_PENDING     = 0x01,
    ZZUSB_ENGINE_STATUS_ERROR       = 0xff,
    ZZUSB_ENGINE_STATUS_TIMEOUT     = 0xfe,
    ZZUSB_ENGINE_STATUS_STALL       = 0xfd,
    ZZUSB_ENGINE_STATUS_NAK         = 0xfc,
    ZZUSB_ENGINE_STATUS_CRC         = 0xfb,
    ZZUSB_ENGINE_STATUS_BABBLE      = 0xfa,
    ZZUSB_ENGINE_STATUS_OVERRUN     = 0xf9,
    ZZUSB_ENGINE_STATUS_UNDERRUN    = 0xf8,
    ZZUSB_ENGINE_STATUS_OFFLINE     = 0xf7,
    ZZUSB_ENGINE_STATUS_BADPARAM    = 0xf6,
    ZZUSB_ENGINE_STATUS_UNSUPPORTED = 0xf5,
    ZZUSB_ENGINE_STATUS_STALE       = 0xf4,
    ZZUSB_ENGINE_STATUS_CANCELLED   = 0xf3,
    ZZUSB_ENGINE_STATUS_HOSTERROR   = 0xf2,
    ZZUSB_ENGINE_STATUS_BUSY        = 0xf1,
    ZZUSB_ENGINE_STATUS_NOMEM       = 0xf0
};

#define ZZUSB_DRIVER_DIAG_MAGIC         0x5a554444UL
#define ZZUSB_DRIVER_DIAG_VERSION       1U
#define ZZUSB_DRIVER_DIAG_EVENT_COUNT   64U
#define ZZUSB_DRIVER_DIAG_COUNTER_COUNT 16U

enum zzusb_driver_diag_counter {
    ZZUSB_DRIVER_COUNT_REQUEST = 0,
    ZZUSB_DRIVER_COUNT_COMPLETION,
    ZZUSB_DRIVER_COUNT_TIMEOUT,
    ZZUSB_DRIVER_COUNT_LATE_COMPLETION,
    ZZUSB_DRIVER_COUNT_CANCELLATION,
    ZZUSB_DRIVER_COUNT_RESET,
    ZZUSB_DRIVER_COUNT_HOST_ERROR,
    ZZUSB_DRIVER_COUNT_RECOVERY,
    ZZUSB_DRIVER_COUNT_STALE,
    ZZUSB_DRIVER_COUNT_QUEUE_HIGH_WATER,
    ZZUSB_DRIVER_COUNT_INTERRUPT_ARM,
    ZZUSB_DRIVER_COUNT_INTERRUPT_REAP,
    ZZUSB_DRIVER_COUNT_ISO_QUEUE,
    ZZUSB_DRIVER_COUNT_ISO_REAP
};

enum zzusb_driver_diag_event_type {
    ZZUSB_DRIVER_EVENT_REQUEST = 1,
    ZZUSB_DRIVER_EVENT_COMPLETION,
    ZZUSB_DRIVER_EVENT_TIMEOUT,
    ZZUSB_DRIVER_EVENT_LATE_COMPLETION,
    ZZUSB_DRIVER_EVENT_CANCELLATION,
    ZZUSB_DRIVER_EVENT_RESET,
    ZZUSB_DRIVER_EVENT_HOST_ERROR,
    ZZUSB_DRIVER_EVENT_RECOVERY,
    ZZUSB_DRIVER_EVENT_STALE,
    ZZUSB_DRIVER_EVENT_HIGH_WATER,
    ZZUSB_DRIVER_EVENT_INTERRUPT,
    ZZUSB_DRIVER_EVENT_ISO,
    ZZUSB_DRIVER_EVENT_PORT = 20,
    ZZUSB_DRIVER_EVENT_CONTROL,
    ZZUSB_DRIVER_EVENT_MAILBOX
};

struct zzusb_driver_diag_event {
    volatile uint32_t sequence;
    uint32_t request_id;
    uint32_t controller_epoch;
    uint32_t detail;
    uint32_t timestamp;
    uint16_t type;
    uint16_t status;
    uint16_t address;
    uint16_t topology;
    uint8_t endpoint;
    uint8_t direction;
    uint16_t schedule;
};

struct zzusb_driver_diag_snapshot {
    uint32_t magic;
    uint32_t generation;
    uint16_t version;
    uint16_t event_count;
    uint32_t next_sequence;
    uint32_t lost_events;
    uint32_t capabilities;
    uint32_t controller_epoch;
    uint32_t counters[ZZUSB_DRIVER_DIAG_COUNTER_COUNT];
    struct zzusb_driver_diag_event events[ZZUSB_DRIVER_DIAG_EVENT_COUNT];
};
struct zzusb_engine_request {
    uint32_t request_id;
    uint32_t controller_epoch;
    uint16_t transport_status;

    uint8_t state;
    uint8_t terminal_result;
    uint8_t abort_requested;
    uint8_t reply_claimed;
    uint8_t buffer_released;
};

void zzusb_engine_init(struct zzusb_engine_request *request);
int zzusb_engine_queue(struct zzusb_engine_request *request);
int zzusb_engine_dispatch(struct zzusb_engine_request *request);
int zzusb_engine_begin(struct zzusb_engine_request *request,
                       uint32_t request_id, uint32_t controller_epoch);
int zzusb_engine_abort(struct zzusb_engine_request *request);
int zzusb_engine_timeout(struct zzusb_engine_request *request);
int zzusb_engine_complete(struct zzusb_engine_request *request,
                          uint32_t request_id, uint32_t controller_epoch,
                          uint16_t transport_status);
int zzusb_engine_retire(struct zzusb_engine_request *request,
                        enum zzusb_terminal_result result,
                        uint16_t transport_status);
int zzusb_engine_claim_reply(struct zzusb_engine_request *request);
int zzusb_engine_release_buffer(struct zzusb_engine_request *request);
enum zzusb_error_class zzusb_engine_classify_status(uint16_t status);
uint16_t zzusb_engine_rt_slice_ms(uint32_t remaining_ms,
                                  uint32_t service_limit_ms);
int zzusb_engine_rt_retry_status(uint16_t status);
void zzusb_engine_diag_reset(void);
void zzusb_engine_diag_count(enum zzusb_driver_diag_counter counter);
int zzusb_engine_diag_high_water(uint32_t depth);
void zzusb_engine_diag_record(uint16_t type, uint16_t status,
                              uint32_t request_id,
                              uint32_t controller_epoch,
                              uint16_t address, uint8_t endpoint,
                              uint8_t direction, uint16_t topology,
                              uint16_t schedule, uint32_t detail,
                              uint32_t timestamp);
int zzusb_engine_diag_snapshot(struct zzusb_driver_diag_snapshot *snapshot,
                               uint32_t capabilities,
                               uint32_t controller_epoch,
                               unsigned retries);

#ifdef __cplusplus
}
#endif
#endif
