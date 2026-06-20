/*
 * t3_client.h — libteleproto3 client-side transport API.
 *
 * Provides a complete client transport abstraction:
 *   DNS resolve → TCP connect → TLS handshake → obfs2 init →
 *   AES-CTR encrypt/decrypt → WS/HTTP-chunked framing
 *
 * All clients (tdlib, tdesktop, telegram-android, telegram-ios)
 * use this API. The host sees only:
 *   create → get_fd (for poll) → write MTProto → read MTProto → destroy
 *
 * Stability: lib-v0.3.0 ABI. This header is public API.
 */

#ifndef TELEPROTO3_T3_CLIENT_H
#define TELEPROTO3_T3_CLIENT_H

#include "t3.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ====================================================================
 * Opaque client stream handle
 * ==================================================================== */
typedef struct t3_client_stream t3_client_stream;

/* ====================================================================
 * Connection state (pollable by host event loop)
 * ==================================================================== */
typedef enum {
    T3_CLIENT_STATE_CONNECTING  = 0,  /* TCP connect in progress */
    T3_CLIENT_STATE_TLS         = 1,  /* TLS handshake in progress */
    T3_CLIENT_STATE_HANDSHAKE   = 2,  /* obfs2 init sent, waiting for first data */
    T3_CLIENT_STATE_READY       = 3,  /* fully connected, wrap/unwrap available */
    T3_CLIENT_STATE_ERROR       = 4,  /* terminal error */
    T3_CLIENT_STATE_CLOSED      = 5   /* destroyed */
} t3_client_state_t;

/* ====================================================================
 * Create + connect
 *
 * endpoint_url:  "https://host:443/api/v1/data" (HTTP stream)
 *                "wss://host:443/ws/path"       (WebSocket)
 * secret:        16-byte proxy secret
 * dc_id:         target Telegram DC (signed, negative = media)
 *
 * On success: *out is a valid handle in CONNECTING state.
 * The caller must poll get_fd() and call t3_client_pump() on events.
 * ==================================================================== */
T3_API t3_result_t t3_client_create(
    const char     *endpoint_url,
    const uint8_t   secret[16],
    int16_t         dc_id,
    t3_client_stream **out
);

/* ====================================================================
 * Create + connect a SOCKS5-TUNNEL stream (story 9.2).
 *
 * Identical to t3_client_create EXCEPT the obfs2 init stamps the SOCKS5-tunnel
 * sentinel 0x5353 at header[60:62] in place of a dc_id (the canonical padded-
 * intermediate tag 0xdddddddd at [56:60] is kept). A tag-agnostic server
 * dispatches a tunnel connection when it decodes this sentinel (story 9.4);
 * the sentinel is the sole trigger.
 *
 * There is no dc_id parameter — the sentinel replaces it. Everything else
 * (TLS, framing, t3_client_pump/write/read/destroy) behaves exactly as for a
 * normal stream, so the tunnel is shape-indistinguishable from a chat stream
 * and inherits padding/jitter shaping. HTTP-stream endpoints only ("https://").
 *
 * On success: *out is a valid handle in CONNECTING state — pump it like any
 * other t3_client_stream. Does not affect normal (non-tunnel) callers.
 * ==================================================================== */
T3_API t3_result_t t3_client_create_tunnel(
    const char     *endpoint_url,
    const uint8_t   secret[16],
    t3_client_stream **out
);

/* ====================================================================
 * Pump — drive the state machine forward.
 *
 * Call after poll/epoll signals readability or writability on get_fd().
 * Returns T3_OK if progress was made, T3_ERR_BUF_TOO_SMALL if more
 * I/O is needed (keep polling), or an error code on failure.
 *
 * Once state transitions to READY, wrap/unwrap are available.
 * ==================================================================== */
T3_API t3_result_t t3_client_pump(t3_client_stream *s);

/* ====================================================================
 * Query state
 * ==================================================================== */
T3_API t3_client_state_t t3_client_get_state(const t3_client_stream *s);

/* Get the underlying fd for poll/epoll/kqueue registration.
 * Valid after create() and until destroy(). Returns -1 on error. */
T3_API int t3_client_get_fd(const t3_client_stream *s);

/* ====================================================================
 * Write MTProto payload → encrypted + framed output
 *
 * Encrypts plaintext with AES-256-CTR, frames it (WS binary or HTTP
 * chunked), and writes to the underlying TLS stream.
 *
 * Returns T3_OK on success. May return T3_ERR_BUF_TOO_SMALL if the
 * kernel send buffer is full (caller should retry after POLLOUT).
 *
 * Requires state == READY.
 * ==================================================================== */
T3_API t3_result_t t3_client_write(
    t3_client_stream *s,
    const uint8_t    *plaintext,
    size_t            len
);

/* ====================================================================
 * Read decrypted MTProto payload from incoming data
 *
 * Deframes (WS or HTTP chunked), decrypts AES-256-CTR, writes
 * plaintext to out_buf.
 *
 * *out_len = number of plaintext bytes written.
 * Returns T3_OK if data was read, T3_ERR_BUF_TOO_SMALL if no
 * complete frame is available yet (keep polling).
 *
 * Requires state == READY.
 * ==================================================================== */
T3_API t3_result_t t3_client_read(
    t3_client_stream *s,
    uint8_t          *out_buf,
    size_t            out_cap,
    size_t           *out_len
);

/* ====================================================================
 * Destroy — close fd, free all resources.
 * Safe to call in any state. NULL-safe.
 * ==================================================================== */
T3_API void t3_client_destroy(t3_client_stream *s);

/* ====================================================================
 * Error message for the last failed operation.
 * Returns a static string; valid until the next call on the same handle.
 * ==================================================================== */
T3_API const char *t3_client_last_error(const t3_client_stream *s);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* TELEPROTO3_T3_CLIENT_H */
