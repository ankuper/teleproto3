/*
 * t3_client_stream.c — Full client transport: connect → TLS → obfs2 → framing.
 *
 * State machine:
 *   CONNECTING → TLS → HANDSHAKE → READY
 *                                     ↕
 *                                   ERROR
 *
 * Non-blocking, fd-based. Host polls get_fd() and calls t3_client_pump().
 */

#include "t3_client.h"
#include "t3_client_crypto.h"
#include "t3_client_ws.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/rand.h>

/* ── Internal ring buffer ───────────────────────────────────────────── */
#define RING_CAP (256 * 1024)

typedef struct {
    uint8_t *data;
    size_t   head;   /* read position */
    size_t   tail;   /* write position */
    size_t   cap;
} ring_buf_t;

static void ring_init(ring_buf_t *r) {
    r->data = (uint8_t *)malloc(RING_CAP);
    r->head = 0;
    r->tail = 0;
    r->cap = RING_CAP;
}

static void ring_free(ring_buf_t *r) {
    free(r->data);
    r->data = NULL;
}

static size_t ring_used(const ring_buf_t *r) { return r->tail - r->head; }
static size_t ring_avail(const ring_buf_t *r) { return r->cap - ring_used(r); }

static void ring_compact(ring_buf_t *r) {
    if (r->head > 0 && ring_used(r) > 0) {
        memmove(r->data, r->data + r->head, ring_used(r));
        r->tail -= r->head;
        r->head = 0;
    } else if (ring_used(r) == 0) {
        r->head = r->tail = 0;
    }
}

static int ring_append(ring_buf_t *r, const uint8_t *data, size_t len) {
    if (ring_avail(r) < len) {
        ring_compact(r);
        if (ring_avail(r) < len) return -1;
    }
    memcpy(r->data + r->tail, data, len);
    r->tail += len;
    return 0;
}

static size_t ring_read_ptr(const ring_buf_t *r, const uint8_t **ptr) {
    *ptr = r->data + r->head;
    return ring_used(r);
}

static void ring_consume(ring_buf_t *r, size_t n) {
    r->head += n;
    if (r->head >= r->tail) {
        r->head = r->tail = 0;
    }
}

/* ── Transport mode ─────────────────────────────────────────────────── */
typedef enum {
    T3C_MODE_WS,
    T3C_MODE_HTTP
} t3c_mode_t;

/* ── Internal state ─────────────────────────────────────────────────── */
struct t3_client_stream {
    int                  fd;
    t3_client_state_t    state;
    t3c_mode_t           mode;

    /* TLS */
    SSL_CTX             *ssl_ctx;
    SSL                 *ssl;
    int                  tls_connected;

    /* Crypto */
    t3c_aes_ctx          encrypt_ctx;
    t3c_aes_ctx          decrypt_ctx;

    /* Config */
    uint8_t              secret[16];
    int16_t              dc_id;
    char                *host;       /* from endpoint URL (with port for Host header) */
    char                *sni_host;   /* hostname only (no port) for TLS SNI */
    char                *path;       /* URL path */
    int                  port;

    /* Buffers */
    ring_buf_t           in_ring;    /* raw bytes from TLS → deframe → decrypt */
    ring_buf_t           out_ring;   /* encrypt → frame → TLS write */
    ring_buf_t           plain_in;   /* decrypted MTProto payloads for caller */

    /* WS state */
    int                  ws_upgrade_sent;
    int                  ws_upgrade_done;
    uint8_t              ws_key[24];

    /* HTTP stream state */
    int                  http_headers_sent;
    int                  http_response_done;

    /* obfs2 */
    int                  obfs2_sent;

    /* Error */
    char                 last_error[256];
};

/* ── URL parsing ────────────────────────────────────────────────────── */
static int parse_endpoint(const char *url, char **sni_host, char **host_port,
                          char **path, int *port, t3c_mode_t *mode) {
    const char *p = url;
    if (strncmp(p, "https://", 8) == 0) {
        *mode = T3C_MODE_HTTP;
        p += 8;
    } else if (strncmp(p, "wss://", 6) == 0) {
        *mode = T3C_MODE_WS;
        p += 6;
    } else {
        return -1;
    }

    const char *slash = strchr(p, '/');
    size_t hp_len = slash ? (size_t)(slash - p) : strlen(p);

    *host_port = strndup(p, hp_len);
    *path = slash ? strdup(slash) : strdup("/");

    /* Extract hostname (no port) for SNI */
    const char *colon = NULL;
    for (size_t i = hp_len; i > 0; i--) {
        if (p[i-1] == ':') { colon = p + i - 1; break; }
    }
    if (colon) {
        *sni_host = strndup(p, (size_t)(colon - p));
        *port = atoi(colon + 1);
    } else {
        *sni_host = strndup(p, hp_len);
        *port = 443;
    }

    return 0;
}

/* ── Non-blocking TCP connect ───────────────────────────────────────── */
static int nb_connect(const char *host, int port) {
    struct addrinfo hints = {0};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    struct addrinfo *res = NULL;
    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) {
        return -1;
    }

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        return -1;
    }

    /* Non-blocking */
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    /* TCP_NODELAY */
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    int rc = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);

    if (rc < 0 && errno != EINPROGRESS) {
        close(fd);
        return -1;
    }

    return fd;
}

/* ── Create ─────────────────────────────────────────────────────────── */
T3_API t3_result_t t3_client_create(
    const char *endpoint_url,
    const uint8_t secret[16],
    int16_t dc_id,
    t3_client_stream **out)
{
    if (!endpoint_url || !secret || !out) return T3_ERR_INVALID_ARG;

    t3_client_stream *s = (t3_client_stream *)calloc(1, sizeof(*s));
    if (!s) return T3_ERR_INTERNAL;

    memcpy(s->secret, secret, 16);
    s->dc_id = dc_id;
    s->fd = -1;
    s->state = T3_CLIENT_STATE_CONNECTING;

    ring_init(&s->in_ring);
    ring_init(&s->out_ring);
    ring_init(&s->plain_in);

    if (parse_endpoint(endpoint_url, &s->sni_host, &s->host, &s->path,
                       &s->port, &s->mode) != 0) {
        snprintf(s->last_error, sizeof(s->last_error), "Invalid endpoint URL");
        t3_client_destroy(s);
        return T3_ERR_INVALID_ARG;
    }

    /* TCP connect (non-blocking) */
    s->fd = nb_connect(s->sni_host, s->port);
    if (s->fd < 0) {
        snprintf(s->last_error, sizeof(s->last_error), "TCP connect failed: %s", strerror(errno));
        t3_client_destroy(s);
        return T3_ERR_INTERNAL;
    }

    /* Prepare TLS */
    s->ssl_ctx = SSL_CTX_new(TLS_client_method());
    if (!s->ssl_ctx) {
        snprintf(s->last_error, sizeof(s->last_error), "SSL_CTX_new failed");
        t3_client_destroy(s);
        return T3_ERR_INTERNAL;
    }
    SSL_CTX_set_default_verify_paths(s->ssl_ctx);

    s->ssl = SSL_new(s->ssl_ctx);
    if (!s->ssl) {
        snprintf(s->last_error, sizeof(s->last_error), "SSL_new failed");
        t3_client_destroy(s);
        return T3_ERR_INTERNAL;
    }
    SSL_set_tlsext_host_name(s->ssl, s->sni_host);
    SSL_set_fd(s->ssl, s->fd);
    SSL_set_connect_state(s->ssl);

    *out = s;
    return T3_OK;
}

/* ── TLS pump ───────────────────────────────────────────────────────── */
static int pump_tls_handshake(t3_client_stream *s) {
    int rc = SSL_do_handshake(s->ssl);
    if (rc == 1) {
        s->tls_connected = 1;
        s->state = T3_CLIENT_STATE_HANDSHAKE;
        return 0;
    }
    int err = SSL_get_error(s->ssl, rc);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        return 1;  /* keep polling */
    }
    snprintf(s->last_error, sizeof(s->last_error), "TLS handshake failed: %s",
             ERR_reason_error_string(ERR_peek_last_error()));
    s->state = T3_CLIENT_STATE_ERROR;
    return -1;
}

/* ── SSL read/write helpers ─────────────────────────────────────────── */
static int ssl_flush_out(t3_client_stream *s) {
    while (ring_used(&s->out_ring) > 0) {
        const uint8_t *ptr;
        size_t avail = ring_read_ptr(&s->out_ring, &ptr);
        int n = SSL_write(s->ssl, ptr, (int)(avail > 16384 ? 16384 : avail));
        if (n > 0) {
            ring_consume(&s->out_ring, (size_t)n);
        } else {
            int err = SSL_get_error(s->ssl, n);
            if (err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_READ) {
                return 0;  /* try later */
            }
            return -1;  /* fatal */
        }
    }
    return 0;
}

static int ssl_read_in(t3_client_stream *s) {
    uint8_t tmp[16384];
    for (;;) {
        int n = SSL_read(s->ssl, tmp, sizeof(tmp));
        if (n > 0) {
            ring_append(&s->in_ring, tmp, (size_t)n);
        } else {
            int err = SSL_get_error(s->ssl, n);
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                return 0;
            }
            if (err == SSL_ERROR_ZERO_RETURN) {
                return -1;  /* peer closed */
            }
            return -1;
        }
    }
}

/* ── HTTP POST headers ──────────────────────────────────────────────── */
static int send_http_post_headers(t3_client_stream *s) {
    char hdrs[1024];
    int n = snprintf(hdrs, sizeof(hdrs),
        "POST %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Content-Type: application/octet-stream\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Connection: keep-alive\r\n"
        "\r\n",
        s->path, s->host);
    if (n < 0 || (size_t)n >= sizeof(hdrs)) return -1;
    ring_append(&s->out_ring, (uint8_t *)hdrs, (size_t)n);
    return 0;
}

/* ── Send obfs2 init ────────────────────────────────────────────────── */
static int send_obfs2_init(t3_client_stream *s) {
    uint8_t header[64];
    if (t3c_obfs2_generate_init(s->secret, s->dc_id, header,
                                 &s->encrypt_ctx, &s->decrypt_ctx) != 0) {
        snprintf(s->last_error, sizeof(s->last_error), "obfs2 init generation failed");
        return -1;
    }

    /* Frame the 64-byte header according to transport mode */
    if (s->mode == T3C_MODE_WS) {
        uint8_t framed[128];
        size_t framed_len;
        if (t3c_ws_frame_write(header, 64, framed, sizeof(framed), &framed_len) != 0) {
            return -1;
        }
        ring_append(&s->out_ring, framed, framed_len);
    } else {
        /* HTTP chunked */
        uint8_t chunk[128];
        size_t chunk_len;
        if (t3_http_chunk_write(chunk, sizeof(chunk), header, 64, &chunk_len) != T3_OK) {
            return -1;
        }
        ring_append(&s->out_ring, chunk, chunk_len);
    }

    s->obfs2_sent = 1;
    return 0;
}

/* ── Handle WS upgrade ──────────────────────────────────────────────── */
static int handle_ws_upgrade(t3_client_stream *s) {
    if (!s->ws_upgrade_sent) {
        uint8_t req[1024];
        size_t req_len;
        if (t3c_ws_upgrade_request(s->host, s->path, req, sizeof(req),
                                    &req_len, s->ws_key) != 0) {
            return -1;
        }
        ring_append(&s->out_ring, req, req_len);
        s->ws_upgrade_sent = 1;
        return 1;  /* need response */
    }

    /* Check for 101 response */
    const uint8_t *ptr;
    size_t avail = ring_read_ptr(&s->in_ring, &ptr);
    if (avail < 12) return 1;  /* need more */

    /* Find \r\n\r\n */
    const uint8_t *end = NULL;
    for (size_t i = 0; i + 3 < avail; i++) {
        if (ptr[i] == '\r' && ptr[i+1] == '\n' && ptr[i+2] == '\r' && ptr[i+3] == '\n') {
            end = ptr + i + 4;
            break;
        }
    }
    if (!end) {
        if (avail > 4096) return -1;  /* headers too long */
        return 1;  /* need more */
    }

    /* Validate 101 */
    if (avail < 12 || memcmp(ptr, "HTTP/1.1 101", 12) != 0) {
        snprintf(s->last_error, sizeof(s->last_error),
                 "WS upgrade failed: not 101");
        return -1;
    }

    ring_consume(&s->in_ring, (size_t)(end - ptr));
    s->ws_upgrade_done = 1;
    return 0;
}

/* ── Handle HTTP POST response ──────────────────────────────────────── */
/* handle_http_response removed — response header parsing is now inline in pump() */

/* ── Deframe + decrypt incoming data ────────────────────────────────── */
static int process_incoming(t3_client_stream *s) {
    for (;;) {
        const uint8_t *ptr;
        size_t avail = ring_read_ptr(&s->in_ring, &ptr);
        if (avail == 0) break;

        const uint8_t *payload;
        size_t payload_len;
        size_t consumed;

        if (s->mode == T3C_MODE_WS) {
            int rc = t3c_ws_frame_read(ptr, avail, &payload, &payload_len, &consumed);
            if (rc == 1) break;  /* need more */
            if (rc < 0) return -1;
        } else {
            t3_result_t rc = t3_http_chunk_parse(ptr, avail, &payload, &payload_len, &consumed);
            if (rc == T3_ERR_BUF_TOO_SMALL) break;
            if (rc != T3_OK) return -1;
            if (payload_len == 0) {
                /* Terminal chunk */
                ring_consume(&s->in_ring, consumed);
                continue;
            }
        }

        /* Decrypt */
        uint8_t *decrypted = (uint8_t *)malloc(payload_len);
        if (!decrypted) return -1;
        if (t3c_aes_crypt(&s->decrypt_ctx, payload, decrypted, payload_len) != 0) {
            free(decrypted);
            return -1;
        }

        /* Skip padding frames (first byte == 0xFE) */
        if (payload_len > 0 && decrypted[0] != T3_PADDING_MARKER) {
            ring_append(&s->plain_in, decrypted, payload_len);
        }
        free(decrypted);

        ring_consume(&s->in_ring, consumed);
    }
    return 0;
}

/* ── Pump ───────────────────────────────────────────────────────────── */
T3_API t3_result_t t3_client_pump(t3_client_stream *s) {
    if (!s) return T3_ERR_INVALID_ARG;
    if (s->state == T3_CLIENT_STATE_ERROR) return T3_ERR_INTERNAL;
    if (s->state == T3_CLIENT_STATE_CLOSED) return T3_ERR_INVALID_ARG;

    /* ── CONNECTING: check if TCP connect completed ─────────────── */
    if (s->state == T3_CLIENT_STATE_CONNECTING) {
        struct pollfd pfd = { .fd = s->fd, .events = POLLOUT };
        int rc = poll(&pfd, 1, 0);
        if (rc <= 0) return T3_ERR_BUF_TOO_SMALL;  /* not yet */
        int err = 0;
        socklen_t len = sizeof(err);
        getsockopt(s->fd, SOL_SOCKET, SO_ERROR, &err, &len);
        if (err != 0) {
            snprintf(s->last_error, sizeof(s->last_error), "TCP connect failed: %s", strerror(err));
            s->state = T3_CLIENT_STATE_ERROR;
            return T3_ERR_INTERNAL;
        }
        s->state = T3_CLIENT_STATE_TLS;
    }

    /* ── TLS: drive handshake ───────────────────────────────────── */
    if (s->state == T3_CLIENT_STATE_TLS) {
        int rc = pump_tls_handshake(s);
        if (rc != 0) {
            return rc < 0 ? T3_ERR_INTERNAL : T3_ERR_BUF_TOO_SMALL;
        }
        /* Falls through to HANDSHAKE */
    }

    /* ── HANDSHAKE: send headers + obfs2 init ───────────────────── */
    if (s->state == T3_CLIENT_STATE_HANDSHAKE) {
        if (s->mode == T3C_MODE_WS) {
            if (!s->ws_upgrade_done) {
                int rc = handle_ws_upgrade(s);
                if (rc != 0) {
                    ssl_flush_out(s);
                    ssl_read_in(s);
                    return rc < 0 ? T3_ERR_INTERNAL : T3_ERR_BUF_TOO_SMALL;
                }
            }
        } else {
            /* HTTP stream: send POST headers */
            if (!s->http_headers_sent) {
                send_http_post_headers(s);
                s->http_headers_sent = 1;
            }
        }

        /* Send obfs2 init */
        if (!s->obfs2_sent) {
            if (send_obfs2_init(s) != 0) {
                s->state = T3_CLIENT_STATE_ERROR;
                return T3_ERR_INTERNAL;
            }
        }

        /* Flush */
        ssl_flush_out(s);

        /* For HTTP: consume response headers if present.
           nginx may deliver headers in a separate TLS record that arrives
           before or after the chunked body. If first bytes are not "HTTP",
           body arrived first — skip header parsing. */
        if (s->mode == T3C_MODE_HTTP && !s->http_response_done) {
            ssl_flush_out(s);
            ssl_read_in(s);
            const uint8_t *ptr;
            size_t avail = ring_read_ptr(&s->in_ring, &ptr);
            if (avail == 0) {
                return T3_ERR_BUF_TOO_SMALL;
            }
            if (avail >= 4 && memcmp(ptr, "HTTP", 4) == 0) {
                const uint8_t *end = NULL;
                for (size_t i = 0; i + 3 < avail; i++) {
                    if (ptr[i]=='\r' && ptr[i+1]=='\n' && ptr[i+2]=='\r' && ptr[i+3]=='\n') {
                        end = ptr + i + 4;
                        break;
                    }
                }
                if (!end) {
                    if (avail > 8192) {
                        s->state = T3_CLIENT_STATE_ERROR;
                        return T3_ERR_INTERNAL;
                    }
                    return T3_ERR_BUF_TOO_SMALL;
                }
                ring_consume(&s->in_ring, (size_t)(end - ptr));
            }
            s->http_response_done = 1;
        }

        s->state = T3_CLIENT_STATE_READY;
    }

    /* ── READY: normal I/O ──────────────────────────────────────── */
    if (s->state == T3_CLIENT_STATE_READY) {
        ssl_read_in(s);
        process_incoming(s);
        ssl_flush_out(s);
    }

    return T3_OK;
}

/* ── Write ──────────────────────────────────────────────────────────── */
T3_API t3_result_t t3_client_write(t3_client_stream *s,
                                    const uint8_t *plaintext, size_t len) {
    if (!s || !plaintext || len == 0) return T3_ERR_INVALID_ARG;
    if (s->state != T3_CLIENT_STATE_READY) return T3_ERR_INVALID_ARG;

    /* Encrypt */
    uint8_t *encrypted = (uint8_t *)malloc(len);
    if (!encrypted) return T3_ERR_INTERNAL;
    if (t3c_aes_crypt(&s->encrypt_ctx, plaintext, encrypted, len) != 0) {
        free(encrypted);
        return T3_ERR_INTERNAL;
    }

    /* Frame */
    if (s->mode == T3C_MODE_WS) {
        size_t frame_cap = len + 14;  /* WS header max 14 bytes */
        uint8_t *frame = (uint8_t *)malloc(frame_cap);
        if (!frame) { free(encrypted); return T3_ERR_INTERNAL; }
        size_t frame_len;
        if (t3c_ws_frame_write(encrypted, len, frame, frame_cap, &frame_len) != 0) {
            free(frame); free(encrypted);
            return T3_ERR_INTERNAL;
        }
        ring_append(&s->out_ring, frame, frame_len);
        free(frame);
    } else {
        size_t chunk_cap = len + 32;
        uint8_t *chunk = (uint8_t *)malloc(chunk_cap);
        if (!chunk) { free(encrypted); return T3_ERR_INTERNAL; }
        size_t chunk_len;
        if (t3_http_chunk_write(chunk, chunk_cap, encrypted, len, &chunk_len) != T3_OK) {
            free(chunk); free(encrypted);
            return T3_ERR_INTERNAL;
        }
        ring_append(&s->out_ring, chunk, chunk_len);
        free(chunk);
    }
    free(encrypted);

    /* Try to flush immediately */
    ssl_flush_out(s);

    return T3_OK;
}

/* ── Read ───────────────────────────────────────────────────────────── */
T3_API t3_result_t t3_client_read(t3_client_stream *s,
                                   uint8_t *out_buf, size_t out_cap,
                                   size_t *out_len) {
    if (!s || !out_buf || !out_len) return T3_ERR_INVALID_ARG;
    if (s->state != T3_CLIENT_STATE_READY) return T3_ERR_INVALID_ARG;

    size_t avail = ring_used(&s->plain_in);
    if (avail == 0) {
        *out_len = 0;
        return T3_ERR_BUF_TOO_SMALL;  /* no data yet */
    }

    size_t to_read = avail < out_cap ? avail : out_cap;
    const uint8_t *ptr;
    ring_read_ptr(&s->plain_in, &ptr);
    memcpy(out_buf, ptr, to_read);
    ring_consume(&s->plain_in, to_read);

    *out_len = to_read;
    return T3_OK;
}

/* ── Query ──────────────────────────────────────────────────────────── */
T3_API t3_client_state_t t3_client_get_state(const t3_client_stream *s) {
    return s ? s->state : T3_CLIENT_STATE_ERROR;
}

T3_API int t3_client_get_fd(const t3_client_stream *s) {
    return s ? s->fd : -1;
}

T3_API const char *t3_client_last_error(const t3_client_stream *s) {
    return s ? s->last_error : "null handle";
}

/* ── Destroy ────────────────────────────────────────────────────────── */
T3_API void t3_client_destroy(t3_client_stream *s) {
    if (!s) return;

    t3c_aes_ctx_free(&s->encrypt_ctx);
    t3c_aes_ctx_free(&s->decrypt_ctx);

    if (s->ssl) {
        SSL_shutdown(s->ssl);
        SSL_free(s->ssl);
    }
    if (s->ssl_ctx) SSL_CTX_free(s->ssl_ctx);
    if (s->fd >= 0) close(s->fd);

    ring_free(&s->in_ring);
    ring_free(&s->out_ring);
    ring_free(&s->plain_in);

    free(s->host);
    free(s->sni_host);
    free(s->path);

    s->state = T3_CLIENT_STATE_CLOSED;
    free(s);
}
