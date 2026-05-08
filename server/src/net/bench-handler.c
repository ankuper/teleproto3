/*
 * bench-handler.c — Type3 bench echo handler implementation (Story 1a-2).
 *
 * Pure logic module: receives one chunk of post-handshake payload at a time,
 * dispatches by sub-mode, and writes output (if any) into c->out_buf.
 * The dispatch glue (net-type3-dispatch.c) drains c->out_buf into the real
 * connection's c->out rwm and resets out_len.
 *
 * Whole TU is a no-op when TELEPROTO3_BENCH is undefined — bench symbols
 * are absent from release artefacts (verified by tests/test_bench_ci_release.sh,
 * AC #4).
 */

#ifdef TELEPROTO3_BENCH

#define _GNU_SOURCE 1

#include "bench-handler.h"

#include <stdint.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>

/* ---------------------------------------------------------------- *
 * Runtime gate (Task 2).                                            *
 * Mutated by mtproto-proxy.c when --enable-bench-handler is given.  *
 * ---------------------------------------------------------------- */
int g_bench_handler_enabled = 0;

/* ---------------------------------------------------------------- *
 * Aggregate stats (Task 6). Atomic to allow concurrent NET threads. *
 * ---------------------------------------------------------------- */
static _Atomic uint64_t g_bench_sink_bytes   = 0;
static _Atomic uint64_t g_bench_echo_bytes   = 0;
static _Atomic uint64_t g_bench_source_bytes = 0;

bench_stats_t bench_handler_get_stats(void) {
    bench_stats_t s;
    s.sink_bytes   = atomic_load_explicit(&g_bench_sink_bytes,   memory_order_relaxed);
    s.echo_bytes   = atomic_load_explicit(&g_bench_echo_bytes,   memory_order_relaxed);
    s.source_bytes = atomic_load_explicit(&g_bench_source_bytes, memory_order_relaxed);
    return s;
}

/* ---------------------------------------------------------------- *
 * /dev/urandom-backed CSPRNG. Same /dev/urandom-fd-cache pattern    *
 * as net-type3-dispatch.c::dispatch_rng — both files are dev-only   *
 * tooling and don't pull libteleproto3-internal headers.            *
 *                                                                    *
 * C12: bounded EAGAIN retry (up to 3 attempts); on persistent error  *
 * or unexpected EOF the fd is closed and re-opened on the next call. *
 * Returns 0 on success, -1 on unrecoverable error.                   *
 * ---------------------------------------------------------------- */
static int bench_csprng(uint8_t *buf, size_t len) {
    static int urandom_fd = -1;
    if (urandom_fd < 0) {
        urandom_fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
        if (urandom_fd < 0) return -1;
    }
    size_t total = 0;
    /* P2 (R2): EAGAIN retry budget is per-call, NOT reset on partial success.
     * The previous "reset on every n>0 read" allowed unbounded oscillation
     * between EAGAIN and 1-byte success — claim of "bounded retry" was
     * materially false. Track total EAGAINs across the whole call instead. */
    int eagain_total = 0;
    while (total < len) {
        ssize_t n = read(urandom_fd, buf + total, len - total);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN && eagain_total < 3) { eagain_total++; continue; }
            /* Persistent error or EAGAIN exhausted — close fd, reopen next call. */
            close(urandom_fd);
            urandom_fd = -1;
            return -1;
        }
        if (n == 0) {
            /* Unexpected EOF on /dev/urandom — reopen next call. */
            close(urandom_fd);
            urandom_fd = -1;
            return -1;
        }
        total += (size_t)n;
    }
    return 0;
}

/* ---------------------------------------------------------------- *
 * bench_handler_init — AC #1, #5                                    *
 * Allocates state on the caller's bench_conn_t. Both gates required.*
 * ---------------------------------------------------------------- */
int bench_handler_init(bench_conn_t *c) {
    if (!c) return -1;
    if (!g_bench_handler_enabled) return -1;
    memset(&c->bench_state, 0, sizeof(c->bench_state));
    c->bench_state.initialised = 1;
    c->out_len = 0;
    return 0;
}

/* ---------------------------------------------------------------- *
 * Output helpers — write into c->out_buf, respect BENCH_OUT_CAP.    *
 * Returns bytes written (may be < n if buffer is near full).        *
 * ---------------------------------------------------------------- */
static int out_push(bench_conn_t *c, const void *src, int n) {
    int avail = BENCH_OUT_CAP - c->out_len;
    if (avail <= 0) return 0;
    if (n > avail) n = avail;
    memcpy(c->out_buf + c->out_len, src, (size_t)n);
    c->out_len += n;
    return n;
}

/* ---------------------------------------------------------------- *
 * bench_handler_recv — AC #2, #3, #6                                *
 *                                                                    *
 * Sub-mode dispatch on first decrypted byte:                         *
 *   0x01 SINK   — discard incoming bytes, no response                *
 *   0x02 ECHO   — echo input bytes back identically                  *
 *   0x03 SOURCE — read 4-byte LE length N, emit N CSPRNG bytes       *
 *   other      — return -1003 (caller closes with WS code 1003)      *
 *                                                                    *
 * Hot-path safety (AC #3): no blocking calls beyond /dev/urandom     *
 * read (Linux kernel-internal, non-blocking after first read).       *
 * ---------------------------------------------------------------- */
int bench_handler_recv(bench_conn_t *c, const void *data, int len) {
    if (!c) return -1;
    if (!c->bench_state.initialised) return -1;
    if (!g_bench_handler_enabled)    return -1;
    if (len < 0) return -1;
    /* C6: len==0 is a valid "continue SOURCE emission" signal when the mode
     * byte has been consumed, the length field is complete, and bytes remain
     * to emit.  For all other states it means there is nothing to do. */
    if (len == 0) {
        if (c->bench_state.mode_byte_seen &&
            c->bench_state.mode == BENCH_MODE_SOURCE &&
            c->bench_state.source_len_pending == 0 &&
            c->bench_state.source_remaining > 0) {
            /* fall through to SOURCE emission */
        } else {
            return 0;
        }
    }

    const uint8_t *p = (const uint8_t *)data;
    int payload_consumed = 0;  /* bytes counted toward bytes_processed */

    /* Consume mode byte on first call. */
    if (!c->bench_state.mode_byte_seen) {
        uint8_t m = p[0];
        if (m != BENCH_MODE_SINK && m != BENCH_MODE_ECHO && m != BENCH_MODE_SOURCE) {
            /* AC #2: invalid sub-mode → caller closes with WS 1003. */
            return -1003;
        }
        c->bench_state.mode = m;
        c->bench_state.mode_byte_seen = 1;
        c->bench_state.source_len_pending = (m == BENCH_MODE_SOURCE) ? 4 : 0;
        p   += 1;
        len -= 1;
    }

    switch (c->bench_state.mode) {
    case BENCH_MODE_SINK: {
        /* Read into discard, just count. */
        c->bench_state.bytes_processed += (uint64_t)len;
        atomic_fetch_add_explicit(&g_bench_sink_bytes, (uint64_t)len,
                                  memory_order_relaxed);
        payload_consumed = len;
        break;
    }

    case BENCH_MODE_ECHO: {
        int written = out_push(c, p, len);
        c->bench_state.bytes_processed += (uint64_t)written;
        atomic_fetch_add_explicit(&g_bench_echo_bytes, (uint64_t)written,
                                  memory_order_relaxed);
        /* Caller will drain out_buf and call again with any unconsumed
         * tail; we report only the bytes we actually placed in out_buf. */
        payload_consumed = written;
        break;
    }

    case BENCH_MODE_SOURCE: {
        /* Drain pending length bytes first. */
        while (c->bench_state.source_len_pending > 0 && len > 0) {
            int idx = 4 - c->bench_state.source_len_pending;
            c->bench_state.source_len_buf[idx] = *p++;
            c->bench_state.source_len_pending--;
            len--;
        }
        /* Length not fully buffered yet — wait for next chunk. */
        if (c->bench_state.source_len_pending > 0) {
            return 0;
        }
        /* P6 (R2): on the call where we just completed the length,
         * materialise N. Use length_decoded sentinel instead of the
         * brittle (source_remaining == 0 && bytes_processed == 0)
         * coupling — a future change that bumps bytes_processed for
         * any reason previously would have broken materialise silently. */
        if (!c->bench_state.length_decoded) {
            uint32_t n = (uint32_t)c->bench_state.source_len_buf[0]
                      | ((uint32_t)c->bench_state.source_len_buf[1] << 8)
                      | ((uint32_t)c->bench_state.source_len_buf[2] << 16)
                      | ((uint32_t)c->bench_state.source_len_buf[3] << 24);
            /* P12 (R2 / D2): hard-cap N to prevent adversarial 4 GiB DoS
             * blocking the net-worker thread. AC #3 hot-path safety. */
            if (n > BENCH_SOURCE_MAX) {
                return -1003;
            }
            c->bench_state.source_remaining = n;
            c->bench_state.length_decoded = 1;
            /* P1 (R2): N=0 boundary — AC #2 says SOURCE "emits exactly N
             * bytes and closes". With N=0 there is nothing to emit; close
             * immediately rather than leaving a zombie session that never
             * returns BENCH_RC_SOURCE_DONE through the emit path. */
            if (n == 0) {
                return BENCH_RC_SOURCE_DONE;
            }
        }

        /* Emit CSPRNG bytes up to min(remaining, BENCH_OUT_CAP - out_len). */
        int avail = BENCH_OUT_CAP - c->out_len;
        if (avail < 0) avail = 0;
        uint32_t to_emit = c->bench_state.source_remaining;
        if ((uint32_t)avail < to_emit) to_emit = (uint32_t)avail;

        if (to_emit > 0) {
            uint8_t tmp[1024];
            uint32_t emitted = 0;
            while (emitted < to_emit) {
                uint32_t chunk = to_emit - emitted;
                if (chunk > sizeof(tmp)) chunk = sizeof(tmp);
                if (bench_csprng(tmp, chunk) != 0) {
                    /* CSPRNG failure: surface as internal error. */
                    return -2;
                }
                out_push(c, tmp, (int)chunk);
                emitted += chunk;
            }
            c->bench_state.source_remaining -= emitted;
            c->bench_state.bytes_processed  += emitted;
            atomic_fetch_add_explicit(&g_bench_source_bytes, (uint64_t)emitted,
                                      memory_order_relaxed);
            /* C5: all N bytes emitted — signal caller to flush and WS-close. */
            if (c->bench_state.source_remaining == 0) {
                return BENCH_RC_SOURCE_DONE;
            }
            payload_consumed = (int)emitted;
        }
        break;
    }

    default:
        /* Unreachable: mode validated when first byte was consumed. */
        return -1003;
    }

    return payload_consumed;
}

#endif /* TELEPROTO3_BENCH */
