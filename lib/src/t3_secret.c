/*
 * t3_secret.c — implements the secret-format parsing / serialisation API.
 *
 * This is the reference implementation of the Type3 protocol.
 * Normative behaviour is defined in spec/. Where they differ, spec/ wins.
 *
 * Source: spec/secret-format.md §1–4 + amendments A-003 through A-006.
 * Stability: lib-v0.1.0 ABI (frozen by story 1.6).
 */

#include "t3.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

struct t3_secret {
    uint8_t  key[16];
    char    *domain;
    size_t   domain_len;
    char    *host;
    size_t   host_len;
    char    *path;
    size_t   path_len;
    /* A-010 split rule (spec/secret-format.md §5.2): when the parsed
     * domain contains literal '?' or '#' octets (a producer-side A-007
     * violation that the consumer parses leniently), the suffixes are
     * routed here so that `host` and `path` remain byte-clean. Both
     * fields are always allocated; empty string means "no such octet". */
    char    *query;
    size_t   query_len;
    char    *fragment;
    size_t   fragment_len;
};

/* UTF-8 + C0/DEL validation (spec/secret-format.md §2.1 rules 5–6). */
static int validate_utf8_and_controls(const uint8_t *buf, size_t len) {
    size_t i = 0;
    while (i < len) {
        uint8_t b = buf[i];
        /* Rule 6: C0 control (0x00–0x1F) and DEL (0x7F). */
        if (b <= 0x1F || b == 0x7F) return 0;
        if (b <= 0x7F) { i++; continue; }
        if (b <= 0xC1) return 0;  /* lone continuation / overlong lead */
        if (b <= 0xDF) {
            if (i + 1 >= len) return 0;
            if ((buf[i+1] & 0xC0u) != 0x80u) return 0;
            i += 2;
        } else if (b <= 0xEF) {
            if (i + 2 >= len) return 0;
            uint8_t b2 = buf[i+1], b3 = buf[i+2];
            if ((b2 & 0xC0u) != 0x80u || (b3 & 0xC0u) != 0x80u) return 0;
            if (b == 0xE0u && b2 < 0xA0u) return 0;  /* overlong */
            if (b == 0xEDu && b2 >= 0xA0u) return 0; /* surrogate */
            i += 3;
        } else if (b <= 0xF4) {
            if (i + 3 >= len) return 0;
            uint8_t b2 = buf[i+1], b3 = buf[i+2], b4 = buf[i+3];
            if ((b2 & 0xC0u) != 0x80u || (b3 & 0xC0u) != 0x80u || (b4 & 0xC0u) != 0x80u) return 0;
            if (b == 0xF0u && b2 < 0x90u) return 0;  /* overlong */
            if (b == 0xF4u && b2 > 0x8Fu) return 0;  /* > U+10FFFF */
            i += 4;
        } else { return 0; }
    }
    return 1;
}

T3_API t3_result_t t3_secret_parse(const uint8_t *buf, size_t len, t3_secret_t **out) {
    if (!out) return T3_ERR_INVALID_ARG;
    *out = NULL;
    if (!buf && len > 0) return T3_ERR_INVALID_ARG;
    if (len < 18) return T3_ERR_MALFORMED;
    if (buf[0] != 0xFFu) return T3_ERR_MALFORMED;
    size_t domain_len = len - 17;
    if (domain_len > 512) return T3_ERR_MALFORMED;
    const uint8_t *domain_buf = buf + 17;
    if (domain_buf[0] == 0x2Fu) return T3_ERR_MALFORMED;
    if (!validate_utf8_and_controls(domain_buf, domain_len)) return T3_ERR_MALFORMED;

    t3_secret_t *s = (t3_secret_t *)calloc(1, sizeof(*s));
    if (!s) return T3_ERR_INTERNAL;
    memcpy(s->key, buf + 1, 16);
    s->domain = (char *)malloc(domain_len + 1);
    if (!s->domain) { free(s); return T3_ERR_INTERNAL; }
    memcpy(s->domain, domain_buf, domain_len);
    s->domain[domain_len] = '\0';
    s->domain_len = domain_len;

    const uint8_t *slash = (const uint8_t *)memchr(domain_buf, 0x2F, domain_len);
    if (!slash) {
        s->host_len = domain_len;
        s->host = (char *)malloc(domain_len + 1);
        if (!s->host) { free(s->domain); free(s); return T3_ERR_INTERNAL; }
        memcpy(s->host, domain_buf, domain_len);
        s->host[domain_len] = '\0';
        s->path_len = 0;
        s->path = (char *)malloc(1);
        if (!s->path) { free(s->host); free(s->domain); free(s); return T3_ERR_INTERNAL; }
        s->path[0] = '\0';
    } else {
        size_t host_len = (size_t)(slash - domain_buf);
        size_t path_len = domain_len - host_len;
        s->host_len = host_len;
        s->host = (char *)malloc(host_len + 1);
        if (!s->host) { free(s->domain); free(s); return T3_ERR_INTERNAL; }
        memcpy(s->host, domain_buf, host_len);
        s->host[host_len] = '\0';
        s->path_len = path_len;
        s->path = (char *)malloc(path_len + 1);
        if (!s->path) { free(s->host); free(s->domain); free(s); return T3_ERR_INTERNAL; }
        memcpy(s->path, slash, path_len);
        s->path[path_len] = '\0';
    }

    /* A-010 split: extract literal '?' / '#' suffixes from host/path so
     * that downstream callers (DNS, HTTP Host: header, URL constructors)
     * receive byte-clean strings. The first '?' terminates host (if no
     * '/' had been seen, i.e. no path) or path; bytes between '?' and
     * the first subsequent '#' become query; bytes after '#' become
     * fragment. This is informational; the producer-side MUST-NOT-emit
     * rule from A-007 is enforced in t3_secret_serialise. */
    {
        const char *host_q = (const char *)memchr(s->host, '?', s->host_len);
        const char *host_f = (const char *)memchr(s->host, '#', s->host_len);
        const char *path_q = NULL;
        const char *path_f = NULL;
        if (s->path_len > 0) {
            path_q = (const char *)memchr(s->path, '?', s->path_len);
            path_f = (const char *)memchr(s->path, '#', s->path_len);
        }

        /* If '?' or '#' appears in host, anything after it (including the
         * existing path) belongs to query/fragment. Reconstruct from the
         * original domain to capture cross-boundary suffix correctly. */
        const char *first_special = NULL;
        if (host_q && host_f)      first_special = (host_q < host_f) ? host_q : host_f;
        else if (host_q)            first_special = host_q;
        else if (host_f)            first_special = host_f;

        if (first_special) {
            /* Recompute clean host/path/query/fragment from domain. */
            size_t clean_host_len = (size_t)(first_special - s->host);
            /* Trailing bytes from this point forward live in domain. */
            const char *suffix = s->domain + clean_host_len;
            size_t suffix_len = s->domain_len - clean_host_len;
            const char *q = (const char *)memchr(suffix, '?', suffix_len);
            const char *f = (const char *)memchr(suffix, '#', suffix_len);
            /* Truncate host. */
            s->host_len = clean_host_len;
            s->host[clean_host_len] = '\0';
            /* No path when '?'/'#' appears in host region (no '/' before it). */
            free(s->path);
            s->path = (char *)malloc(1);
            if (!s->path) { free(s->host); free(s->domain); free(s); return T3_ERR_INTERNAL; }
            s->path[0] = '\0';
            s->path_len = 0;
            /* Split suffix into query (if '?' present) and fragment (if '#' present). */
            const char *q_start = q ? q + 1 : NULL;
            const char *f_start = f ? f + 1 : NULL;
            size_t q_len = 0, f_len = 0;
            if (q_start && f_start && f > q) {
                q_len = (size_t)(f - q_start);
                f_len = (size_t)((suffix + suffix_len) - f_start);
            } else if (q_start) {
                q_len = (size_t)((suffix + suffix_len) - q_start);
            }
            if (f_start && (!q_start || f < q)) {
                f_len = (size_t)((suffix + suffix_len) - f_start);
                q_len = 0;
            }
            s->query = (char *)malloc(q_len + 1);
            if (!s->query) { free(s->path); free(s->host); free(s->domain); free(s); return T3_ERR_INTERNAL; }
            if (q_len > 0 && q_start) memcpy(s->query, q_start, q_len);
            s->query[q_len] = '\0';
            s->query_len = q_len;
            s->fragment = (char *)malloc(f_len + 1);
            if (!s->fragment) { free(s->query); free(s->path); free(s->host); free(s->domain); free(s); return T3_ERR_INTERNAL; }
            if (f_len > 0 && f_start) memcpy(s->fragment, f_start, f_len);
            s->fragment[f_len] = '\0';
            s->fragment_len = f_len;
        } else if (path_q || path_f) {
            /* '?' / '#' in path region — split path. */
            const char *p_special;
            if (path_q && path_f)   p_special = (path_q < path_f) ? path_q : path_f;
            else if (path_q)        p_special = path_q;
            else                    p_special = path_f;
            size_t clean_path_len = (size_t)(p_special - s->path);
            size_t path_consumed = clean_path_len;
            s->path[clean_path_len] = '\0';
            s->path_len = clean_path_len;
            /* Compute tail bounds in the original domain buffer. */
            size_t host_offset = s->host_len;
            size_t tail_total = s->domain_len - host_offset - path_consumed;
            const char *tail_start = s->domain + host_offset + path_consumed;
            const char *q = (const char *)memchr(tail_start, '?', tail_total);
            const char *f = (const char *)memchr(tail_start, '#', tail_total);
            const char *q_start = q ? q + 1 : NULL;
            const char *f_start = f ? f + 1 : NULL;
            size_t q_len = 0, f_len = 0;
            if (q_start && f_start && f > q) {
                q_len = (size_t)(f - q_start);
                f_len = (size_t)((tail_start + tail_total) - f_start);
            } else if (q_start) {
                q_len = (size_t)((tail_start + tail_total) - q_start);
            }
            if (f_start && (!q_start || f < q)) {
                f_len = (size_t)((tail_start + tail_total) - f_start);
                q_len = 0;
            }
            s->query = (char *)malloc(q_len + 1);
            if (!s->query) { free(s->path); free(s->host); free(s->domain); free(s); return T3_ERR_INTERNAL; }
            if (q_len > 0 && q_start) memcpy(s->query, q_start, q_len);
            s->query[q_len] = '\0';
            s->query_len = q_len;
            s->fragment = (char *)malloc(f_len + 1);
            if (!s->fragment) { free(s->query); free(s->path); free(s->host); free(s->domain); free(s); return T3_ERR_INTERNAL; }
            if (f_len > 0 && f_start) memcpy(s->fragment, f_start, f_len);
            s->fragment[f_len] = '\0';
            s->fragment_len = f_len;
        } else {
            /* No '?' / '#' anywhere — empty query/fragment. */
            s->query = (char *)malloc(1);
            if (!s->query) { free(s->path); free(s->host); free(s->domain); free(s); return T3_ERR_INTERNAL; }
            s->query[0] = '\0';
            s->query_len = 0;
            s->fragment = (char *)malloc(1);
            if (!s->fragment) { free(s->query); free(s->path); free(s->host); free(s->domain); free(s); return T3_ERR_INTERNAL; }
            s->fragment[0] = '\0';
            s->fragment_len = 0;
        }
    }

    *out = s;
    return T3_OK;
}

static void t3_internal_bzero(void *p, size_t n) {
    volatile uint8_t *vp = (volatile uint8_t *)p;
    while (n--) *vp++ = 0;
}

T3_API void t3_secret_free(t3_secret_t *s) {
    if (!s) return;
    t3_internal_bzero(s->key, sizeof(s->key));
    free(s->domain);
    free(s->host);
    free(s->path);
    free(s->query);
    free(s->fragment);
    memset(s, 0, sizeof(*s));
    free(s);
}

T3_API void t3_secret_zeroise(t3_secret_fields *fields) {
    if (!fields) return;
    t3_internal_bzero(fields->key, sizeof(fields->key));
}

T3_API t3_result_t t3_secret_validate_host(const char *host) {
    if (!host || host[0] == '\0') return T3_ERR_HOST_EMPTY;
    for (const char *p = host; *p; p++)
        if ((unsigned char)*p > 0x7Fu) return T3_ERR_HOST_NON_ASCII;
    size_t total = 0, label_len = 0;
    const char *p = host;
    while (*p) {
        char c = *p;
        if (++total > 253) return T3_ERR_HOST_INVALID;
        if (c == '.') {
            if (label_len == 0 || label_len > 63 || *(p-1) == '-') return T3_ERR_HOST_INVALID;
            label_len = 0;
        } else {
            if (label_len == 0 && c == '-') return T3_ERR_HOST_INVALID;
            if (!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'))
                return T3_ERR_HOST_INVALID;
            if (++label_len > 63) return T3_ERR_HOST_INVALID;
        }
        p++;
    }
    if (label_len == 0 || *(p-1) == '-') return T3_ERR_HOST_INVALID;
    return T3_OK;
}

T3_API t3_result_t t3_secret_validate_path(const char *path) {
    if (!path || path[0] == '\0') return T3_OK;
    if (path[0] != '/') return T3_ERR_PATH_MISSING_LEADING_SLASH;
    const char *p = path;
    while (*p) {
        unsigned char c = (unsigned char)*p;
        if (c > 0x7Fu) return T3_ERR_PATH_NON_ASCII;
        if (c == '%')  return T3_ERR_PATH_PERCENT_ENCODED;
        p++;
    }
    for (p = path; *p; p++)
        if (p[0] == '/' && p[1] == '/') return T3_ERR_PATH_EMPTY_SEGMENT;
    size_t len = (size_t)(p - path);
    if (len > 1 && path[len-1] == '/') return T3_ERR_PATH_TRAILING_SLASH;
    return T3_OK;
}

T3_API t3_result_t t3_secret_serialise(const t3_secret_fields *in, uint8_t *out, size_t *inout_len) {
    if (!in || !inout_len) return T3_ERR_INVALID_ARG;
    t3_result_t rc = t3_secret_validate_host(in->host);
    if (rc != T3_OK) return rc;
    rc = t3_secret_validate_path(in->path);
    if (rc != T3_OK) return rc;
    int all_zero = 1;
    for (int i = 0; i < 16; i++) if (in->key[i]) { all_zero = 0; break; }
    if (all_zero) return T3_ERR_KEY_INVALID;
    size_t host_len = strlen(in->host);
    size_t path_len = in->path ? strlen(in->path) : 0;
    size_t domain_len = host_len + path_len;
    if (domain_len > 512) return T3_ERR_INVALID_ARG;
    size_t required = 1 + 16 + domain_len;
    if (!out || *inout_len < required) { *inout_len = required; return T3_ERR_BUF_TOO_SMALL; }
    out[0] = 0xFFu;
    memcpy(out+1, in->key, 16);
    memcpy(out+17, in->host, host_len);
    if (path_len) memcpy(out+17+host_len, in->path, path_len);
    *inout_len = required;
    return T3_OK;
}
