/* Host tests for sampling-calibration UI scoring helpers. */
#include <stdio.h>

#include "zz9000_sampling_calibration.h"

static int checks;
static int failures;

#define CHECK(expr, message) do { \
    checks++; \
    if (!(expr)) { \
        failures++; \
        printf("  FAIL: %s (%s:%d)\n", message, __FILE__, __LINE__); \
    } \
} while (0)

int main(void)
{
    CHECK(zz_sampling_score_clean(0, 0),
        "a zero-error and unchanged capture is clean");
    CHECK(!zz_sampling_score_clean(1, 0),
        "an expected-pixel error rejects a bin");
    CHECK(!zz_sampling_score_clean(0, 1),
        "a changing capture rejects a bin");
    CHECK(zz_sampling_phase_bin(ZZ_CAPTURE_PHASE_MIN) == 0,
        "the first sweep phase selects bin zero");
    CHECK(zz_sampling_phase_bin(ZZ_CAPTURE_PHASE_MIN +
        (int)(17 * ZZ_CAPTURE_BIN_STEPS)) == 17,
        "coarse bin phases map exactly");
    CHECK(zz_sampling_phase_bin(ZZ_CAPTURE_PHASE_MAX) ==
        ZZ_CAPTURE_BINS - 1,
        "the final phase remains in the final bin");

    printf("sampling_calibration_test: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
