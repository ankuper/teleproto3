/*
 * t3_csprng_none.c — no-op CSPRNG backend (Story 1-11 CI only).
 *
 * Always returns T3_ERR_RNG. Used during the portability-gate CI run
 * (T3_CSPRNG_BACKEND=none) where the library compiles but cannot provide
 * entropy. Stories 1-12/1-13/1-14 replace this with platform backends.
 */

#include "internal/t3_csprng.h"

t3_result_t t3_csprng_bytes(uint8_t *buf, size_t len) {
    (void)buf;
    (void)len;
    return T3_ERR_RNG;
}
