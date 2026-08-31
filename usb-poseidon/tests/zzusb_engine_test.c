#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "zzusb_engine.h"

#define CHECK(expr) do {     if (!(expr)) {         fprintf(stderr, "%s:%d: check failed: %s\n",                 __FILE__, __LINE__, #expr);         return EXIT_FAILURE;     } } while (0)

static void prepare(struct zzusb_engine_request *request,
                    uint32_t id, uint32_t epoch)
{
    zzusb_engine_init(request);
    if (zzusb_engine_queue(request) == 0 ||
        zzusb_engine_dispatch(request) == 0 ||
        zzusb_engine_begin(request, id, epoch) == 0)
        abort();
}

int main(void)
{
    struct zzusb_engine_request request;
    uint32_t seed = 0x5a17u;
    int replies = 0;
    int releases = 0;

    zzusb_engine_init(&request);
    CHECK(zzusb_engine_queue(&request));
    CHECK(zzusb_engine_abort(&request));
    CHECK(request.terminal_result == ZZUSB_RESULT_ABORTED);
    replies += zzusb_engine_claim_reply(&request);
    replies += zzusb_engine_claim_reply(&request);
    releases += zzusb_engine_release_buffer(&request);
    releases += zzusb_engine_release_buffer(&request);
    CHECK(replies == 1);
    CHECK(releases == 1);

    prepare(&request, 7u, 3u);
    CHECK(!zzusb_engine_abort(&request));
    CHECK(request.abort_requested);
    CHECK(!zzusb_engine_complete(&request, 6u, 3u, ZZUSB_ENGINE_STATUS_OK));
    CHECK(!zzusb_engine_complete(&request, 7u, 2u, ZZUSB_ENGINE_STATUS_OK));
    CHECK(zzusb_engine_complete(&request, 7u, 3u, ZZUSB_ENGINE_STATUS_OK));
    CHECK(request.terminal_result == ZZUSB_RESULT_ABORTED);
    CHECK(zzusb_engine_claim_reply(&request));
    CHECK(!zzusb_engine_claim_reply(&request));

    prepare(&request, 8u, 4u);
    CHECK(zzusb_engine_timeout(&request));
    CHECK(request.state == ZZUSB_REQ_RETIRING);
    CHECK(!zzusb_engine_claim_reply(&request));
    CHECK(zzusb_engine_retire(&request, ZZUSB_RESULT_TIMEOUT,
                              ZZUSB_ENGINE_STATUS_TIMEOUT));
    CHECK(request.terminal_result == ZZUSB_RESULT_TIMEOUT);

    for (int iteration = 0; iteration < 10000; iteration++) {
        int claimed = 0;
        int released = 0;
        prepare(&request, (uint32_t)iteration + 1u, 9u);
        for (int step = 0; step < 8; step++) {
            seed = seed * 1664525u + 1013904223u;
            switch ((seed >> 28) & 3u) {
            case 0: zzusb_engine_abort(&request); break;
            case 1: zzusb_engine_complete(&request,
                        (uint32_t)iteration + 1u, 9u,
                        ZZUSB_ENGINE_STATUS_OK); break;
            case 2: zzusb_engine_timeout(&request); break;
            default: zzusb_engine_retire(&request,
                        ZZUSB_RESULT_TIMEOUT, ZZUSB_ENGINE_STATUS_TIMEOUT); break;
            }
            claimed += zzusb_engine_claim_reply(&request);
            released += zzusb_engine_release_buffer(&request);
        }
        if (request.state != ZZUSB_REQ_TERMINAL)
            zzusb_engine_retire(&request, ZZUSB_RESULT_IO_ERROR,
                                ZZUSB_ENGINE_STATUS_HOSTERROR);
        claimed += zzusb_engine_claim_reply(&request);
        released += zzusb_engine_release_buffer(&request);
        CHECK(claimed == 1);
        CHECK(released == 1);
    }

    puts("USB request engine exact-once contract satisfied");
    return EXIT_SUCCESS;
}
