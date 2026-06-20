/*
 * shim_socks5.c — localhost SOCKS5/CONNECT shim tunnelling through Type3
 * HTTP-stream (story 9.2; originally story 9-1).
 *
 * Story 9-1 built this as a hand-rolled WebSocket stack. Story 9.2 re-bases the
 * transport onto the canonical t3_client_* API (HTTP-stream ONLY — POST +
 * chunked, AES-CTR, obfs2 init). There is NO WebSocket upgrade and NO WS frame
 * anywhere on the wire: the WS upgrade was exactly the TSPU/DPI fingerprint the
 * project eliminated for chats. Build-flag-gated by T3_SHIM_SOCKS5=ON; requires
 * T3_BUILD_CLIENT (the t3_client transport it links against).
 *
 * Transport model (all TLS/crypto/framing delegated to t3_client_*):
 *   t3_client_create_tunnel(url, secret) -> pump to READY -> write/read -> destroy
 * The tunnel-mode obfs2 init stamps the SOCKS5 sentinel 0x5353 at header[60:62]
 * (in place of a dc_id) and keeps the canonical padded-intermediate tag
 * 0xdddddddd at [56:60]; a tag-agnostic server (story 9.4) dispatches a tunnel
 * connection on that sentinel — the sentinel is the sole trigger.
 *
 * Length-delimited inner framing (PINNED design):
 *   t3_client_write injects 0-15 bytes of intermediate padding and t3_client_read
 *   returns the payload PLUS that padding, so a raw byte stream cannot recover
 *   its exact length. Every tunnel message is therefore wrapped with a 2-byte
 *   little-endian length prefix INSIDE the t3_client payload:
 *       send: t3_client_write([len:2 LE][bytes])
 *       recv: t3_client_read -> [len:2 LE][bytes][t3 padding]; take exactly len
 *   This keeps t3_client's padding/jitter shaping ("inherits padding") while
 *   making the SOCKS5 byte stream byte-exact end to end. The story 9.4 server
 *   MUST strip the 2-byte LE prefix on receive and prepend it on send.
 *
 * TLS posture: t3_client uses SSL_VERIFY_NONE — TLS is camouflage; the obfs2
 * handshake + 16-byte secret authenticate the server (the Type3 threat model).
 * This shim deliberately delegates ALL TLS/crypto to t3_client; it keeps no
 * bespoke certificate verification of its own.
 *
 * Architecture: one accept thread per shim, one detached tunnel thread per
 * connection. Thread-per-connection is correct for <=5 dogfood users.
 * TCP_NODELAY set on accepted client sockets per CBR-experiment lesson.
 *
 * // XXX 9-3: timing jitter required before public-release widening
 * //          (CBR-tell verdict BLOCK 2026-05-09; see
 * //          _bmad-output/experiments/cbr-tell-2026-05-08/RESULT.md).
 */

#include "t3_shim_socks5.h"
#include "t3_client.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Platform layer: sockets, threads, atomics, monotonic time ──────────────
   The shim runs a localhost SOCKS5 listener with one thread per connection plus
   a handful of atomic counters. POSIX (Linux/macOS/Android) and Windows expose
   all of this differently; isolate it here so the body stays platform-neutral.
   The socket shims mirror net/t3_client_stream.c, which already builds clean on
   the Windows (MSVC /W4 /WX) CI. */
#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#  include <process.h>        /* _beginthreadex (CRT-safe thread spawn) */

#  define SHIM_CLOSESOCKET(fd)  closesocket((SOCKET)(fd))
#  define SHIM_POLL             WSAPoll
#  define SHIM_MSG_PEEK_NB      MSG_PEEK   /* the probed fd is already non-blocking */

static int shim_sock_intr(void) { return WSAGetLastError() == WSAEINTR; }

static int shim_sock_startup(void) {
    static int done = 0;
    if (!done) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return -1;
        done = 1;
    }
    return 0;
}
static int shim_sock_set_nonblock(int fd, int on) {
    u_long mode = on ? 1u : 0u;
    return ioctlsocket((SOCKET)fd, FIONBIO, &mode);
}

static int64_t now_ms(void) { return (int64_t)GetTickCount64(); }
static void shim_sleep_ms(int ms) { Sleep((DWORD)ms); }

/* Threads (Win32; _beginthreadex keeps the CRT happy vs raw CreateThread). */
typedef HANDLE shim_thread_t;
#  define SHIM_THREAD_FN(name, argp)  static unsigned __stdcall name(void *argp)
#  define SHIM_THREAD_RETURN          return 0u
static int shim_thread_create(shim_thread_t *t,
                              unsigned(__stdcall *fn)(void *), void *arg) {
    uintptr_t h = _beginthreadex(NULL, 0, fn, arg, 0, NULL);
    if (h == 0) return -1;
    *t = (HANDLE)h;
    return 0;
}
static void shim_thread_join(shim_thread_t t) {
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
}
static void shim_thread_detach(shim_thread_t t) { CloseHandle(t); }

/* Atomics (Interlocked — MSVC-safe regardless of <stdatomic.h> availability).
 * The pointer is cast to non-const volatile because Interlocked* take a
 * non-const pointer even for the read-only compare-exchange-as-load idiom, and
 * t3_shim_stats() reads through a `const t3_shim_t *` (would otherwise be C4090).
 * POSIX atomic_load accepts const, so only the Windows macros need the cast. */
typedef volatile LONG     shim_atomic_i32;
typedef volatile LONGLONG shim_atomic_i64;
#  define SHIM_A_STORE(p, v)  ((void)InterlockedExchange((volatile LONG *)(p), (LONG)(v)))
#  define SHIM_A_LOAD(p)      ((LONG)InterlockedCompareExchange((volatile LONG *)(p), 0, 0))
#  define SHIM_A_ADD(p, v)    ((void)InterlockedExchangeAdd((volatile LONG *)(p), (LONG)(v)))
#  define SHIM_A_LOAD64(p)    ((LONGLONG)InterlockedCompareExchange64((volatile LONGLONG *)(p), 0, 0))
#  define SHIM_A_ADD64(p, v)  ((void)InterlockedExchangeAdd64((volatile LONGLONG *)(p), (LONGLONG)(v)))
#else
#  include <arpa/inet.h>
#  include <fcntl.h>
#  include <netinet/tcp.h>
#  include <poll.h>
#  include <pthread.h>
#  include <stdatomic.h>
#  include <sys/socket.h>
#  include <time.h>
#  include <unistd.h>

#  define SHIM_CLOSESOCKET(fd)  close(fd)
#  define SHIM_POLL             poll
#  define SHIM_MSG_PEEK_NB      (MSG_PEEK | MSG_DONTWAIT)

static int shim_sock_intr(void) { return errno == EINTR; }
static int shim_sock_startup(void) { return 0; }
static int shim_sock_set_nonblock(int fd, int on) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    flags = on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return fcntl(fd, F_SETFL, flags);
}

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static void shim_sleep_ms(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

typedef pthread_t shim_thread_t;
#  define SHIM_THREAD_FN(name, argp)  static void *name(void *argp)
#  define SHIM_THREAD_RETURN          return NULL
static int shim_thread_create(shim_thread_t *t,
                              void *(*fn)(void *), void *arg) {
    return pthread_create(t, NULL, fn, arg) == 0 ? 0 : -1;
}
static void shim_thread_join(shim_thread_t t) { pthread_join(t, NULL); }
static void shim_thread_detach(shim_thread_t t) { pthread_detach(t); }

typedef atomic_int            shim_atomic_i32;
typedef atomic_uint_least64_t shim_atomic_i64;
#  define SHIM_A_STORE(p, v)  atomic_store((p), (v))
#  define SHIM_A_LOAD(p)      atomic_load((p))
#  define SHIM_A_ADD(p, v)    ((void)atomic_fetch_add((p), (v)))
#  define SHIM_A_LOAD64(p)    atomic_load((p))
#  define SHIM_A_ADD64(p, v)  ((void)atomic_fetch_add((p), (v)))
#endif

#include <openssl/crypto.h>   /* CRYPTO_memcmp — constant-time credential compare */
#include <openssl/rand.h>     /* RAND_bytes — per-shim SOCKS5 USER/PASS */

#define SHIM_BUF 16384

/* Bounded waits (ms). Generous for high-latency CDN paths; the local SOCKS5
 * client times out independently if these are exceeded. */
#define TUNNEL_CONNECT_TIMEOUT_MS   15000
#define TUNNEL_HANDSHAKE_TIMEOUT_MS 15000

/* ================================================================
 * Local SOCKS5 client socket I/O (blocking fd)
 * ================================================================ */

static int read_exact(int fd, uint8_t *buf, int len) {
    int got = 0;
    while (got < len) {
        int r = recv(fd, (char *)(buf + got), len - got, 0);
        if (r <= 0) return -1;
        got += r;
    }
    return 0;
}

static int send_all(int fd, const uint8_t *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int n = send(fd, (const char *)(buf + sent), (int)(len - sent), 0);
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

/* Send a 10-byte SOCKS5 reply (BND.ADDR = 0.0.0.0:0) with the given REP code. */
static void socks5_reply(int cfd, uint8_t rep) {
    uint8_t r[10] = {0x05, rep, 0x00, 0x01, 0, 0, 0, 0, 0, 0};
    (void)send(cfd, (const char *)r, (int)sizeof(r), 0);
}

/* ================================================================
 * Type3 tunnel transport (t3_client_*) + length-delimited framing
 * ================================================================ */

/* Definitive TCP-EOF probe. t3_client_pump does NOT flip state on a peer FIN,
 * so a half-closed server leaves the fd permanently readable; without this a
 * pump/read loop would spin. MSG_PEEK does not consume, and only a 0 return is
 * a definitive EOF (>0 = data/partial record, <0 = EAGAIN/not-yet-connected). */
static int fd_peer_closed(int fd) {
    char probe;
    int pk = recv(fd, &probe, 1, SHIM_MSG_PEEK_NB);
    return pk == 0;
}

/* Drive the client state machine to READY (or fail) within timeout_ms.
 * Returns 0 on READY, -1 on error/timeout. */
static int tunnel_wait_ready(t3_client_stream *st, int timeout_ms) {
    int fd = t3_client_get_fd(st);
    int64_t deadline = now_ms() + timeout_ms;
    for (;;) {
        t3_client_pump(st);
        t3_client_state_t s = t3_client_get_state(st);
        if (s == T3_CLIENT_STATE_READY) return 0;
        if (s == T3_CLIENT_STATE_ERROR || s == T3_CLIENT_STATE_CLOSED) return -1;
        if (s != T3_CLIENT_STATE_CONNECTING && fd_peer_closed(fd)) return -1;
        int64_t remaining = deadline - now_ms();
        if (remaining <= 0) return -1;
        struct pollfd pfd = { .fd = fd, .events = POLLIN | POLLOUT };
        int pr = SHIM_POLL(&pfd, 1, remaining > 200 ? 200 : (int)remaining);
        if (pr < 0) { if (shim_sock_intr()) continue; return -1; }
    }
}

/* Wrap `len` bytes with a 2-byte LE length prefix and write one tunnel message.
 * Returns 0 on success, -1 on error. len must be in [1, SHIM_BUF]. */
static int tunnel_send(t3_client_stream *st, const uint8_t *data, size_t len) {
    if (len == 0 || len > SHIM_BUF) return -1;
    uint8_t framed[2 + SHIM_BUF];
    framed[0] = (uint8_t)(len & 0xFF);
    framed[1] = (uint8_t)((len >> 8) & 0xFF);
    memcpy(framed + 2, data, len);
    if (t3_client_write(st, framed, len + 2) != T3_OK) return -1;
    t3_client_pump(st);  /* flush queued bytes toward the wire */
    return 0;
}

/* Read exactly one length-delimited tunnel message into out (cap bytes).
 * Returns payload length (>0) on success, 0 on timeout/no-data, -1 on
 * error/close. timeout_ms == 0 is a single non-blocking poll-drain attempt. */
static int tunnel_recv(t3_client_stream *st, uint8_t *out, size_t cap, int timeout_ms) {
    int fd = t3_client_get_fd(st);
    int64_t deadline = now_ms() + timeout_ms;
    uint8_t msg[2 + SHIM_BUF + 16];  /* inner len + payload + up to 15B padding */
    for (;;) {
        t3_client_pump(st);
        size_t got = 0;
        t3_result_t rc = t3_client_read(st, msg, sizeof(msg), &got);
        if (rc == T3_OK) {
            if (got < 2) return -1;
            size_t inner = (size_t)msg[0] | ((size_t)msg[1] << 8);
            if (inner > got - 2 || inner > cap) return -1;
            memcpy(out, msg + 2, inner);
            return (int)inner;
        }
        if (rc != T3_ERR_BUF_TOO_SMALL) return -1;
        t3_client_state_t s = t3_client_get_state(st);
        if (s == T3_CLIENT_STATE_ERROR || s == T3_CLIENT_STATE_CLOSED) return -1;
        if (fd_peer_closed(fd)) return -1;  /* peer FIN — pump won't surface it */
        int64_t remaining = deadline - now_ms();
        if (remaining <= 0) return 0;
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int pr = SHIM_POLL(&pfd, 1, remaining > 200 ? 200 : (int)remaining);
        if (pr < 0) { if (shim_sock_intr()) continue; return -1; }
    }
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
    char     ws_path[512];   /* URL path (legacy name); carried into the https:// URL */
    uint8_t  secret_key[16];
    /* D6: auto-generated SOCKS5 USER/PASS that gate the loopback listener.
     * 32 lower-case hex digits each + NUL. Generated once at t3_shim_open
     * and never rotated. NOT to be logged or persisted. */
    char     user[T3_SHIM_CRED_BUFLEN];
    char     pass[T3_SHIM_CRED_BUFLEN];
    shim_thread_t   accept_thread;
    shim_atomic_i32 stopping;
    shim_atomic_i32 active_tunnels;
    shim_atomic_i64 bytes_up;
    shim_atomic_i64 bytes_down;
};

/* ================================================================
 * Tunnel thread
 * ================================================================ */

SHIM_THREAD_FN(tunnel_thread, arg) {
    tunnel_arg_t *ta = arg;
    struct t3_shim *sh = ta->shim;
    int cfd = ta->client_fd;
    free(ta);

    t3_client_stream *st = NULL;
    int counted = 0;   /* whether active_tunnels was incremented */
    uint8_t buf[512];

    /* --- SOCKS5 greeting from the local client (D6: require USERNAME/PASSWORD) --- */
    if (read_exact(cfd, buf, 2) < 0 || buf[0] != 0x05) goto done;
    uint8_t nmeth = buf[1];
    if (nmeth > 0 && read_exact(cfd, buf, nmeth) < 0) goto done;
    /* D6: require RFC 1929 USERNAME/PASSWORD (method 0x02). NO-AUTH (0x00) is
     * deliberately refused — without it, any other process on the local
     * machine could connect to the loopback listener and tunnel arbitrary
     * traffic through our Type3 channel. */
    int has_userpass = 0;
    for (uint8_t i = 0; i < nmeth; i++) {
        if (buf[i] == 0x02) { has_userpass = 1; break; }
    }
    if (!has_userpass) {
        uint8_t no_method[2] = {0x05, 0xFF};
        send(cfd, (const char *)no_method, 2, 0);
        goto done;
    }
    {
        uint8_t method_reply[2] = {0x05, 0x02};
        send(cfd, (const char *)method_reply, 2, 0);
    }

    /* RFC 1929 sub-negotiation: 0x01 ULEN [UNAME] PLEN [PASSWD]
     * We require ULEN == PLEN == T3_SHIM_CRED_LEN (32 hex chars). */
    {
        uint8_t auth_hdr[2];
        if (read_exact(cfd, auth_hdr, 2) < 0) goto done;
        if (auth_hdr[0] != 0x01) goto done;  /* unsupported sub-version */
        int ulen = auth_hdr[1];
        if (ulen < 1 || ulen > 255) goto done;
        uint8_t user_recv[256];
        if (read_exact(cfd, user_recv, ulen) < 0) goto done;

        uint8_t plen_byte;
        if (read_exact(cfd, &plen_byte, 1) < 0) goto done;
        int plen = plen_byte;
        if (plen < 1 || plen > 255) goto done;
        uint8_t pass_recv[256];
        if (read_exact(cfd, pass_recv, plen) < 0) goto done;

        /* Constant-time compare. CRYPTO_memcmp returns 0 on equality. */
        int user_ok = (ulen == T3_SHIM_CRED_LEN)
            && (CRYPTO_memcmp(user_recv, sh->user, T3_SHIM_CRED_LEN) == 0);
        int pass_ok = (plen == T3_SHIM_CRED_LEN)
            && (CRYPTO_memcmp(pass_recv, sh->pass, T3_SHIM_CRED_LEN) == 0);

        uint8_t auth_reply[2] = { 0x01, (user_ok && pass_ok) ? 0x00 : 0xFF };
        send(cfd, (const char *)auth_reply, 2, 0);
        if (!user_ok || !pass_ok) goto done;
    }

    /* --- SOCKS5 CONNECT request --- */
    if (read_exact(cfd, buf, 4) < 0) goto done;
    if (buf[0] != 0x05) goto done;
    if (buf[1] != 0x01) {
        /* BIND or UDP-ASSOCIATE: REP=0x07 (command not supported). */
        socks5_reply(cfd, 0x07);
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

    /* --- Open the Type3 tunnel to the server (HTTP-stream via t3_client_*) --- */
    {
        char url[1024];
        int un = snprintf(url, sizeof(url), "https://%s:%u%s",
                          sh->server_host, (unsigned)sh->server_port, sh->ws_path);
        if (un < 0 || (size_t)un >= sizeof(url)) {
            socks5_reply(cfd, 0x01);  /* general failure */
            goto done;
        }
        if (t3_client_create_tunnel(url, sh->secret_key, &st) != T3_OK || !st) {
            socks5_reply(cfd, 0x04);  /* host unreachable */
            st = NULL;
            goto done;
        }
        if (tunnel_wait_ready(st, TUNNEL_CONNECT_TIMEOUT_MS) < 0) {
            socks5_reply(cfd, 0x04);
            goto done;
        }
    }

    /* --- Inner SOCKS5 handshake with the Type3 server (through the tunnel) --- */
    {
        /* NO-AUTH greeting to the server end of the tunnel. */
        uint8_t greet[3] = {0x05, 0x01, 0x00};
        if (tunnel_send(st, greet, 3) < 0) { socks5_reply(cfd, 0x04); goto done; }
        uint8_t sr[2];
        if (tunnel_recv(st, sr, sizeof(sr), TUNNEL_HANDSHAKE_TIMEOUT_MS) != 2
            || sr[0] != 0x05 || sr[1] != 0x00) {
            socks5_reply(cfd, 0x04);
            goto done;
        }

        /* Forward the original CONNECT target to the server. */
        uint8_t creq[4 + 1 + 255 + 2];
        int creq_len = 0;
        creq[creq_len++] = 0x05;
        creq[creq_len++] = 0x01;
        creq[creq_len++] = 0x00;
        creq[creq_len++] = atyp;
        if (atyp == 0x03) creq[creq_len++] = (uint8_t)alen;
        memcpy(creq + creq_len, dst_addr, (size_t)alen); creq_len += alen;
        memcpy(creq + creq_len, dst_port, 2);            creq_len += 2;
        if (tunnel_send(st, creq, (size_t)creq_len) < 0) {
            socks5_reply(cfd, 0x04);
            goto done;
        }

        uint8_t sresp[256];
        int rn = tunnel_recv(st, sresp, sizeof(sresp), TUNNEL_HANDSHAKE_TIMEOUT_MS);
        if (rn < 4 || sresp[1] != 0x00) {
            socks5_reply(cfd, 0x05);  /* connection refused by upstream */
            goto done;
        }
    }

    /* --- Reply SOCKS5 success to the local client --- */
    socks5_reply(cfd, 0x00);
    SHIM_A_ADD(&sh->active_tunnels, 1);
    counted = 1;

    /* --- Bidirectional splice loop --- */
    {
        int sfd = t3_client_get_fd(st);
        uint8_t fbuf[SHIM_BUF];
        struct pollfd fds[2];
        fds[0].fd = cfd;  fds[0].events = POLLIN;
        fds[1].fd = sfd;  fds[1].events = POLLIN;

        for (;;) {
            /* Cooperate with t3_shim_close shutdown so the close drain
             * completes within bounded time. */
            if (SHIM_A_LOAD(&sh->stopping)) break;

            int pret = SHIM_POLL(fds, 2, 1000);
            if (pret < 0) { if (shim_sock_intr()) continue; break; }

            /* Always pump: flush queued writes + ingest server data. */
            t3_client_pump(st);
            t3_client_state_t s = t3_client_get_state(st);
            if (s == T3_CLIENT_STATE_ERROR || s == T3_CLIENT_STATE_CLOSED) break;

            /* Drain tunnel -> local client (one length-delimited message at a time). */
            int drained_err = 0;
            for (;;) {
                uint8_t tbuf[SHIM_BUF];
                int n = tunnel_recv(st, tbuf, sizeof(tbuf), 0);
                if (n < 0) { drained_err = 1; break; }
                if (n == 0) break;
                if (send_all(cfd, tbuf, (size_t)n) < 0) { drained_err = 1; break; }
                SHIM_A_ADD64(&sh->bytes_down, n);
            }
            if (drained_err) break;

            /* Detect a peer FIN on the tunnel fd (pump does not surface EOF, so
             * a half-closed server would otherwise keep the fd readable and
             * spin the loop). */
            if ((fds[1].revents & POLLIN) && fd_peer_closed(sfd)) break;
            if (fds[1].revents & (POLLHUP | POLLERR)) break;

            /* Local client -> tunnel. */
            if (fds[0].revents & POLLIN) {
                int n = recv(cfd, (char *)fbuf, (int)sizeof(fbuf), 0);
                if (n <= 0) break;
                if (tunnel_send(st, fbuf, (size_t)n) < 0) break;
                SHIM_A_ADD64(&sh->bytes_up, n);
            }
            if (fds[0].revents & (POLLHUP | POLLERR)) break;
        }
    }

done:
    if (counted) SHIM_A_ADD(&sh->active_tunnels, -1);
    if (st) t3_client_destroy(st);
    SHIM_CLOSESOCKET(cfd);
    SHIM_THREAD_RETURN;
}

/* ================================================================
 * Accept thread
 * ================================================================ */

SHIM_THREAD_FN(accept_thread, arg) {
    struct t3_shim *sh = arg;
    while (!SHIM_A_LOAD(&sh->stopping)) {
        /* Poll the (non-blocking) listener with a short tick so we re-check
         * `stopping` and exit promptly on t3_shim_close. Do NOT rely on
         * close(listen_fd) waking a blocked accept(): that wakes accept on
         * BSD/macOS but is a no-op on Linux, where it would otherwise hang the
         * accept-thread join in t3_shim_close forever. */
        struct pollfd pfd = { .fd = sh->listen_fd, .events = POLLIN };
        int pr = SHIM_POLL(&pfd, 1, 500);
        if (pr <= 0) continue;  /* timeout / error -> re-check stopping */

        struct sockaddr_storage sa;
        socklen_t slen = (socklen_t)sizeof(sa);
        int cfd = (int)accept(sh->listen_fd, (struct sockaddr *)&sa, &slen);
        if (cfd < 0) continue;  /* EAGAIN / aborted connection -> retry */
        /* The listener is non-blocking; force the accepted socket BLOCKING so
         * the synchronous SOCKS5 handshake (read_exact) works on every platform
         * (Windows accepts inherit the listener's non-blocking mode). */
        shim_sock_set_nonblock(cfd, 0);
        int one = 1;
        setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
        tunnel_arg_t *ta = malloc(sizeof(*ta));
        if (!ta) { SHIM_CLOSESOCKET(cfd); continue; }
        ta->shim = sh;
        ta->client_fd = cfd;
        shim_thread_t tid;
        if (shim_thread_create(&tid, tunnel_thread, ta) != 0) {
            SHIM_CLOSESOCKET(cfd); free(ta);
        } else {
            shim_thread_detach(tid);
        }
    }
    SHIM_THREAD_RETURN;
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

    /* D6: auto-generate per-shim SOCKS5 USER/PASS (16 random bytes each,
     * hex-encoded to 32 chars). RAND_bytes failure -> abort open. */
    {
        uint8_t user_raw[16], pass_raw[16];
        if (RAND_bytes(user_raw, 16) != 1 || RAND_bytes(pass_raw, 16) != 1) {
            free(sh);
            return T3_ERR_INTERNAL;
        }
        static const char hexchars[] = "0123456789abcdef";
        for (int i = 0; i < 16; i++) {
            sh->user[i * 2]     = hexchars[user_raw[i] >> 4];
            sh->user[i * 2 + 1] = hexchars[user_raw[i] & 0xF];
            sh->pass[i * 2]     = hexchars[pass_raw[i] >> 4];
            sh->pass[i * 2 + 1] = hexchars[pass_raw[i] & 0xF];
        }
        sh->user[T3_SHIM_CRED_LEN] = '\0';
        sh->pass[T3_SHIM_CRED_LEN] = '\0';
    }

    if (shim_sock_startup() != 0) { free(sh); return T3_ERR_INTERNAL; }  /* WSAStartup on Windows */
    sh->listen_fd = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (sh->listen_fd < 0) { free(sh); return T3_ERR_INTERNAL; }
    int reuse = 1;
    setsockopt(sh->listen_fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));
    struct sockaddr_in la = {0};
    la.sin_family = AF_INET;
    la.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    la.sin_port = htons(local_port);
    if (bind(sh->listen_fd, (struct sockaddr *)&la, (socklen_t)sizeof(la)) < 0 ||
        listen(sh->listen_fd, 16) < 0) {
        SHIM_CLOSESOCKET(sh->listen_fd); free(sh);
        return T3_ERR_INTERNAL;
    }
    socklen_t llen = (socklen_t)sizeof(la);
    getsockname(sh->listen_fd, (struct sockaddr *)&la, &llen);
    sh->local_port = ntohs(la.sin_port);

    /* Non-blocking listener: the accept thread poll()s it with a timeout so it
     * observes `stopping` and exits cleanly without relying on close() waking a
     * blocked accept() (a Linux no-op). See accept_thread. */
    shim_sock_set_nonblock(sh->listen_fd, 1);

    SHIM_A_STORE(&sh->stopping, 0);
    if (shim_thread_create(&sh->accept_thread, accept_thread, sh) != 0) {
        /* P11: unwind on accept_thread spawn failure */
        SHIM_CLOSESOCKET(sh->listen_fd); free(sh);
        return T3_ERR_INTERNAL;
    }
    *out = sh;
    return T3_OK;
}

T3_API void t3_shim_close(t3_shim_t *sh) {
    if (!sh) return;
    SHIM_A_STORE(&sh->stopping, 1);
    SHIM_CLOSESOCKET(sh->listen_fd);
    shim_thread_join(sh->accept_thread);

    /* P4: bounded-wait drain — detached tunnel threads must observe the
     * stopping flag and exit before we free the shim they reference. Wait up
     * to ~4s in 50ms ticks (short enough to fit a typical caller shutdown wait;
     * tunnels notice `stopping` within one ~1s splice tick). If tunnels still
     * haven't drained, leak the shim rather than free-and-UAF it. */
    for (int i = 0; i < 80; i++) {
        if (SHIM_A_LOAD(&sh->active_tunnels) == 0) break;
        shim_sleep_ms(50);
    }
    if (SHIM_A_LOAD(&sh->active_tunnels) != 0) {
        return;  /* leak rather than UAF */
    }

    free(sh);
}

T3_API uint16_t t3_shim_local_port(const t3_shim_t *sh) {
    return sh ? sh->local_port : 0;
}

T3_API t3_result_t t3_shim_get_credentials(
    const t3_shim_t *sh,
    char *out_user, size_t user_len,
    char *out_pass, size_t pass_len) {
    if (!sh || !out_user || !out_pass) return T3_ERR_INVALID_ARG;
    if (user_len < T3_SHIM_CRED_BUFLEN || pass_len < T3_SHIM_CRED_BUFLEN) {
        return T3_ERR_INVALID_ARG;
    }
    memcpy(out_user, sh->user, T3_SHIM_CRED_BUFLEN);
    memcpy(out_pass, sh->pass, T3_SHIM_CRED_BUFLEN);
    return T3_OK;
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
    if (out_active) *out_active = (int32_t)SHIM_A_LOAD(&sh->active_tunnels);
    if (out_up)     *out_up     = (uint64_t)SHIM_A_LOAD64(&sh->bytes_up);
    if (out_down)   *out_down   = (uint64_t)SHIM_A_LOAD64(&sh->bytes_down);
}
