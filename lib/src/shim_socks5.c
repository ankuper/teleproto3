/*
 * shim_socks5.c — localhost SOCKS5/CONNECT shim tunnelling through Type3 WSS.
 *
 * Story 9-1 (AC #1, #2). Build-flag-gated by T3_SHIM_SOCKS5=ON.
 *
 * // XXX 9-2: jitter randomization required before public-release widening
 * //          (CBR-tell verdict BLOCK 2026-05-09; see
 * //          _bmad-output/experiments/cbr-tell-2026-05-08/RESULT.md).
 *
 * Architecture: one accept thread per shim, one detached tunnel thread per
 * connection. Thread-per-connection is correct for ≤5 dogfood users.
 * TCP_NODELAY set on accepted client sockets per CBR-experiment lesson.
 *
 * Crypto (wire-format.md §4.2, canonical KDF from mtproxy3-legacy):
 *   enc_key = SHA256(rh[8:40] || sk)      enc_iv = rh[40:56]
 *   dec_key = SHA256(rev(rh[24:56]) || sk) dec_iv = rev(rh[8:24])
 * rh = 64-byte random header with magic tag at [56:60).
 * enc = client→server direction; dec = server→client direction.
 */

#include "t3_shim_socks5.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/ssl.h>

#define SHIM_BUF 16384

/* ================================================================
 * Crypto helpers
 * ================================================================ */

static void sha256_cat(const uint8_t *a, size_t al,
                       const uint8_t *b, size_t bl,
                       uint8_t out[32]) {
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    unsigned int mdlen = 32;
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(ctx, a, al);
    EVP_DigestUpdate(ctx, b, bl);
    EVP_DigestFinal_ex(ctx, out, &mdlen);
    EVP_MD_CTX_free(ctx);
}

/* SHA1 over `in` (used for RFC 6455 Sec-WebSocket-Accept derivation, P6). */
static void sha1_data(const uint8_t *in, size_t in_len, uint8_t out[20]) {
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    unsigned int mdlen = 20;
    EVP_DigestInit_ex(ctx, EVP_sha1(), NULL);
    EVP_DigestUpdate(ctx, in, in_len);
    EVP_DigestFinal_ex(ctx, out, &mdlen);
    EVP_MD_CTX_free(ctx);
}

/* Standard RFC 4648 base64 (no URL-safe). Writes `out_cap`-bounded NUL-terminated string.
 * Returns the length of the base64 string (excluding NUL), or -1 if buffer too small.
 * Used for Sec-WebSocket-Key generation (16 random bytes -> 24-char b64 with `==`)
 * and Sec-WebSocket-Accept verification (P5, P6). */
static int b64_encode(const uint8_t *in, size_t in_len, char *out, size_t out_cap) {
    static const char t[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t out_len = ((in_len + 2) / 3) * 4;
    if (out_len + 1 > out_cap) return -1;
    size_t i, j = 0;
    for (i = 0; i + 3 <= in_len; i += 3) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i+1] << 8) | in[i+2];
        out[j++] = t[(v >> 18) & 63];
        out[j++] = t[(v >> 12) & 63];
        out[j++] = t[(v >> 6)  & 63];
        out[j++] = t[ v        & 63];
    }
    size_t rem = in_len - i;
    if (rem == 1) {
        uint32_t v = ((uint32_t)in[i] << 16);
        out[j++] = t[(v >> 18) & 63];
        out[j++] = t[(v >> 12) & 63];
        out[j++] = '=';
        out[j++] = '=';
    } else if (rem == 2) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i+1] << 8);
        out[j++] = t[(v >> 18) & 63];
        out[j++] = t[(v >> 12) & 63];
        out[j++] = t[(v >> 6)  & 63];
        out[j++] = '=';
    }
    out[j] = 0;
    return (int)j;
}

/* Locate `name:` header (case-insensitive per RFC 7230) in HTTP response `data`.
 * Returns pointer to value (whitespace stripped), or NULL if absent. Used for
 * Sec-WebSocket-Accept verification (P6). */
static const char *find_header_icase(const char *data, size_t len, const char *name) {
    size_t nlen = strlen(name);
    for (size_t i = 0; i + nlen + 1 < len; i++) {
        if (i != 0 && data[i-1] != '\n') continue;
        int match = 1;
        for (size_t j = 0; j < nlen; j++) {
            char a = data[i + j], b = name[j];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            if (a != b) { match = 0; break; }
        }
        if (match && data[i + nlen] == ':') {
            const char *v = data + i + nlen + 1;
            const char *end = data + len;
            while (v < end && (*v == ' ' || *v == '\t')) v++;
            return v;
        }
    }
    return NULL;
}

static void derive_keys(const uint8_t rh[64], const uint8_t sk[16],
                        uint8_t ek[32], uint8_t ei[16],
                        uint8_t dk[32], uint8_t di[16]) {
    sha256_cat(rh + 8, 32, sk, 16, ek);
    memcpy(ei, rh + 40, 16);
    uint8_t rev[32];
    for (int i = 0; i < 32; i++) rev[i] = rh[55 - i];
    sha256_cat(rev, 32, sk, 16, dk);
    for (int i = 0; i < 16; i++) di[i] = rh[23 - i];
}

/* Build 64-byte random_header:
 *  - bytes [56:60): obfuscated2 magic tag (COMPACT = 0xEFEFEFEF)
 *  - bytes [60:62): SOCKS5-tunnel DC sentinel (0x5353) so the server
 *    detects this as a SOCKS5/CONNECT connection instead of routing to
 *    a Telegram DC.  Value 0x5353 is outside the valid DC range (1–5).
 * Both patches are in the clear after AES-CTR decryption by the server.
 * KDF input ranges: ek from rh[8:40], ei from rh[40:56] — neither
 * overlaps [56:64], so key derivation is unaffected by the patches. */
/* Returns 0 on success, -1 if OpenSSL CSPRNG fails (P3).
 * The previous body was a `for(;;)` loop with a tautological tag-verify
 * (the tag was forced via XOR-patch then re-checked); removed as dead code
 * per P9 — tag value 0xEFEFEFEF is deterministic by construction. */
static int make_random_header(const uint8_t sk[16], uint8_t rh[64]) {
    static const uint32_t MAGIC = 0xEFEFEFEFu;
    static const int16_t  SOCKS5_DC = 0x5353;

    if (RAND_bytes(rh, 64) != 1) return -1;  /* P3 */

    uint8_t ek[32], ei[16], dk[32], di[16];
    derive_keys(rh, sk, ek, ei, dk, di);

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return -1;
    EVP_EncryptInit_ex(ctx, EVP_aes_256_ctr(), NULL, ek, ei);
    uint8_t z[64] = {0}, ks[64];
    int n = 64;
    EVP_EncryptUpdate(ctx, ks, &n, z, 64);
    EVP_CIPHER_CTX_free(ctx);

    /* patch bytes [56:60) so server decrypts the magic tag 0xEFEFEFEF */
    uint8_t m[4];
    memcpy(m, &MAGIC, 4);
    for (int i = 0; i < 4; i++) rh[56 + i] = m[i] ^ ks[56 + i];

    /* patch bytes [60:62) so server decrypts SOCKS5_DC sentinel 0x5353 */
    uint8_t dc[2];
    memcpy(dc, &SOCKS5_DC, 2);
    for (int i = 0; i < 2; i++) rh[60 + i] = dc[i] ^ ks[60 + i];

    return 0;
}

/* ================================================================
 * WebSocket helpers (synchronous, blocking SSL I/O)
 * ================================================================ */

/* Send a client-masked binary WS frame over ssl. */
static int ws_send(SSL *ssl, const uint8_t *payload, size_t len) {
    uint8_t hdr[14];
    int hi = 0;
    hdr[hi++] = 0x82;  /* FIN + binary opcode */
    uint8_t mask[4];
    if (RAND_bytes(mask, 4) != 1) return -1;  /* P3 */
    if (len < 126) {
        hdr[hi++] = (uint8_t)(0x80 | len);
    } else if (len <= 65535) {
        hdr[hi++] = 0x80 | 126;
        hdr[hi++] = (uint8_t)(len >> 8);
        hdr[hi++] = (uint8_t)len;
    } else {
        hdr[hi++] = 0x80 | 127;
        for (int i = 7; i >= 0; i--) hdr[hi++] = (uint8_t)(len >> (i * 8));
    }
    memcpy(hdr + hi, mask, 4);
    hi += 4;
    if (SSL_write(ssl, hdr, hi) <= 0) return -1;
    /* Mask payload in chunks and send */
    uint8_t chunk[4096];
    for (size_t off = 0; off < len; ) {
        size_t n = len - off < sizeof(chunk) ? len - off : sizeof(chunk);
        for (size_t i = 0; i < n; i++) chunk[i] = payload[off + i] ^ mask[(off + i) % 4];
        if (SSL_write(ssl, chunk, (int)n) <= 0) return -1;
        off += n;
    }
    return 0;
}

/* Read one complete unmasked server WS frame, skip control/text frames.
 * Returns payload length (≥0), or -1 on error. */
static int ws_recv_frame(SSL *ssl, uint8_t *buf, int cap) {
    for (;;) {
        uint8_t h2[2];
        int r = SSL_read(ssl, h2, 2);
        if (r != 2) return -1;
        int opcode = h2[0] & 0x0F;
        size_t len = h2[1] & 0x7F;
        if (len == 126) {
            uint8_t e[2];
            if (SSL_read(ssl, e, 2) != 2) return -1;
            len = ((size_t)e[0] << 8) | e[1];
        } else if (len == 127) {
            uint8_t e[8];
            if (SSL_read(ssl, e, 8) != 8) return -1;
            len = 0;
            for (int i = 0; i < 8; i++) len = (len << 8) | e[i];
        }
        /* Discard control frames (PING/PONG/CLOSE) and text frames */
        if ((opcode & 0x08) || opcode == 0x01) {
            uint8_t sc[256];
            while (len > 0) {
                int rd = SSL_read(ssl, sc, (int)(len < sizeof(sc) ? len : sizeof(sc)));
                if (rd <= 0) return -1;
                len -= (size_t)rd;
            }
            continue;
        }
        if ((int)len > cap) return -1;
        int got = 0;
        while (got < (int)len) {
            r = SSL_read(ssl, buf + got, (int)len - got);
            if (r <= 0) return -1;
            got += r;
        }
        return got;
    }
}

/* ================================================================
 * Type3 connection
 * ================================================================ */

typedef struct {
    SSL            *ssl;
    EVP_CIPHER_CTX *enc;
    EVP_CIPHER_CTX *dec;
} t3c_t;

static int t3_open(SSL *ssl, const uint8_t sk[16], t3c_t *c) {
    uint8_t rh[64];
    if (make_random_header(sk, rh) < 0) return -1;  /* P3 */
    uint8_t ek[32], ei[16], dk[32], di[16];
    derive_keys(rh, sk, ek, ei, dk, di);

    /* 64-byte random_header in one WS frame.
     * Teleproxy obfuscated2 protocol expects exactly 64 bytes — no session
     * header prefix.  (The session header is a libteleproto3 spec concept;
     * the wire server only speaks raw obfuscated2.) */
    if (ws_send(ssl, rh, 64) < 0) return -1;

    c->ssl = ssl;
    c->enc = EVP_CIPHER_CTX_new();
    EVP_EncryptInit_ex(c->enc, EVP_aes_256_ctr(), NULL, ek, ei);
    /* advance enc by 64 bytes (keystream consumed by random_header transmission) */
    uint8_t out[64]; int n = 64;
    EVP_EncryptUpdate(c->enc, out, &n, rh, 64);

    c->dec = EVP_CIPHER_CTX_new();
    EVP_EncryptInit_ex(c->dec, EVP_aes_256_ctr(), NULL, dk, di);
    return 0;
}

static void t3_close(t3c_t *c) {
    EVP_CIPHER_CTX_free(c->enc);
    EVP_CIPHER_CTX_free(c->dec);
}

static int t3_send(t3c_t *c, const uint8_t *plain, int len) {
    uint8_t buf[SHIM_BUF];
    if (len > SHIM_BUF) return -1;
    int out = len;
    EVP_EncryptUpdate(c->enc, buf, &out, plain, len);
    return ws_send(c->ssl, buf, out);
}

static int t3_recv(t3c_t *c, uint8_t *plain, int cap) {
    uint8_t enc[SHIM_BUF];
    int n = ws_recv_frame(c->ssl, enc, cap < SHIM_BUF ? cap : SHIM_BUF);
    if (n <= 0) return n;
    int out = n;
    EVP_EncryptUpdate(c->dec, plain, &out, enc, n);
    return out;
}

/* ================================================================
 * TLS + WebSocket connect
 * ================================================================ */

static SSL *tls_connect_host(SSL_CTX *ctx, const char *host, uint16_t port) {
    struct addrinfo hints = {0}, *res;
    hints.ai_socktype = SOCK_STREAM;
    char ps[8];
    snprintf(ps, sizeof(ps), "%u", port);
    if (getaddrinfo(host, ps, &hints, &res) != 0) return NULL;
    int fd = socket(res->ai_family, SOCK_STREAM, 0);
    int ok = fd >= 0 && connect(fd, res->ai_addr, res->ai_addrlen) == 0;
    freeaddrinfo(res);
    if (!ok) { if (fd >= 0) close(fd); return NULL; }

    SSL *ssl = SSL_new(ctx);
    if (!ssl) { close(fd); return NULL; }  /* P7 */
    SSL_set_fd(ssl, fd);
    SSL_set_tlsext_host_name(ssl, host);
    /* P1: bind expected peer hostname to the verification params so that
     * SSL_VERIFY_PEER actually verifies SAN/CN match (not just chain-trust).
     * Without this, any CA-signed cert for any hostname would pass — MITM. */
    if (SSL_set1_host(ssl, host) != 1) {
        SSL_free(ssl); close(fd); return NULL;
    }
    if (SSL_connect(ssl) <= 0) {
        SSL_free(ssl); close(fd); return NULL;
    }
    return ssl;
}

/* RFC 6455 §4.1: client sends Sec-WebSocket-Key = base64(16 random bytes).
 * Server replies with Sec-WebSocket-Accept = base64(SHA1(key + GUID)) where
 * GUID is "258EAFA5-E914-47DA-95CA-C5AB0DC85B11".
 * Status line MUST be "HTTP/1.x 101 ...".
 *
 * Prior implementation (pre-9-1 review):
 *   - used 12-byte random + literal "==" appended → 18-char Sec-WebSocket-Key
 *     that is structurally invalid base64; strict servers reject (P5).
 *   - checked only `memmem(resp, off, "101", 3)` → any body containing "101"
 *     passed; never validated Sec-WebSocket-Accept (P6).
 * Both replaced below. */
static int ws_do_upgrade(SSL *ssl, const char *host, const char *path) {
    /* 16-byte random key per RFC 6455 §4.1 — 24-char base64 (P5). */
    uint8_t rk[16];
    if (RAND_bytes(rk, 16) != 1) return -1;  /* P3 */
    char key[32];
    if (b64_encode(rk, 16, key, sizeof(key)) < 0) return -1;

    char req[512];
    int rlen = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: Upgrade\r\n"
        "Upgrade: websocket\r\nSec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: %s\r\n\r\n",
        path, host, key);
    if (rlen < 0 || rlen >= (int)sizeof(req)) return -1;
    if (SSL_write(ssl, req, rlen) <= 0) return -1;

    /* Read until end of HTTP headers ("\r\n\r\n"). */
    char resp[2048];
    int off = 0;
    while (1) {
        if ((size_t)off + 1 >= sizeof(resp)) return -1;
        if (off > 0 && memmem(resp, (size_t)off, "\r\n\r\n", 4)) break;
        int r = SSL_read(ssl, resp + off, (int)(sizeof(resp) - 1 - off));
        if (r <= 0) return -1;
        off += r;
    }
    resp[off] = 0;

    /* P6: strict status-line check — must be "HTTP/1.x 101 ...". */
    if (off < 13 || memcmp(resp, "HTTP/1.", 7) != 0) return -1;
    if (resp[7] != '0' && resp[7] != '1') return -1;
    if (memcmp(resp + 8, " 101", 4) != 0) return -1;
    /* Char after "101" must be SP/HTAB/CR/LF — guards against "1010" etc. */
    char term = resp[12];
    if (term != ' ' && term != '\t' && term != '\r' && term != '\n') return -1;

    /* P6: verify Sec-WebSocket-Accept = base64(SHA1(key + GUID)). */
    static const char GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    char accept_input[32 + sizeof(GUID)];
    size_t kl = strlen(key);
    if (kl + sizeof(GUID) - 1 > sizeof(accept_input)) return -1;
    memcpy(accept_input, key, kl);
    memcpy(accept_input + kl, GUID, sizeof(GUID) - 1);
    uint8_t sha[20];
    sha1_data((uint8_t *)accept_input, kl + sizeof(GUID) - 1, sha);
    char expected[32];
    if (b64_encode(sha, 20, expected, sizeof(expected)) < 0) return -1;

    const char *hv = find_header_icase(resp, (size_t)off, "Sec-WebSocket-Accept");
    if (!hv) return -1;
    size_t el = strlen(expected);
    if (memcmp(hv, expected, el) != 0) return -1;
    char post = hv[el];
    if (post != '\r' && post != '\n' && post != ' ' && post != '\t') return -1;

    return 0;
}

/* ================================================================
 * SOCKS5 helpers
 * ================================================================ */

static int read_exact(int fd, uint8_t *buf, int len) {
    int got = 0;
    while (got < len) {
        ssize_t r = recv(fd, buf + got, (size_t)(len - got), 0);
        if (r <= 0) return -1;
        got += (int)r;
    }
    return 0;
}

/* ================================================================
 * Shim / tunnel structures
 * ================================================================ */

typedef struct {
    struct t3_shim *shim;
    int             client_fd;
} tunnel_arg_t;

struct t3_shim {
    int      listen_fd;
    uint16_t local_port;
    char     server_host[256];
    uint16_t server_port;
    char     ws_path[512];
    uint8_t  secret_key[16];
    SSL_CTX *ssl_ctx;
    pthread_t       accept_thread;
    atomic_bool     stopping;
    atomic_int      active_tunnels;
    atomic_uint_least64_t bytes_up;
    atomic_uint_least64_t bytes_down;
};

/* ================================================================
 * Tunnel thread
 * ================================================================ */

static void *tunnel_thread(void *arg) {
    tunnel_arg_t *ta = arg;
    struct t3_shim *sh = ta->shim;
    int cfd = ta->client_fd;
    free(ta);

    uint8_t buf[512];

    /* --- SOCKS5 greeting from tgcalls --- */
    if (read_exact(cfd, buf, 2) < 0 || buf[0] != 0x05) goto done;
    uint8_t nmeth = buf[1];
    if (nmeth > 0 && read_exact(cfd, buf, nmeth) < 0) goto done;
    /* Verify NO-AUTH (0x00) is in the offered method list */
    int has_no_auth = 0;
    for (uint8_t i = 0; i < nmeth; i++) {
        if (buf[i] == 0x00) { has_no_auth = 1; break; }
    }
    if (!has_no_auth) {
        uint8_t no_method[2] = {0x05, 0xFF};
        send(cfd, no_method, 2, 0);
        goto done;
    }
    uint8_t nauth[2] = {0x05, 0x00};
    send(cfd, nauth, 2, 0);

    /* --- SOCKS5 CONNECT request --- */
    if (read_exact(cfd, buf, 4) < 0) goto done;
    if (buf[0] != 0x05) goto done;
    if (buf[1] != 0x01) {
        /* BIND or UDP-ASSOCIATE: return REP=0x07 */
        uint8_t r[10] = {0x05,0x07,0x00,0x01,0,0,0,0,0,0};
        send(cfd, r, 10, 0);
        goto done;
    }
    uint8_t atyp = buf[3];
    uint8_t dst_addr[256], dst_port[2];
    int alen = 0;
    if (atyp == 0x01) {
        alen = 4;
    } else if (atyp == 0x03) {
        if (read_exact(cfd, buf, 1) < 0) goto done;
        alen = buf[0];
        if (alen == 0) goto done;  /* P8: reject empty-domain CONNECT */
    } else if (atyp == 0x04) {
        alen = 16;
    } else {
        goto done;
    }
    if (read_exact(cfd, dst_addr, alen) < 0) goto done;
    if (read_exact(cfd, dst_port, 2) < 0) goto done;

    {
        /* --- Connect to Type3 server --- */
        SSL *ssl = tls_connect_host(sh->ssl_ctx, sh->server_host, sh->server_port);
        if (!ssl) {
            uint8_t r[10] = {0x05,0x04,0x00,0x01,0,0,0,0,0,0};
            send(cfd, r, 10, 0);
            goto done;
        }
        if (ws_do_upgrade(ssl, sh->server_host, sh->ws_path) < 0) goto cleanup_ssl;

        t3c_t c;
        if (t3_open(ssl, sh->secret_key, &c) < 0) goto cleanup_ssl;

        /* --- SOCKS5 tunnel to server: NO-AUTH greeting --- */
        uint8_t greet[3] = {0x05, 0x01, 0x00};
        if (t3_send(&c, greet, 3) < 0) goto cleanup_t3;
        uint8_t sr[2];
        if (t3_recv(&c, sr, 2) != 2 || sr[0] != 0x05 || sr[1] != 0x00) goto cleanup_t3;

        /* --- SOCKS5 CONNECT to original target --- */
        uint8_t creq[4 + 1 + 255 + 2];
        int creq_len = 0;
        creq[creq_len++] = 0x05;
        creq[creq_len++] = 0x01;
        creq[creq_len++] = 0x00;
        creq[creq_len++] = atyp;
        if (atyp == 0x03) creq[creq_len++] = (uint8_t)alen;
        memcpy(creq + creq_len, dst_addr, (size_t)alen); creq_len += alen;
        memcpy(creq + creq_len, dst_port, 2);            creq_len += 2;
        if (t3_send(&c, creq, creq_len) < 0) goto cleanup_t3;

        /* Read SOCKS5 response (variable length, at least 10 bytes for IPv4) */
        uint8_t sresp[256];
        int rn = t3_recv(&c, sresp, sizeof(sresp));
        if (rn < 4 || sresp[1] != 0x00) goto cleanup_t3;

        /* --- Reply SOCKS5 success to tgcalls --- */
        uint8_t ok[10] = {0x05,0x00,0x00,0x01,0,0,0,0,0,0};
        send(cfd, ok, 10, 0);

        atomic_fetch_add(&sh->active_tunnels, 1);

        /* --- Bidirectional splice loop --- */
        uint8_t fbuf[SHIM_BUF], tbuf[SHIM_BUF];
        struct pollfd fds[2];
        fds[0].fd = cfd;              fds[0].events = POLLIN;
        fds[1].fd = SSL_get_fd(ssl);  fds[1].events = POLLIN;

        for (;;) {
            /* P4: cooperate with t3_shim_close shutdown — break out of splice
             * so the drain in t3_shim_close completes within bounded time. */
            if (atomic_load(&sh->stopping)) break;
            int pret = poll(fds, 2, 5000);
            if (pret < 0 && errno == EINTR) continue;
            if (pret < 0) break;
            if (fds[0].revents & (POLLHUP | POLLERR)) break;
            if (fds[1].revents & (POLLHUP | POLLERR)) break;

            if (fds[0].revents & POLLIN) {
                ssize_t n = recv(cfd, fbuf, sizeof(fbuf), 0);
                if (n <= 0) break;
                if (t3_send(&c, fbuf, (int)n) < 0) break;
                atomic_fetch_add(&sh->bytes_up, (uint_least64_t)n);
            }
            if (fds[1].revents & POLLIN) {
                int n = t3_recv(&c, tbuf, sizeof(tbuf));
                if (n <= 0) break;
                if (send(cfd, tbuf, (size_t)n, 0) < 0) break;
                atomic_fetch_add(&sh->bytes_down, (uint_least64_t)n);
            }
        }

        atomic_fetch_sub(&sh->active_tunnels, 1);
cleanup_t3:
        t3_close(&c);
cleanup_ssl:
        SSL_shutdown(ssl);
        int sfd = SSL_get_fd(ssl);
        SSL_free(ssl);
        close(sfd);
    }
done:
    close(cfd);
    return NULL;
}

/* ================================================================
 * Accept thread
 * ================================================================ */

static void *accept_thread(void *arg) {
    struct t3_shim *sh = arg;
    while (!atomic_load(&sh->stopping)) {
        struct sockaddr_storage sa;
        socklen_t slen = sizeof(sa);
        int cfd = accept(sh->listen_fd, (struct sockaddr *)&sa, &slen);
        if (cfd < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            break;
        }
        int one = 1;
        setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        tunnel_arg_t *ta = malloc(sizeof(*ta));
        if (!ta) { close(cfd); continue; }
        ta->shim = sh;
        ta->client_fd = cfd;
        pthread_t tid;
        if (pthread_create(&tid, NULL, tunnel_thread, ta) != 0) {
            close(cfd); free(ta);
        } else {
            pthread_detach(tid);
        }
    }
    return NULL;
}

/* ================================================================
 * Public API
 * ================================================================ */

T3_API t3_result_t t3_shim_open(
    const char  *server_host,
    uint16_t     server_port,
    const char  *ws_path,
    const char  *secret_hex,
    uint16_t     local_port,
    t3_shim_t  **out) {
    if (!server_host || !ws_path || !secret_hex || !out) return T3_ERR_INVALID_ARG;
    if (strlen(server_host) >= 256 || strlen(ws_path) >= 512) return T3_ERR_INVALID_ARG;
    if (strlen(secret_hex) < 34) return T3_ERR_INVALID_ARG;  /* "ff" + 32 hex digits min */

    /* Parse secret_hex: skip 0xff marker (2 hex chars), take next 32 hex chars (16 bytes) */
    uint8_t sk[16];
    for (int i = 0; i < 16; i++) {
        unsigned int byte;
        if (sscanf(secret_hex + 2 + i * 2, "%02x", &byte) != 1) return T3_ERR_INVALID_ARG;
        sk[i] = (uint8_t)byte;
    }

    t3_shim_t *sh = calloc(1, sizeof(*sh));
    if (!sh) return T3_ERR_INTERNAL;
    strncpy(sh->server_host, server_host, 255);
    sh->server_port = server_port;
    strncpy(sh->ws_path, ws_path, 511);
    memcpy(sh->secret_key, sk, 16);

    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) { free(sh); return T3_ERR_INTERNAL; }  /* P2 */
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
    SSL_CTX_set_default_verify_paths(ctx);
    sh->ssl_ctx = ctx;

    sh->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sh->listen_fd < 0) { SSL_CTX_free(ctx); free(sh); return T3_ERR_INTERNAL; }
    int reuse = 1;
    setsockopt(sh->listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in la = {0};
    la.sin_family = AF_INET;
    la.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    la.sin_port = htons(local_port);
    if (bind(sh->listen_fd, (struct sockaddr *)&la, sizeof(la)) < 0 ||
        listen(sh->listen_fd, 16) < 0) {
        close(sh->listen_fd); SSL_CTX_free(ctx); free(sh);
        return T3_ERR_INTERNAL;
    }
    socklen_t llen = sizeof(la);
    getsockname(sh->listen_fd, (struct sockaddr *)&la, &llen);
    sh->local_port = ntohs(la.sin_port);

    atomic_store(&sh->stopping, 0);
    if (pthread_create(&sh->accept_thread, NULL, accept_thread, sh) != 0) {
        /* P11: unwind on accept_thread spawn failure */
        close(sh->listen_fd); SSL_CTX_free(ctx); free(sh);
        return T3_ERR_INTERNAL;
    }
    *out = sh;
    return T3_OK;
}

T3_API void t3_shim_close(t3_shim_t *sh) {
    if (!sh) return;
    atomic_store(&sh->stopping, 1);
    close(sh->listen_fd);
    pthread_join(sh->accept_thread, NULL);

    /* P4: bounded-wait drain — detached tunnel threads must observe the
     * stopping flag and exit before we free the SSL_CTX they're still using.
     * Wait up to ~10s in 50ms ticks. If tunnels still haven't drained, leak
     * the shim rather than free-and-UAF the SSL_CTX out from under them. */
    for (int i = 0; i < 200; i++) {
        if (atomic_load(&sh->active_tunnels) == 0) break;
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 50 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    if (atomic_load(&sh->active_tunnels) != 0) {
        return;  /* leak rather than UAF */
    }

    SSL_CTX_free(sh->ssl_ctx);
    free(sh);
}

T3_API uint16_t t3_shim_local_port(const t3_shim_t *sh) {
    return sh ? sh->local_port : 0;
}

T3_API void t3_shim_stats(const t3_shim_t *sh,
                           int32_t  *out_active,
                           uint64_t *out_up,
                           uint64_t *out_down) {
    if (!sh) {
        if (out_active) *out_active = 0;
        if (out_up)     *out_up = 0;
        if (out_down)   *out_down = 0;
        return;
    }
    if (out_active) *out_active = (int32_t)atomic_load(&sh->active_tunnels);
    if (out_up)     *out_up     = (uint64_t)atomic_load(&sh->bytes_up);
    if (out_down)   *out_down   = (uint64_t)atomic_load(&sh->bytes_down);
}
