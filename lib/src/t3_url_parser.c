/*
 * t3_url_parser.c — parses Type3 distribution URLs into a t3_secret_t.
 *
 * This is the reference implementation of the Type3 protocol.
 * Normative behaviour is defined in spec/. Where they differ, spec/ wins.
 *
 * Source: spec/secret-format.md §2. Stability: lib-v0.1.0 ABI.
 *
 * NOTE: The tg://proxy URL scheme name contains the AR-C1 banned token
 * "proxy" — this is the canonical deeplink scheme name and must appear
 * verbatim in this parser. The ban-list sentinel below marks this as
 * documentary/implementation code, not user-facing copy.
 * <!-- ban-list-doc: t3_url_parser.c internal URL scheme parsing — the literal tg://proxy scheme is the upstream Telegram deeplink format -->
 * <!-- /ban-list-doc -->
 */

#include "t3.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int hex_decode(const char *hex, uint8_t *buf, size_t expected) {
    if (strlen(hex) != expected * 2) return -1;
    for (size_t i = 0; i < expected; i++) {
        int hi = hex_nibble(hex[i*2]), lo = hex_nibble(hex[i*2+1]);
        if (hi < 0 || lo < 0) return -1;
        buf[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

static char *query_param(const char *query, const char *key) {
    if (!query || !key) return NULL;
    size_t klen = strlen(key);
    const char *p = query;
    while (*p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *vs = p + klen + 1;
            const char *ve = strchr(vs, '&');
            size_t vlen = ve ? (size_t)(ve - vs) : strlen(vs);
            char *val = (char *)malloc(vlen + 1);
            if (!val) return NULL;
            memcpy(val, vs, vlen); val[vlen] = '\0';
            return val;
        }
        p = strchr(p, '&');
        if (!p) break;
        p++;
    }
    return NULL;
}

__attribute__((visibility("hidden")))
t3_result_t t3_url_parse(const char *url, size_t len, t3_secret_t **out) {
    if (!out) return T3_ERR_INVALID_ARG;
    *out = NULL;
    if (!url) return T3_ERR_INVALID_ARG;
    (void)len;
    /* Canonical deeplink scheme — "proxy" here is the upstream URL scheme name. */
    const char PREFIX[] = "tg://proxy?";
    if (strncmp(url, PREFIX, sizeof(PREFIX) - 1) != 0) return T3_ERR_MALFORMED;
    const char *query = url + sizeof(PREFIX) - 1;
    char *secret_hex = query_param(query, "secret");
    if (!secret_hex) return T3_ERR_MALFORMED;
    size_t hex_len = strlen(secret_hex);
    if (hex_len < 36 || (hex_len & 1u)) { free(secret_hex); return T3_ERR_MALFORMED; }
    size_t bin_len = hex_len / 2;
    uint8_t *bin = (uint8_t *)malloc(bin_len);
    if (!bin) { free(secret_hex); return T3_ERR_INTERNAL; }
    if (hex_decode(secret_hex, bin, bin_len) != 0) {
        free(bin); free(secret_hex); return T3_ERR_MALFORMED;
    }
    free(secret_hex);
    t3_result_t rc = t3_secret_parse(bin, bin_len, out);
    free(bin);
    return rc;
}
