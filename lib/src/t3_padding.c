/*
 * t3_padding.c — padding frame generation and payload splitting.
 *
 * This is the reference implementation of the Type3 protocol.
 * Normative behaviour is defined in spec/. Where they differ, spec/ wins.
 *
 * Source: spec/wire-format.md §2.1 + Story 11-3 + Epic 11.
 * Stability: lib-v0.2.0 ABI.
 */

#include "t3.h"
#include "internal/session.h"
#include <stdint.h>
#include <stddef.h>

/*
 * t3__rng_range — draw a uniformly random size_t in [lo, hi] via
 * rejection sampling over a 32-bit RNG word.
 *
 * Preconditions (callers must ensure):
 *   - lo <= hi
 *   - hi - lo + 1 <= 0xFFFFFFFF  (range fits in uint32_t)
 *
 * Returns T3_OK and writes the result to *out, or T3_ERR_RNG on RNG failure.
 */
static t3_result_t t3__rng_range(t3_session_t *sess,
                                 size_t lo, size_t hi,
                                 size_t *out) {
    size_t  range        = hi - lo + 1;
    uint32_t bucket_count = (uint32_t)(0xFFFFFFFFu / range);
    uint32_t cutoff       = bucket_count * (uint32_t)range;

    for (;;) {
        uint8_t bytes[4];
        if (sess->cb.rng(sess->cb.ctx, bytes, sizeof(bytes)) != 0) {
            return T3_ERR_RNG;
        }
        uint32_t r = (uint32_t)bytes[0]
                   | ((uint32_t)bytes[1] <<  8)
                   | ((uint32_t)bytes[2] << 16)
                   | ((uint32_t)bytes[3] << 24);
        if (r >= cutoff) {
            continue;
        }
        *out = lo + (r % range);
        return T3_OK;
    }
}

T3_API t3_result_t t3_padding_generate(t3_session_t *sess,
                                       uint8_t *buf, size_t min_len,
                                       size_t max_len, size_t *out_len) {
    if (!sess || !buf || !out_len) {
        return T3_ERR_INVALID_ARG;
    }
    if (!sess->callbacks_bound) {
        return T3_ERR_INVALID_ARG;
    }
    if (min_len < 1 || max_len < min_len || max_len > 65535) {
        return T3_ERR_INVALID_ARG;
    }

    size_t len = 0;
    t3_result_t rc = t3__rng_range(sess, min_len, max_len, &len);
    if (rc != T3_OK) {
        return rc;
    }

    buf[0] = T3_PADDING_MARKER;
    if (len > 1) {
        if (sess->cb.rng(sess->cb.ctx, buf + 1, len - 1) != 0) {
            return T3_ERR_RNG;
        }
    }

    *out_len = len;
    return T3_OK;
}

T3_API t3_result_t t3_split_plan(t3_session_t *sess,
                                 size_t total_len,
                                 size_t min_chunk, size_t max_chunk,
                                 size_t *plan, size_t max_chunks,
                                 size_t *out_count) {
    if (!sess || !plan || !out_count) {
        return T3_ERR_INVALID_ARG;
    }
    if (!sess->callbacks_bound) {
        return T3_ERR_INVALID_ARG;
    }
    if (total_len < 1 || min_chunk < 1 || max_chunk < min_chunk || total_len < min_chunk) {
        return T3_ERR_INVALID_ARG;
    }
    if (max_chunks == 0) {
        return T3_ERR_INVALID_ARG;
    }

    size_t max_possible = 0;
    /* SIZE_MAX is defined in <stdint.h> */
    if (max_chunk > SIZE_MAX / max_chunks) {
        max_possible = SIZE_MAX;
    } else {
        max_possible = max_chunks * max_chunk;
    }

    if (max_possible < total_len) {
        return T3_ERR_BUF_TOO_SMALL;
    }

    size_t rem_len = total_len;
    size_t count = 0;

    for (size_t i = 0; i < max_chunks; i++) {
        size_t rem_slots = max_chunks - 1 - i;
        int finish = 0;

        if (rem_len <= max_chunk) {
            if (rem_slots == 0 || rem_len < 2 * min_chunk) {
                finish = 1;
            } else {
                uint8_t rand_choice;
                if (sess->cb.rng(sess->cb.ctx, &rand_choice, 1) != 0) {
                    return T3_ERR_RNG;
                }
                finish = (rand_choice % 2 == 0);
            }
        }

        if (finish) {
            plan[i] = rem_len;
            count = i + 1;
            rem_len = 0;
            break;
        }

        /* We split. Calculate bounds L and H for the chunk size c */
        size_t max_rem_cover = 0;
        if (rem_slots > 0 && max_chunk > SIZE_MAX / rem_slots) {
            max_rem_cover = SIZE_MAX;
        } else {
            max_rem_cover = rem_slots * max_chunk;
        }

        size_t L = min_chunk;
        if (rem_len > max_rem_cover) {
            L = rem_len - max_rem_cover;
            if (L < min_chunk) {
                L = min_chunk;
            }
        }

        size_t H = max_chunk;
        if (rem_len - min_chunk < H) {
            H = rem_len - min_chunk;
        }

        /* Safety: clamp arithmetic must never produce H < L.
         * This should be unreachable given a valid feasibility check above,
         * but guard explicitly to prevent size_t underflow UB. */
        if (H < L) {
            return T3_ERR_INTERNAL;
        }

        size_t c = 0;
        t3_result_t rc = t3__rng_range(sess, L, H, &c);
        if (rc != T3_OK) {
            return rc;
        }

        plan[i] = c;
        rem_len -= c;
    }

    if (rem_len > 0) {
        return T3_ERR_INTERNAL;
    }

    *out_count = count;
    return T3_OK;
}

