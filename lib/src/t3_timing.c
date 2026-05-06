/*
 * t3_timing.c — silent-close uniform-random timing engine.
 *
 * This is the reference implementation of the Type3 protocol.
 * Normative behaviour is defined in spec/. Where they differ, spec/ wins.
 *
 * Source: spec/anti-probe.md §8 + AR-C2 + story 1.7 Task 4 + Dev Notes §E.
 * Stability: lib-v0.1.0 ABI (frozen by story 1.6).
 */

#include "t3.h"
#include "internal/timing.h"
#include "internal/session.h"
#include <stdint.h>
#include <limits.h>

T3_HIDDEN
int t3_timing_rejection_sample_uniform_ns(const t3_callbacks_t *cb,
                                          uint64_t lo_ns, uint64_t hi_ns,
                                          uint64_t *out_ns) {
    if (!cb || !out_ns || hi_ns < lo_ns) return -1;
    uint64_t range       = hi_ns - lo_ns + 1ULL;
    uint64_t bucket_count = (uint64_t)UINT64_MAX / range;
    uint64_t cutoff      = bucket_count * range;
    for (;;) {
        uint8_t bytes[8];
        if (cb->rng(cb->ctx, bytes, sizeof bytes) != 0) return -2;
        uint64_t r = (uint64_t)bytes[0]
                   | ((uint64_t)bytes[1] << 8) | ((uint64_t)bytes[2] << 16)
                   | ((uint64_t)bytes[3] << 24) | ((uint64_t)bytes[4] << 32)
                   | ((uint64_t)bytes[5] << 40) | ((uint64_t)bytes[6] << 48)
                   | ((uint64_t)bytes[7] << 56);
        if (r >= cutoff) continue;
        *out_ns = lo_ns + (r % range);
        return 0;
    }
}

T3_API t3_result_t t3_silent_close_delay_sample_ns(t3_session_t *sess,
                                                   uint64_t *out_ns) {
    if (!sess || !out_ns) return T3_ERR_INVALID_ARG;
    if (!sess->callbacks_bound) return T3_ERR_INVALID_ARG;
    static const uint64_t LO = 50000000ULL;
    static const uint64_t HI = 200000000ULL;
    int rc = t3_timing_rejection_sample_uniform_ns(&sess->cb, LO, HI, out_ns);
    if (rc == -2) return T3_ERR_RNG;
    if (rc != 0)  return T3_ERR_INVALID_ARG;
    return T3_OK;
}
