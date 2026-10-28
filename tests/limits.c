/*
 * Deadline and body-cap constants. The suite does not sleep to prove them.
 * Ordinary calls, including emails get and lists: 1 MiB and a 30s total.
 * Receiving get: 128 MiB. Logs get and download: 64 MiB. Those three abort
 * after a 1 byte/s stall for 30s, and at a 300s ceiling.
 */

#include "internal.h"

#include <stdio.h>

int test_limits(void) {
    int failures = 0;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
            failures++;                                                                \
        }                                                                              \
    } while (0)

    CHECK(LIBRESEND_TIMEOUT_SEC == 30);
    CHECK(LIBRESEND_TIMEOUT_CEILING_SEC == 300);
    CHECK(LIBRESEND_LOW_SPEED_LIMIT == 1);
    CHECK(LIBRESEND_BODY_MAX == 1024 * 1024);
    CHECK(LIBRESEND_LARGE_BODY_MAX == (size_t)64 * 1024 * 1024);
    CHECK(LIBRESEND_RECEIVING_BODY_MAX == (size_t)128 * 1024 * 1024);
#undef CHECK
    return failures;
}
