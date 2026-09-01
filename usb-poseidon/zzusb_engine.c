/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "zzusb_engine.h"

#include <string.h>

struct zzusb_engine_diag_state {
    volatile uint32_t next_sequence;
    volatile uint32_t lost_events;
    volatile uint32_t mutation_generation;
    volatile uint32_t counters[ZZUSB_DRIVER_DIAG_COUNTER_COUNT];
    struct zzusb_driver_diag_event events[ZZUSB_DRIVER_DIAG_EVENT_COUNT];
};

static struct zzusb_engine_diag_state engine_diag;
static zzusb_engine_diag_critical_fn diag_critical_enter;
static zzusb_engine_diag_critical_fn diag_critical_leave;

_Static_assert(sizeof(struct zzusb_driver_diag_event) == 32,
               "driver diagnostic event size changed");


static void diag_barrier(void)
{
    __asm__ __volatile__("" ::: "memory");
}

void zzusb_engine_diag_set_critical(
    zzusb_engine_diag_critical_fn enter,
    zzusb_engine_diag_critical_fn leave)
{
    diag_critical_enter = enter;
    diag_critical_leave = leave;
}
/*
 * Driver request mutation is serialized. Readers use this odd/even
 * generation to reject snapshots interrupted by AbortIO or poll-task work.
 */
static uint32_t diag_mutation_begin(void)
{
    uint32_t generation;

    if (diag_critical_enter)
        diag_critical_enter();
    generation = engine_diag.mutation_generation + 1U;
    diag_barrier();
    return generation;
}

static void diag_mutation_end(uint32_t generation)
{
    diag_barrier();
    engine_diag.mutation_generation = generation + 1U;
    if (diag_critical_leave)
        diag_critical_leave();
}


void zzusb_engine_diag_reset(void)
{
    uint32_t generation = diag_mutation_begin();

    engine_diag.next_sequence = 0;
    engine_diag.lost_events = 0;
    memset((void *)engine_diag.counters, 0, sizeof(engine_diag.counters));
    memset(engine_diag.events, 0, sizeof(engine_diag.events));
    diag_mutation_end(generation);
}

void zzusb_engine_diag_count(enum zzusb_driver_diag_counter counter)
{
    if ((unsigned)counter < ZZUSB_DRIVER_DIAG_COUNTER_COUNT) {
        uint32_t generation = diag_mutation_begin();

        engine_diag.counters[counter]++;
        diag_mutation_end(generation);
    }
}

int zzusb_engine_diag_high_water(uint32_t depth)
{
    uint32_t generation = diag_mutation_begin();

    if (depth <=
        engine_diag.counters[ZZUSB_DRIVER_COUNT_QUEUE_HIGH_WATER]) {
        diag_mutation_end(generation);
        return 0;
    }
    engine_diag.counters[ZZUSB_DRIVER_COUNT_QUEUE_HIGH_WATER] = depth;
    diag_mutation_end(generation);
    return 1;
}

void zzusb_engine_diag_record(uint16_t type, uint16_t status,
                              uint32_t request_id,
                              uint32_t controller_epoch,
                              uint16_t address, uint8_t endpoint,
                              uint8_t direction, uint16_t topology,
                              uint16_t schedule, uint32_t detail,
                              uint32_t timestamp)
{
    uint32_t generation = diag_mutation_begin();
    uint32_t sequence = ++engine_diag.next_sequence;
    struct zzusb_driver_diag_event *event =
        &engine_diag.events[(sequence - 1U) %
                            ZZUSB_DRIVER_DIAG_EVENT_COUNT];

    if (sequence > ZZUSB_DRIVER_DIAG_EVENT_COUNT)
        engine_diag.lost_events++;
    event->sequence = 0;
    event->request_id = request_id;
    event->controller_epoch = controller_epoch;
    event->detail = detail;
    event->timestamp = timestamp;
    event->type = type;
    event->status = status;
    event->address = address;
    event->topology = topology;
    event->endpoint = endpoint;
    event->direction = direction;
    event->schedule = schedule;
    diag_barrier();
    event->sequence = sequence;
    diag_mutation_end(generation);
}

int zzusb_engine_diag_snapshot(struct zzusb_driver_diag_snapshot *snapshot,
                               uint32_t capabilities,
                               uint32_t controller_epoch,
                               unsigned retries)
{
    if (!snapshot)
        return 0;

    while (retries--) {
        uint32_t before = engine_diag.mutation_generation;
        if (before & 1U)
            continue;
        diag_barrier();
        uint32_t next = engine_diag.next_sequence;
        uint32_t count = next < ZZUSB_DRIVER_DIAG_EVENT_COUNT
                       ? next : ZZUSB_DRIVER_DIAG_EVENT_COUNT;
        uint32_t first = count ? next - count + 1U : 0U;
        uint32_t encoded = 0;

        diag_barrier();
        memset(snapshot, 0, sizeof(*snapshot));
        snapshot->magic = ZZUSB_DRIVER_DIAG_MAGIC;
        snapshot->version = ZZUSB_DRIVER_DIAG_VERSION;
        snapshot->next_sequence = next;
        snapshot->lost_events = engine_diag.lost_events;
        snapshot->capabilities = capabilities;
        snapshot->controller_epoch = controller_epoch;
        for (uint32_t i = 0; i < ZZUSB_DRIVER_DIAG_COUNTER_COUNT; i++)
            snapshot->counters[i] = engine_diag.counters[i];
        for (uint32_t i = 0; i < count; i++) {
            uint32_t sequence = first + i;
            const struct zzusb_driver_diag_event *event =
                &engine_diag.events[(sequence - 1U) %
                                    ZZUSB_DRIVER_DIAG_EVENT_COUNT];
            if (event->sequence != sequence)
                continue;
            snapshot->events[encoded] = *event;
            encoded++;
        }
        snapshot->event_count = (uint16_t)encoded;
        diag_barrier();
        if (before == engine_diag.mutation_generation) {
            snapshot->generation = before;
            return 1;
        }
    }
    memset(snapshot, 0, sizeof(*snapshot));
    return 0;
}

static int make_terminal(struct zzusb_engine_request *request,
                         enum zzusb_terminal_result result,
                         uint16_t status)
{
    if (!request || request->state == ZZUSB_REQ_FREE ||
        request->state == ZZUSB_REQ_TERMINAL)
        return 0;
    request->transport_status = status;
    request->terminal_result = request->abort_requested
        ? ZZUSB_RESULT_ABORTED : (uint8_t)result;
    request->state = ZZUSB_REQ_TERMINAL;
    return 1;
}

void zzusb_engine_init(struct zzusb_engine_request *request)
{
    if (request)
        memset(request, 0, sizeof(*request));
}

int zzusb_engine_queue(struct zzusb_engine_request *request)
{
    if (!request || request->state != ZZUSB_REQ_FREE)
        return 0;
    request->state = ZZUSB_REQ_QUEUED;
    return 1;
}

int zzusb_engine_dispatch(struct zzusb_engine_request *request)
{
    if (!request || request->state != ZZUSB_REQ_QUEUED)
        return 0;
    request->state = ZZUSB_REQ_DISPATCHING;
    return 1;
}

int zzusb_engine_begin(struct zzusb_engine_request *request,
                       uint32_t request_id, uint32_t controller_epoch)
{
    if (!request || request->state != ZZUSB_REQ_DISPATCHING ||
        request_id == 0)
        return 0;
    request->request_id = request_id;
    request->controller_epoch = controller_epoch;
    request->state = ZZUSB_REQ_IN_FLIGHT;
    zzusb_engine_diag_count(ZZUSB_DRIVER_COUNT_REQUEST);
    zzusb_engine_diag_record(ZZUSB_DRIVER_EVENT_REQUEST,
                             ZZUSB_ENGINE_STATUS_PENDING,
                             request_id, controller_epoch,
                             0, 0, 0, 0, 0, 0, 0);
    return 1;
}

int zzusb_engine_abort(struct zzusb_engine_request *request)
{
    if (!request || request->state == ZZUSB_REQ_FREE ||
        request->state == ZZUSB_REQ_TERMINAL ||
        request->abort_requested)
        return 0;
    request->abort_requested = 1;
    zzusb_engine_diag_count(ZZUSB_DRIVER_COUNT_CANCELLATION);
    zzusb_engine_diag_record(ZZUSB_DRIVER_EVENT_CANCELLATION,
                             ZZUSB_ENGINE_STATUS_CANCELLED,
                             request->request_id,
                             request->controller_epoch,
                             0, 0, 0, 0, 0, request->state, 0);
    if (request->state == ZZUSB_REQ_QUEUED)
        return make_terminal(request, ZZUSB_RESULT_ABORTED,
                             ZZUSB_ENGINE_STATUS_CANCELLED);
    return 0;
}

int zzusb_engine_timeout(struct zzusb_engine_request *request)
{
    if (!request || request->state != ZZUSB_REQ_IN_FLIGHT)
        return 0;
    request->state = ZZUSB_REQ_RETIRING;
    zzusb_engine_diag_count(ZZUSB_DRIVER_COUNT_TIMEOUT);
    zzusb_engine_diag_record(ZZUSB_DRIVER_EVENT_TIMEOUT,
                             ZZUSB_ENGINE_STATUS_TIMEOUT,
                             request->request_id,
                             request->controller_epoch,
                             0, 0, 0, 0, 0, request->state, 0);
    return 1;
}

int zzusb_engine_complete(struct zzusb_engine_request *request,
                          uint32_t request_id, uint32_t controller_epoch,
                          uint16_t transport_status)
{
    enum zzusb_terminal_result result;

    if (!request)
        return 0;
    if ((request->state != ZZUSB_REQ_IN_FLIGHT &&
         request->state != ZZUSB_REQ_RETIRING) ||
        request_id != request->request_id ||
        controller_epoch != request->controller_epoch) {
        zzusb_engine_diag_count(ZZUSB_DRIVER_COUNT_LATE_COMPLETION);
        zzusb_engine_diag_record(ZZUSB_DRIVER_EVENT_LATE_COMPLETION,
                                 transport_status,
                                 request_id, controller_epoch,
                                 0, 0, 0, 0, 0, request->state, 0);
        return 0;
    }
    result = transport_status == ZZUSB_ENGINE_STATUS_OK
        ? ZZUSB_RESULT_OK : ZZUSB_RESULT_IO_ERROR;
    zzusb_engine_diag_count(ZZUSB_DRIVER_COUNT_COMPLETION);
    if (transport_status != ZZUSB_ENGINE_STATUS_OK)
        zzusb_engine_diag_count(ZZUSB_DRIVER_COUNT_HOST_ERROR);
    zzusb_engine_diag_record(ZZUSB_DRIVER_EVENT_COMPLETION,
                             transport_status, request_id,
                             controller_epoch, 0, 0, 0, 0, 0,
                             request->abort_requested, 0);
    return make_terminal(request, result, transport_status);
}

int zzusb_engine_retire(struct zzusb_engine_request *request,
                        enum zzusb_terminal_result result,
                        uint16_t transport_status)
{
    if (!request ||
        (request->state != ZZUSB_REQ_DISPATCHING &&
         request->state != ZZUSB_REQ_IN_FLIGHT &&
         request->state != ZZUSB_REQ_RETIRING))
        return 0;
    zzusb_engine_diag_count(ZZUSB_DRIVER_COUNT_COMPLETION);
    zzusb_engine_diag_record(ZZUSB_DRIVER_EVENT_COMPLETION,
                             transport_status,
                             request->request_id,
                             request->controller_epoch,
                             0, 0, 0, 0, 0, result, 0);
    return make_terminal(request, result, transport_status);
}

int zzusb_engine_claim_reply(struct zzusb_engine_request *request)
{
    if (!request || request->state != ZZUSB_REQ_TERMINAL ||
        request->reply_claimed)
        return 0;
    request->reply_claimed = 1;
    return 1;
}

int zzusb_engine_release_buffer(struct zzusb_engine_request *request)
{
    if (!request || request->state != ZZUSB_REQ_TERMINAL ||
        request->buffer_released)
        return 0;
    request->buffer_released = 1;
    return 1;
}

enum zzusb_error_class zzusb_engine_classify_status(uint16_t status)
{
    switch (status) {
    case ZZUSB_ENGINE_STATUS_OK:          return ZZUSB_ERROR_NONE;
    case ZZUSB_ENGINE_STATUS_TIMEOUT:     return ZZUSB_ERROR_TIMEOUT;
    case ZZUSB_ENGINE_STATUS_NAK:         return ZZUSB_ERROR_NAK;
    case ZZUSB_ENGINE_STATUS_STALL:       return ZZUSB_ERROR_STALL;
    case ZZUSB_ENGINE_STATUS_OFFLINE:     return ZZUSB_ERROR_OFFLINE;
    case ZZUSB_ENGINE_STATUS_CRC:         return ZZUSB_ERROR_CRC;
    case ZZUSB_ENGINE_STATUS_BABBLE:      return ZZUSB_ERROR_BABBLE;
    case ZZUSB_ENGINE_STATUS_OVERRUN:     return ZZUSB_ERROR_OVERFLOW;
    case ZZUSB_ENGINE_STATUS_UNDERRUN:    return ZZUSB_ERROR_UNDERFLOW;
    case ZZUSB_ENGINE_STATUS_CANCELLED:   return ZZUSB_ERROR_CANCELLED;
    case ZZUSB_ENGINE_STATUS_BADPARAM:    return ZZUSB_ERROR_BAD_PARAMETER;
    case ZZUSB_ENGINE_STATUS_NOMEM:       return ZZUSB_ERROR_NO_MEMORY;
    case ZZUSB_ENGINE_STATUS_UNSUPPORTED: return ZZUSB_ERROR_UNSUPPORTED;
    case ZZUSB_ENGINE_STATUS_STALE:
    case ZZUSB_ENGINE_STATUS_HOSTERROR:
    case ZZUSB_ENGINE_STATUS_BUSY:
    case ZZUSB_ENGINE_STATUS_ERROR:
    default:                       return ZZUSB_ERROR_HOST;
    }
}
