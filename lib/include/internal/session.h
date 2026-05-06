/*
 * session.h — internal t3_session struct layout.
 *
 * This is the reference implementation of the Type3 protocol.
 * Normative behaviour is defined in spec/. Where they differ, spec/ wins.
 *
 * Stability: internal header; NOT exported; NOT ABI-stable.
 */

#ifndef T3_INTERNAL_SESSION_H
#define T3_INTERNAL_SESSION_H

#include <stdint.h>
#include <stddef.h>
#include "t3.h"
#include "t3_platform.h"
#include "retry.h"

struct t3_session {
    const t3_secret_t *secret;
    t3_callbacks_t     cb;
    int                callbacks_bound;
    uint8_t            parse_buf[4];
    size_t             parse_buf_len;
    uint8_t            peer_version;
    uint8_t            local_version;
    t3_retry_ring_t    ring;
    t3_retry_state_t   state;
    uint64_t           tier2_entered_ns;
    uint64_t           last_close_ns;
};

T3_HIDDEN
t3_result_t t3_session_handle_header_byte(t3_session_t *s, uint8_t b);

#endif /* T3_INTERNAL_SESSION_H */
