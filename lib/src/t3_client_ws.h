/*
 * t3_client_ws.h — internal: WebSocket framing for client side.
 */
#ifndef T3_CLIENT_WS_H
#define T3_CLIENT_WS_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

/* Write a masked WS binary frame.
 * Returns 0 on success, -1 on error. */
int t3c_ws_frame_write(const uint8_t *payload, size_t payload_len,
                       uint8_t *out, size_t out_cap, size_t *out_len);

/* Read an unmasked WS binary frame.
 * Returns 0 on success (frame complete), 1 if more data needed, -1 on error.
 * payload_out points into buf (zero-copy). */
int t3c_ws_frame_read(const uint8_t *buf, size_t buf_len,
                      const uint8_t **payload_out, size_t *payload_len_out,
                      size_t *consumed_out);

/* Generate WS upgrade HTTP request.
 * ws_key_out receives the 24-byte base64 key (for validating 101 response). */
int t3c_ws_upgrade_request(const char *host, const char *path,
                           uint8_t *out, size_t out_cap, size_t *out_len,
                           uint8_t ws_key_out[24]);

#endif /* T3_CLIENT_WS_H */
