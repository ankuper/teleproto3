/*
 * t3_http_stream.c — HTTP chunked transfer framing helpers.
 *
 * This is the reference implementation of the Type3 protocol.
 * Normative behaviour is defined in spec/. Where they differ, spec/ wins.
 *
 * Source: spec/wire-format.md §2.2 + Story 12-3 + Epic 12.
 * Stability: lib-v0.2.0 ABI.
 */

#include "t3.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#ifndef SIZE_MAX
#define SIZE_MAX ((size_t)-1)
#endif

/*
 * t3__hex_to_size — decode lowercase/uppercase hex string to size_t.
 * Returns 1 on success, 0 on failure (invalid character or overflow).
 */
static int t3__hex_to_size(const uint8_t *hex, size_t hex_len, size_t *out) {
    if (hex_len == 0 || !out) {
        return 0;
    }
    size_t val = 0;
    for (size_t i = 0; i < hex_len; i++) {
        uint8_t c = hex[i];
        size_t digit = 0;
        if (c >= '0' && c <= '9') {
            digit = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            digit = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            digit = c - 'A' + 10;
        } else {
            return 0; /* Non-hex character */
        }
        if (val > (SIZE_MAX - digit) / 16) {
            return 0; /* Overflow */
        }
        val = val * 16 + digit;
    }
    *out = val;
    return 1;
}

/*
 * t3__size_to_hex — encode size_t to lowercase hex string, no leading zeros.
 * Returns number of chars written to buf.
 */
static size_t t3__size_to_hex(size_t val, char *buf) {
    char tmp[32];
    size_t i = 0;
    if (val == 0) {
        buf[0] = '0';
        return 1;
    }
    while (val > 0) {
        size_t rem = val % 16;
        tmp[i++] = (char)(rem < 10 ? '0' + rem : 'a' + rem - 10);
        val /= 16;
    }
    for (size_t j = 0; j < i; j++) {
        buf[j] = tmp[i - 1 - j];
    }
    return i;
}

T3_API t3_result_t t3_http_chunk_write(uint8_t *out, size_t out_cap, const uint8_t *data, size_t data_len, size_t *out_written) {
    if (!out || !out_written) {
        return T3_ERR_INVALID_ARG;
    }
    if (data_len == 0) {
        return T3_ERR_INVALID_ARG;
    }
    if (!data) {
        return T3_ERR_INVALID_ARG;
    }
    char hex[32];
    size_t hex_len = t3__size_to_hex(data_len, hex);
    /* required size = hex_len + CRLF + data_len + CRLF */
    size_t required = hex_len + 2 + data_len + 2;
    if (out_cap < required) {
        return T3_ERR_BUF_TOO_SMALL;
    }
    memcpy(out, hex, hex_len);
    out[hex_len] = '\r';
    out[hex_len + 1] = '\n';
    memcpy(out + hex_len + 2, data, data_len);
    out[hex_len + 2 + data_len] = '\r';
    out[hex_len + 2 + data_len + 1] = '\n';
    *out_written = required;
    return T3_OK;
}

T3_API t3_result_t t3_http_chunk_write_terminal(uint8_t *out, size_t out_cap, size_t *out_written) {
    if (!out || !out_written) {
        return T3_ERR_INVALID_ARG;
    }
    if (out_cap < 5) {
        return T3_ERR_BUF_TOO_SMALL;
    }
    memcpy(out, "0\r\n\r\n", 5);
    *out_written = 5;
    return T3_OK;
}

T3_API t3_result_t t3_http_chunk_parse(const uint8_t *buf, size_t buf_len, const uint8_t **out_data, size_t *out_data_len, size_t *out_consumed) {
    if (!buf || !out_data || !out_data_len || !out_consumed) {
        return T3_ERR_INVALID_ARG;
    }
    if (buf_len < 2) {
        return T3_ERR_BUF_TOO_SMALL;
    }
    /* Find the first CRLF that terminates the chunk-size line */
    const uint8_t *crlf1 = NULL;
    for (size_t i = 0; i < buf_len - 1; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n') {
            crlf1 = buf + i;
            break;
        }
    }
    if (!crlf1) {
        /* Heuristic: if we've seen >16 bytes without a CRLF it's malformed;
         * accommodate chunk extensions up to 255 bytes (spec §2.2). */
        if (buf_len > (16 + 2 + 255)) {
            return T3_ERR_MALFORMED;
        }
        return T3_ERR_BUF_TOO_SMALL;
    }
    size_t header_line_len = (size_t)(crlf1 - buf); /* bytes before first CRLF */
    if (header_line_len == 0) {
        return T3_ERR_MALFORMED;
    }
    /* Split chunk-size from optional chunk-extension at ';' (RFC 9112 §7.1.1).
     * Conforming Type3 emitters MUST NOT emit extensions, but receivers MUST
     * parse and skip them (spec §2.2). Extension length capped at 255 bytes. */
    size_t hex_len = header_line_len;
    const uint8_t *semi = NULL;
    for (size_t i = 0; i < header_line_len; i++) {
        if (buf[i] == ';') {
            semi = buf + i;
            hex_len = i;
            break;
        }
    }
    if (semi != NULL) {
        size_t ext_len = header_line_len - hex_len - 1; /* bytes after ';', before CRLF */
        if (ext_len > 255) {
            return T3_ERR_MALFORMED; /* extension too long — DoS cap (spec §2.2) */
        }
    }
    if (hex_len == 0) {
        return T3_ERR_MALFORMED;
    }
    size_t chunk_size = 0;
    if (!t3__hex_to_size(buf, hex_len, &chunk_size)) {
        return T3_ERR_MALFORMED;
    }
    /* Enforce max chunk-size (spec §2.2: 65535 bytes) */
    if (chunk_size > 65535u) {
        return T3_ERR_MALFORMED;
    }
    if (chunk_size == 0) {
        /* Terminal chunk: must be followed by an additional CRLF */
        size_t required_len = header_line_len + 2 + 2; /* line + CRLF + CRLF */
        if (buf_len < required_len) {
            return T3_ERR_BUF_TOO_SMALL;
        }
        if (crlf1[2] != '\r' || crlf1[3] != '\n') {
            return T3_ERR_MALFORMED;
        }
        *out_data_len = 0;
        *out_consumed = required_len;
        *out_data = NULL;
        return T3_OK;
    }
    size_t header_len = header_line_len + 2; /* line bytes + CRLF */
    if (chunk_size > SIZE_MAX - header_len - 2) {
        return T3_ERR_MALFORMED;
    }
    size_t required_len = header_len + chunk_size + 2;
    if (buf_len < required_len) {
        return T3_ERR_BUF_TOO_SMALL;
    }
    const uint8_t *data_start = buf + header_len;
    const uint8_t *crlf2 = data_start + chunk_size;
    if (crlf2[0] != '\r' || crlf2[1] != '\n') {
        return T3_ERR_MALFORMED;
    }
    *out_data = data_start;
    *out_data_len = chunk_size;
    *out_consumed = required_len;
    return T3_OK;
}
