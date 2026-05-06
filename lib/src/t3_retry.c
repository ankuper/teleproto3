/*
 * t3_retry.c — anti-probe retry-tier state machine (FR43).
 *
 * This is the reference implementation of the Type3 protocol.
 * Normative behaviour is defined in spec/. Where they differ, spec/ wins.
 *
 * Source: spec/anti-probe.md §7.2 + FR43 + story 1.7 Task 5 + Dev Notes §C, §D.
 * Stability: lib-v0.1.0 ABI (frozen by story 1.6).
 */

#include "t3.h"
#include "internal/session.h"
#include "internal/retry.h"
#include <stdint.h>
#include <string.h>

#define NS60  (60ULL  * 1000000000ULL)
#define NS30  (30ULL  * 1000000000ULL)
#define NS5MIN (5ULL * 60ULL * 1000000000ULL)

T3_HIDDEN
t3_retry_state_t t3_retry_ring_record(struct t3_session *s, uint64_t now_ns) {
    t3_retry_ring_t *r = &s->ring;

    /* Evict slots older than 60 s. */
    uint64_t kept_buf[8];
    uint8_t  kept = 0;
    for (uint8_t i = 0; i < r->count; i++) {
        uint8_t idx = (uint8_t)((r->head + 8u - r->count + i) & 7u);
        if (now_ns - r->slots[idx] <= NS60) kept_buf[kept++] = r->slots[idx];
    }

    /* Append now_ns; drop oldest if ring full. */
    if (kept == 8) {
        for (uint8_t i = 0; i < 7; i++) kept_buf[i] = kept_buf[i+1];
        kept = 7;
    }
    kept_buf[kept++] = now_ns;

    /* Rewrite ring. */
    for (uint8_t i = 0; i < kept; i++) r->slots[i] = kept_buf[i];
    r->count = kept;
    r->head  = (uint8_t)(kept & 7u);

    /* Count entries in last 30 s. */
    uint8_t in30 = 0;
    for (uint8_t i = 0; i < kept; i++)
        if (now_ns - kept_buf[i] <= NS30) in30++;

    int continuous_tier2_5min =
        (s->state == T3_RETRY_TIER2 && s->tier2_entered_ns != 0 &&
         (now_ns - s->tier2_entered_ns) >= NS5MIN);

    t3_retry_state_t prev = s->state;
    if (in30 >= 5 || continuous_tier2_5min) s->state = T3_RETRY_TIER3;
    else if (kept >= 3)                     s->state = T3_RETRY_TIER2;
    else if (kept >= 1)                     s->state = T3_RETRY_TIER1;
    else                                    s->state = T3_RETRY_OK;

    if (s->state == T3_RETRY_TIER2 && prev != T3_RETRY_TIER2)
        s->tier2_entered_ns = now_ns;
    if (s->state != T3_RETRY_TIER2)
        s->tier2_entered_ns = 0;

    return s->state;
}

T3_HIDDEN
t3_retry_state_t t3_retry_ring_get(const struct t3_session *s) { return s->state; }

T3_HIDDEN
t3_result_t t3_retry_ring_user_retry(struct t3_session *s) {
    if (s->state != T3_RETRY_TIER3) return T3_ERR_INVALID_ARG;
    memset(&s->ring, 0, sizeof(s->ring));
    s->state = T3_RETRY_OK;
    s->tier2_entered_ns = 0;
    s->last_close_ns = 0;
    return T3_OK;
}

T3_API t3_result_t t3_retry_record_close(t3_session_t *sess,
                                         uint64_t now_monotonic_ns,
                                         t3_retry_state_t *out_state) {
    if (!sess || !out_state) return T3_ERR_INVALID_ARG;
    if (sess->last_close_ns != 0 && now_monotonic_ns < sess->last_close_ns)
        return T3_ERR_CLOCK_BACKWARDS;
    sess->last_close_ns = now_monotonic_ns;
    *out_state = t3_retry_ring_record(sess, now_monotonic_ns);
    return T3_OK;
}

T3_API t3_retry_state_t t3_retry_get_state(const t3_session_t *sess) {
    if (!sess) return T3_RETRY_OK;
    return t3_retry_ring_get(sess);
}

T3_API t3_result_t t3_retry_user_retry(t3_session_t *sess) {
    if (!sess) return T3_ERR_INVALID_ARG;
    return t3_retry_ring_user_retry(sess);
}
