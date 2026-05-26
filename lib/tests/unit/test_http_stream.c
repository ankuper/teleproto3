/*
 * test_http_stream.c — unit tests for HTTP chunked transfer framing and transport mode.
 * Not subject to banner-discipline (tests/, not src/).
 * Returns 0 on pass / 1 on fail.
 */

#include "t3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef SIZE_MAX
#define SIZE_MAX ((size_t)-1)
#endif

/* Matching internal struct representation of t3_secret to test t3_secret_transport_mode */
struct t3_secret {
    uint8_t  key[16];
    char    *domain;
    size_t   domain_len;
    char    *host;
    size_t   host_len;
    char    *path;
    size_t   path_len;
    char    *query;
    size_t   query_len;
    char    *fragment;
    size_t   fragment_len;
};

static int test_write_happy(void) {
    printf("[test_write_happy] starting...\n");
    uint8_t out[1024];
    size_t out_written = 0;
    t3_result_t rc;

    /* Write 5 bytes "hello" */
    memset(out, 0xFF, sizeof(out));
    rc = t3_http_chunk_write(out, sizeof(out), (const uint8_t *)"hello", 5, &out_written);
    if (rc != T3_OK) {
        fprintf(stderr, "FAIL: chunk write hello rc=%d\n", rc);
        return 1;
    }
    if (out_written != 10) { /* "5\r\nhello\r\n" -> 1 + 2 + 5 + 2 = 10 */
        fprintf(stderr, "FAIL: hello out_written=%zu, expected 10\n", out_written);
        return 1;
    }
    if (memcmp(out, "5\r\nhello\r\n", 10) != 0) {
        fprintf(stderr, "FAIL: hello content mismatch\n");
        return 1;
    }

    /* Write 16 bytes "0123456789abcdef" */
    memset(out, 0xFF, sizeof(out));
    rc = t3_http_chunk_write(out, sizeof(out), (const uint8_t *)"0123456789abcdef", 16, &out_written);
    if (rc != T3_OK) {
        fprintf(stderr, "FAIL: chunk write 16 bytes rc=%d\n", rc);
        return 1;
    }
    if (out_written != 22) { /* "10\r\n0123456789abcdef\r\n" -> 2 + 2 + 16 + 2 = 22 */
        fprintf(stderr, "FAIL: 16 bytes out_written=%zu, expected 22\n", out_written);
        return 1;
    }
    if (memcmp(out, "10\r\n0123456789abcdef\r\n", 22) != 0) {
        fprintf(stderr, "FAIL: 16 bytes content mismatch\n");
        return 1;
    }

    printf("[test_write_happy] PASS\n");
    return 0;
}

static int test_write_errors(void) {
    printf("[test_write_errors] starting...\n");
    uint8_t out[10];
    size_t out_written = 0;
    t3_result_t rc;

    /* NULL guards */
    if (t3_http_chunk_write(NULL, 10, (const uint8_t *)"a", 1, &out_written) != T3_ERR_INVALID_ARG) return 1;
    if (t3_http_chunk_write(out, 10, (const uint8_t *)"a", 1, NULL) != T3_ERR_INVALID_ARG) return 1;
    if (t3_http_chunk_write(out, 10, NULL, 1, &out_written) != T3_ERR_INVALID_ARG) return 1;

    /* Zero data length is invalid for write (should use write_terminal) */
    if (t3_http_chunk_write(out, 10, (const uint8_t *)"a", 0, &out_written) != T3_ERR_INVALID_ARG) return 1;

    /* Buffer too small */
    /* "5\r\nhello\r\n" requires 10 bytes. Pass cap = 9. */
    rc = t3_http_chunk_write(out, 9, (const uint8_t *)"hello", 5, &out_written);
    if (rc != T3_ERR_BUF_TOO_SMALL) {
        fprintf(stderr, "FAIL: expected T3_ERR_BUF_TOO_SMALL, got %d\n", rc);
        return 1;
    }

    printf("[test_write_errors] PASS\n");
    return 0;
}

static int test_write_terminal(void) {
    printf("[test_write_terminal] starting...\n");
    uint8_t out[10];
    size_t out_written = 0;
    t3_result_t rc;

    /* NULL guards */
    if (t3_http_chunk_write_terminal(NULL, 10, &out_written) != T3_ERR_INVALID_ARG) return 1;
    if (t3_http_chunk_write_terminal(out, 10, NULL) != T3_ERR_INVALID_ARG) return 1;

    /* Buffer too small */
    rc = t3_http_chunk_write_terminal(out, 4, &out_written);
    if (rc != T3_ERR_BUF_TOO_SMALL) {
        fprintf(stderr, "FAIL: expected T3_ERR_BUF_TOO_SMALL, got %d\n", rc);
        return 1;
    }

    /* Happy path */
    memset(out, 0xFF, sizeof(out));
    rc = t3_http_chunk_write_terminal(out, 5, &out_written);
    if (rc != T3_OK || out_written != 5 || memcmp(out, "0\r\n\r\n", 5) != 0) {
        fprintf(stderr, "FAIL: terminal chunk write happy path failed\n");
        return 1;
    }

    printf("[test_write_terminal] PASS\n");
    return 0;
}

static int test_parse_happy(void) {
    printf("[test_parse_happy] starting...\n");
    uint8_t buf[1024];
    size_t written = 0;
    t3_result_t rc;

    /* Write and parse normal chunk */
    rc = t3_http_chunk_write(buf, sizeof(buf), (const uint8_t *)"test_data", 9, &written);
    if (rc != T3_OK) return 1;

    const uint8_t *parsed_data = NULL;
    size_t parsed_len = 0;
    size_t consumed = 0;

    rc = t3_http_chunk_parse(buf, written, &parsed_data, &parsed_len, &consumed);
    if (rc != T3_OK) {
        fprintf(stderr, "FAIL: parse normal chunk rc=%d\n", rc);
        return 1;
    }
    if (parsed_len != 9 || consumed != written) {
        fprintf(stderr, "FAIL: parse normal chunk len/consumed mismatch\n");
        return 1;
    }
    if (parsed_data != buf + 3) { /* "9\r\n" is 3 bytes */
        fprintf(stderr, "FAIL: zero-copy pointer mismatch\n");
        return 1;
    }
    if (memcmp(parsed_data, "test_data", 9) != 0) return 1;

    /* Write and parse terminal chunk */
    rc = t3_http_chunk_write_terminal(buf, sizeof(buf), &written);
    if (rc != T3_OK) return 1;

    rc = t3_http_chunk_parse(buf, written, &parsed_data, &parsed_len, &consumed);
    if (rc != T3_OK) {
        fprintf(stderr, "FAIL: parse terminal chunk rc=%d\n", rc);
        return 1;
    }
    if (parsed_len != 0 || consumed != 5 || parsed_data != NULL) {
        fprintf(stderr, "FAIL: parse terminal chunk validation failed\n");
        return 1;
    }

    /* Multiple chunks in buffer */
    uint8_t multibuf[128];
    size_t w1, w2, w3;
    rc = t3_http_chunk_write(multibuf, sizeof(multibuf), (const uint8_t *)"abc", 3, &w1);
    if (rc != T3_OK) return 1;
    rc = t3_http_chunk_write(multibuf + w1, sizeof(multibuf) - w1, (const uint8_t *)"defgh", 5, &w2);
    if (rc != T3_OK) return 1;
    rc = t3_http_chunk_write_terminal(multibuf + w1 + w2, sizeof(multibuf) - w1 - w2, &w3);
    if (rc != T3_OK) return 1;

    size_t total_len = w1 + w2 + w3;
    size_t offset = 0;

    /* Chunk 1 */
    rc = t3_http_chunk_parse(multibuf + offset, total_len - offset, &parsed_data, &parsed_len, &consumed);
    if (rc != T3_OK || parsed_len != 3 || memcmp(parsed_data, "abc", 3) != 0) return 1;
    offset += consumed;

    /* Chunk 2 */
    rc = t3_http_chunk_parse(multibuf + offset, total_len - offset, &parsed_data, &parsed_len, &consumed);
    if (rc != T3_OK || parsed_len != 5 || memcmp(parsed_data, "defgh", 5) != 0) return 1;
    offset += consumed;

    /* Chunk 3 (Terminal) */
    rc = t3_http_chunk_parse(multibuf + offset, total_len - offset, &parsed_data, &parsed_len, &consumed);
    if (rc != T3_OK || parsed_len != 0 || parsed_data != NULL) return 1;
    offset += consumed;

    if (offset != total_len) {
        fprintf(stderr, "FAIL: total consumed offset mismatch\n");
        return 1;
    }

    printf("[test_parse_happy] PASS\n");
    return 0;
}

static int test_parse_errors(void) {
    printf("[test_parse_errors] starting...\n");
    t3_result_t rc;
    const uint8_t *parsed_data = NULL;
    size_t parsed_len = 0;
    size_t consumed = 0;

    /* NULL guards */
    if (t3_http_chunk_parse(NULL, 10, &parsed_data, &parsed_len, &consumed) != T3_ERR_INVALID_ARG) return 1;
    if (t3_http_chunk_parse((const uint8_t *)"5\r\n", 3, NULL, &parsed_len, &consumed) != T3_ERR_INVALID_ARG) return 1;
    if (t3_http_chunk_parse((const uint8_t *)"5\r\n", 3, &parsed_data, NULL, &consumed) != T3_ERR_INVALID_ARG) return 1;
    if (t3_http_chunk_parse((const uint8_t *)"5\r\n", 3, &parsed_data, &parsed_len, NULL) != T3_ERR_INVALID_ARG) return 1;

    /* Truncated header (missing CRLF) */
    rc = t3_http_chunk_parse((const uint8_t *)"1a", 2, &parsed_data, &parsed_len, &consumed);
    if (rc != T3_ERR_BUF_TOO_SMALL) {
        fprintf(stderr, "FAIL: expected BUF_TOO_SMALL for truncated header, got %d\n", rc);
        return 1;
    }

    /* Malformed header: invalid hex character */
    rc = t3_http_chunk_parse((const uint8_t *)"1z\r\n", 4, &parsed_data, &parsed_len, &consumed);
    if (rc != T3_ERR_MALFORMED) {
        fprintf(stderr, "FAIL: expected MALFORMED for invalid hex, got %d\n", rc);
        return 1;
    }

    /* Malformed header: empty size */
    rc = t3_http_chunk_parse((const uint8_t *)"\r\n", 2, &parsed_data, &parsed_len, &consumed);
    if (rc != T3_ERR_MALFORMED) {
        fprintf(stderr, "FAIL: expected MALFORMED for empty header, got %d\n", rc);
        return 1;
    }

    /* Truncated data body */
    /* Chunk says size is 5, but we only supply 2 bytes of data + CRLF */
    rc = t3_http_chunk_parse((const uint8_t *)"5\r\nab\r\n", 7, &parsed_data, &parsed_len, &consumed);
    if (rc != T3_ERR_BUF_TOO_SMALL) {
        fprintf(stderr, "FAIL: expected BUF_TOO_SMALL for truncated data, got %d\n", rc);
        return 1;
    }

    /* Missing trailing CRLF after data */
    /* Chunk says size is 5, we supply 5 bytes, but trail is "xx" instead of "\r\n" */
    rc = t3_http_chunk_parse((const uint8_t *)"5\r\nhelloxx", 10, &parsed_data, &parsed_len, &consumed);
    if (rc != T3_ERR_MALFORMED) {
        fprintf(stderr, "FAIL: expected MALFORMED for missing trailing CRLF, got %d\n", rc);
        return 1;
    }

    /* Hex overflow check */
    /* 17 hex characters -> larger than 64-bit size_t */
    rc = t3_http_chunk_parse((const uint8_t *)"fffffffffffffffff\r\n", 19, &parsed_data, &parsed_len, &consumed);
    if (rc != T3_ERR_MALFORMED) {
        fprintf(stderr, "FAIL: expected MALFORMED for hex overflow, got %d\n", rc);
        return 1;
    }

    /* Size overflow check (chunk_size + header_len + 2 overflows size_t) */
    /* Size = SIZE_MAX - 1 */
    /* We construct a header "ffffffffffffffff\r\n" (on 64-bit systems) */
    /* Let's construct a hex representation of SIZE_MAX */
    char ovf_buf[64];
    memset(ovf_buf, 'f', 16);
    memcpy(ovf_buf + 16, "\r\n", 2);
    rc = t3_http_chunk_parse((const uint8_t *)ovf_buf, 18, &parsed_data, &parsed_len, &consumed);
    if (rc != T3_ERR_MALFORMED && rc != T3_ERR_BUF_TOO_SMALL) {
        fprintf(stderr, "FAIL: expected MALFORMED or BUF_TOO_SMALL for huge chunk size, got %d\n", rc);
        return 1;
    }

    printf("[test_parse_errors] PASS\n");
    return 0;
}

static int test_transport_mode(void) {
    printf("[test_transport_mode] starting...\n");

    /* Test NULL secret */
    if (t3_secret_transport_mode(NULL) != T3_TRANSPORT_WS) return 1;

    struct t3_secret sec;
    memset(&sec, 0, sizeof(sec));

    /* Test NULL query */
    if (t3_secret_transport_mode(&sec) != T3_TRANSPORT_WS) return 1;

    /* Test empty query */
    sec.query = "";
    if (t3_secret_transport_mode(&sec) != T3_TRANSPORT_WS) return 1;

    /* Test t=1 */
    sec.query = "t=1";
    if (t3_secret_transport_mode(&sec) != T3_TRANSPORT_HTTP_STREAM) return 1;

    /* Test t=0 */
    sec.query = "t=0";
    if (t3_secret_transport_mode(&sec) != T3_TRANSPORT_WS) return 1;

    /* Test t=2 (unrecognized value) */
    sec.query = "t=2";
    if (t3_secret_transport_mode(&sec) != T3_TRANSPORT_WS) return 1;

    /* Test multiple query parameters starting with t=1 */
    sec.query = "t=1&foo=bar";
    if (t3_secret_transport_mode(&sec) != T3_TRANSPORT_HTTP_STREAM) return 1;

    /* Test multiple query parameters starting with t=0 */
    sec.query = "t=0&foo=bar";
    if (t3_secret_transport_mode(&sec) != T3_TRANSPORT_WS) return 1;

    /* Test t=1 not at the start */
    sec.query = "foo=bar&t=1";
    if (t3_secret_transport_mode(&sec) != T3_TRANSPORT_WS) return 1;

    /* Test prefix check "t=12" (should be unrecognized/WS since not followed by end or &) */
    sec.query = "t=12";
    if (t3_secret_transport_mode(&sec) != T3_TRANSPORT_WS) return 1;

    printf("[test_transport_mode] PASS\n");
    return 0;
}

int main(void) {
    int rc = 0;
    rc |= test_write_happy();
    rc |= test_write_errors();
    rc |= test_write_terminal();
    rc |= test_parse_happy();
    rc |= test_parse_errors();
    rc |= test_transport_mode();
    return rc;
}
