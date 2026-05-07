/*
 * t3_session_header.c — Session Header parse/serialise, version negotiation,
 * session lifecycle.
 *
 * This is the reference implementation of the Type3 protocol.
 * Normative behaviour is defined in spec/. Where they differ, spec/ wins.
 *
 * Wire format is LITTLE-ENDIAN regardless of host endianness.
 * Source: spec/wire-format.md §3 + story 1.7 Tasks 2 & 7.
 * Stability: lib-v0.1.x ABI (frozen by story 1.6; patch-bumped by 1a-1).
 */

#include "t3.h"
#include "internal/session.h"
#include "internal/timing.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Known command types at v0.1.1: 0x01 MTPROTO_PASSTHROUGH, 0x04 BENCH.
 * Shared by parser and serialiser to enforce command-type validity.
 */
static inline int t3_cmd_known(uint8_t c) {
    return c == T3_CMD_MTPROTO_PASSTHROUGH || c == T3_CMD_BENCH;
}

/* ======================================================================
 * Session Header parse/serialise (spec/wire-format.md §3)
 * ====================================================================== */

T3_API t3_result_t t3_header_parse(const uint8_t buf[4], t3_header_t *out) {
    if (!buf || !out) return T3_ERR_INVALID_ARG;
    uint8_t  cmd     = buf[0];
    uint8_t  version = buf[1];
    uint16_t flags   = (uint16_t)buf[2] | ((uint16_t)buf[3] << 8);  /* LE→host */

    /* Sentinel slots: always MALFORMED. */
    if (cmd == 0x00 || cmd == 0xFF) return T3_ERR_MALFORMED;
    /* Sentinel version 0x00: always MALFORMED. */
    if (version == 0x00) return T3_ERR_MALFORMED;
    /* Known cmd, unknown version → UNSUPPORTED_VERSION. */
    if (t3_cmd_known(cmd) && version > 0x01) return T3_ERR_UNSUPPORTED_VERSION;
    /* Unknown cmd at known version → MALFORMED. */
    if (!t3_cmd_known(cmd) && version == 0x01) return T3_ERR_MALFORMED;
    /* Unknown cmd at unknown version → UNSUPPORTED_VERSION (future-compat). */
    if (!t3_cmd_known(cmd) && version > 0x01) return T3_ERR_UNSUPPORTED_VERSION;
    /* Flags: at v0.1.x every bit MUST be zero. */
    if (flags != 0) return T3_ERR_MALFORMED;

    out->command_type = cmd;
    out->version      = version;
    out->flags        = flags;
    return T3_OK;
}

T3_API t3_result_t t3_header_serialise(const t3_header_t *in, uint8_t buf[4]) {
    if (!in || !buf) return T3_ERR_INVALID_ARG;
    /* Sender duties (spec §3): MUST NOT emit sentinels, unknown cmds, unknown
     * versions, or non-zero flags. Round-trip property: any output of this
     * function MUST parse-OK; tightening to `version != 0x01` keeps that
     * invariant at v0.1.1 (only known version). Loosen when v0.1.2+ adds new
     * known versions — single-line edit. */
    if (!t3_cmd_known(in->command_type)) return T3_ERR_MALFORMED;
    if (in->version != 0x01) return T3_ERR_MALFORMED;
    if (in->flags != 0) return T3_ERR_MALFORMED;
    buf[0] = in->command_type;
    buf[1] = in->version;
    buf[2] = (uint8_t)(in->flags & 0xFFu);          /* host→LE */
    buf[3] = (uint8_t)((in->flags >> 8) & 0xFFu);
    return T3_OK;
}

/* ======================================================================
 * Version negotiation (spec/wire-format.md §6)
 * ====================================================================== */

T3_API t3_result_t t3_session_negotiate_version(t3_session_t *sess,
                                                uint8_t peer_version,
                                                t3_version_action_t *out) {
    if (!sess || !out) return T3_ERR_INVALID_ARG;
    if (peer_version == 0x01) { sess->peer_version = peer_version; *out = T3_VERSION_OK; }
    else if (peer_version > 0x01) { sess->peer_version = peer_version; *out = T3_VERSION_RETRY_DOWNGRADE; }
    else { *out = T3_VERSION_SILENT_CLOSE; }
    return T3_OK;
}

/* ======================================================================
 * Fragmentation-tolerant header accumulator (spec/wire-format.md §2)
 * ====================================================================== */

T3_HIDDEN
t3_result_t t3_session_handle_header_byte(t3_session_t *s, uint8_t b) {
    if (!s || s->parse_buf_len >= 4) return T3_ERR_INVALID_ARG;
    s->parse_buf[s->parse_buf_len++] = b;
    if (s->parse_buf_len < 4) return T3_OK;
    t3_header_t hdr;
    t3_result_t rc = t3_header_parse(s->parse_buf, &hdr);
    if (rc != T3_OK) return rc;
    t3_version_action_t action;
    rc = t3_session_negotiate_version(s, hdr.version, &action);
    if (rc != T3_OK) return rc;
    if (action != T3_VERSION_OK) return T3_ERR_UNSUPPORTED_VERSION;
    s->command_type = hdr.command_type;
    return T3_OK;
}

/* ======================================================================
 * Session lifecycle (spec/wire-format.md §1)
 * ====================================================================== */

T3_API t3_result_t t3_session_new(const t3_secret_t *s, t3_session_t **out) {
    if (!out) return T3_ERR_INVALID_ARG;
    *out = NULL;
    if (!s) return T3_ERR_INVALID_ARG;
    t3_session_t *sess = (t3_session_t *)calloc(1, sizeof(*sess));
    if (!sess) return T3_ERR_INTERNAL;
    sess->secret        = s;
    sess->local_version = 0x01;
    sess->state         = T3_RETRY_OK;
    *out = sess;
    return T3_OK;
}

T3_API void t3_session_free(t3_session_t *sess) {
    if (!sess) return;
    memset(sess, 0, sizeof(*sess));
    free(sess);
}

T3_API t3_result_t t3_session_bind_callbacks(t3_session_t *sess,
                                             const t3_callbacks_t *cb) {
    if (!sess || !cb) return T3_ERR_INVALID_ARG;
    if (cb->struct_size == 0) return T3_ERR_INVALID_ARG;
    if (cb->struct_size > sizeof(t3_callbacks_t)) return T3_ERR_INVALID_ARG;
    if (!cb->lower_send || !cb->lower_recv || !cb->frame_send ||
        !cb->frame_recv || !cb->rng || !cb->monotonic_ns)
        return T3_ERR_INVALID_ARG;
    memcpy(&sess->cb, cb, cb->struct_size);
    if (cb->struct_size < sizeof(t3_callbacks_t))
        memset(((uint8_t *)&sess->cb) + cb->struct_size, 0,
               sizeof(t3_callbacks_t) - cb->struct_size);
    sess->callbacks_bound = 1;
    return T3_OK;
}
