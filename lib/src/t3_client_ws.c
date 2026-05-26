/*
 * t3_client_ws.c — WebSocket framing for client side.
 *
 * Client → server: MASKED binary frames (RFC 6455 §5.3)
 * Server → client: UNMASKED binary frames
 */

#include "t3_client_ws.h"

#include <string.h>
#include <openssl/rand.h>

/* ── Write a masked binary frame ──────────────────────────────────── */

int t3c_ws_frame_write(const uint8_t *payload, size_t payload_len,
                       uint8_t *out, size_t out_cap, size_t *out_len) {
    if (!out || !out_len) return -1;

    /* Calculate frame size */
    size_t header_size = 2;  /* opcode + mask bit + len */
    if (payload_len >= 126 && payload_len <= 65535) {
        header_size += 2;    /* 16-bit extended length */
    } else if (payload_len > 65535) {
        header_size += 8;    /* 64-bit extended length */
    }
    header_size += 4;  /* masking key */

    size_t total = header_size + payload_len;
    if (out_cap < total) return -1;

    size_t pos = 0;

    /* Byte 0: FIN + opcode 0x02 (binary) */
    out[pos++] = 0x82;

    /* Byte 1: MASK bit (0x80) + payload length */
    if (payload_len < 126) {
        out[pos++] = 0x80 | (uint8_t)payload_len;
    } else if (payload_len <= 65535) {
        out[pos++] = 0x80 | 126;
        out[pos++] = (uint8_t)(payload_len >> 8);
        out[pos++] = (uint8_t)(payload_len & 0xff);
    } else {
        out[pos++] = 0x80 | 127;
        out[pos++] = 0;
        out[pos++] = 0;
        out[pos++] = 0;
        out[pos++] = 0;
        out[pos++] = (uint8_t)((payload_len >> 24) & 0xff);
        out[pos++] = (uint8_t)((payload_len >> 16) & 0xff);
        out[pos++] = (uint8_t)((payload_len >> 8) & 0xff);
        out[pos++] = (uint8_t)(payload_len & 0xff);
    }

    /* 4-byte masking key */
    uint8_t mask[4];
    RAND_bytes(mask, 4);
    memcpy(out + pos, mask, 4);
    pos += 4;

    /* Masked payload */
    for (size_t i = 0; i < payload_len; i++) {
        out[pos + i] = payload[i] ^ mask[i & 3];
    }
    pos += payload_len;

    *out_len = pos;
    return 0;
}

/* ── Read an unmasked binary frame ────────────────────────────────── */

int t3c_ws_frame_read(const uint8_t *buf, size_t buf_len,
                      const uint8_t **payload_out, size_t *payload_len_out,
                      size_t *consumed_out) {
    if (!buf || !payload_out || !payload_len_out || !consumed_out) return -1;
    if (buf_len < 2) return 1;  /* need more data */

    /* uint8_t opcode = buf[0] & 0x0f; */
    int masked = (buf[1] >> 7) & 1;
    size_t len = buf[1] & 0x7f;
    size_t header_size = 2;

    if (len == 126) {
        if (buf_len < 4) return 1;
        len = ((size_t)buf[2] << 8) | buf[3];
        header_size = 4;
    } else if (len == 127) {
        if (buf_len < 10) return 1;
        len = 0;
        for (int i = 0; i < 8; i++) {
            len = (len << 8) | buf[2 + i];
        }
        header_size = 10;
    }

    if (masked) {
        header_size += 4;  /* masking key */
    }

    size_t total = header_size + len;
    if (buf_len < total) return 1;  /* need more data */

    if (masked) {
        /* Server should not send masked frames, but handle it */
        /* For now, return error */
        return -1;
    }

    *payload_out = buf + header_size;
    *payload_len_out = len;
    *consumed_out = total;
    return 0;
}

/* ── Generate WS upgrade request ──────────────────────────────────── */

int t3c_ws_upgrade_request(const char *host, const char *path,
                           uint8_t *out, size_t out_cap, size_t *out_len,
                           uint8_t ws_key_out[24]) {
    if (!host || !path || !out || !out_len) return -1;

    /* Generate 16-byte nonce, base64 encode → 24 chars */
    uint8_t nonce[16];
    RAND_bytes(nonce, 16);

    /* Simple base64 encode */
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char key_b64[25];
    int o = 0;
    for (int i = 0; i < 16; i += 3) {
        uint32_t v = ((uint32_t)nonce[i] << 16);
        if (i + 1 < 16) v |= ((uint32_t)nonce[i+1] << 8);
        if (i + 2 < 16) v |= nonce[i+2];
        key_b64[o++] = b64[(v >> 18) & 0x3f];
        key_b64[o++] = b64[(v >> 12) & 0x3f];
        key_b64[o++] = (i + 1 < 16) ? b64[(v >> 6) & 0x3f] : '=';
        key_b64[o++] = (i + 2 < 16) ? b64[v & 0x3f] : '=';
    }
    key_b64[o] = '\0';

    if (ws_key_out) {
        memcpy(ws_key_out, key_b64, 24);
    }

    /* Build request */
    int n = snprintf((char *)out, out_cap,
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Connection: Upgrade\r\n"
        "Upgrade: websocket\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "\r\n",
        path, host, key_b64);

    if (n < 0 || (size_t)n >= out_cap) return -1;
    *out_len = (size_t)n;
    return 0;
}
