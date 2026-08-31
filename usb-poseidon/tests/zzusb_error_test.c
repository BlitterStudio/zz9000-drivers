#include <stdio.h>
#include <stdlib.h>

#include "zzusb_engine.h"

#define CHECK(expr) do { if (!(expr)) return EXIT_FAILURE; } while (0)

int main(void)
{
    CHECK(zzusb_engine_classify_status(ZZUSB_ENGINE_STATUS_OK) == ZZUSB_ERROR_NONE);
    CHECK(zzusb_engine_classify_status(ZZUSB_ENGINE_STATUS_TIMEOUT) == ZZUSB_ERROR_TIMEOUT);
    CHECK(zzusb_engine_classify_status(ZZUSB_ENGINE_STATUS_NAK) == ZZUSB_ERROR_NAK);
    CHECK(zzusb_engine_classify_status(ZZUSB_ENGINE_STATUS_STALL) == ZZUSB_ERROR_STALL);
    CHECK(zzusb_engine_classify_status(ZZUSB_ENGINE_STATUS_OFFLINE) == ZZUSB_ERROR_OFFLINE);
    CHECK(zzusb_engine_classify_status(ZZUSB_ENGINE_STATUS_CRC) == ZZUSB_ERROR_CRC);
    CHECK(zzusb_engine_classify_status(ZZUSB_ENGINE_STATUS_BABBLE) == ZZUSB_ERROR_BABBLE);
    CHECK(zzusb_engine_classify_status(ZZUSB_ENGINE_STATUS_OVERRUN) == ZZUSB_ERROR_OVERFLOW);
    CHECK(zzusb_engine_classify_status(ZZUSB_ENGINE_STATUS_UNDERRUN) == ZZUSB_ERROR_UNDERFLOW);
    CHECK(zzusb_engine_classify_status(ZZUSB_ENGINE_STATUS_CANCELLED) == ZZUSB_ERROR_CANCELLED);
    CHECK(zzusb_engine_classify_status(ZZUSB_ENGINE_STATUS_HOSTERROR) == ZZUSB_ERROR_HOST);
    CHECK(zzusb_engine_classify_status(ZZUSB_ENGINE_STATUS_STALE) == ZZUSB_ERROR_HOST);
    CHECK(zzusb_engine_classify_status(ZZUSB_ENGINE_STATUS_BUSY) == ZZUSB_ERROR_HOST);
    CHECK(zzusb_engine_classify_status(ZZUSB_ENGINE_STATUS_ERROR) == ZZUSB_ERROR_HOST);
    CHECK(zzusb_engine_classify_status(ZZUSB_ENGINE_STATUS_PENDING) == ZZUSB_ERROR_HOST);
    CHECK(zzusb_engine_classify_status(ZZUSB_ENGINE_STATUS_BADPARAM) == ZZUSB_ERROR_BAD_PARAMETER);
    CHECK(zzusb_engine_classify_status(ZZUSB_ENGINE_STATUS_NOMEM) == ZZUSB_ERROR_NO_MEMORY);
    CHECK(zzusb_engine_classify_status(ZZUSB_ENGINE_STATUS_UNSUPPORTED) == ZZUSB_ERROR_UNSUPPORTED);
    puts("USB proxy status classification satisfied");
    return EXIT_SUCCESS;
}
