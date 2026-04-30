/*
 * retry.h — internal retry-FSM ring-buffer types.
 *
 * This is the reference implementation of the Type3 protocol.
 * Normative behaviour is defined in spec/. Where they differ, spec/ wins.
 *
 * Stability: internal header; NOT exported; NOT ABI-stable.
 */

#ifndef T3_INTERNAL_RETRY_H
#define T3_INTERNAL_RETRY_H

#include <stdint.h>
#include "t3.h"

typedef struct {
    uint64_t slots[8];
    uint8_t  head;
    uint8_t  count;
} t3_retry_ring_t;

struct t3_session;

__attribute__((visibility("hidden")))
t3_retry_state_t t3_retry_ring_record(struct t3_session *s, uint64_t now_monotonic_ns);
__attribute__((visibility("hidden")))
t3_retry_state_t t3_retry_ring_get(const struct t3_session *s);
__attribute__((visibility("hidden")))
t3_result_t      t3_retry_ring_user_retry(struct t3_session *s);

#endif /* T3_INTERNAL_RETRY_H */
