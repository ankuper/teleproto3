/*
 * net-type3-dispatch.c — fork-local Type3 dispatch hook implementation.
 *
 * Bridges upstream MTProxy's CRYPTO_INIT to libteleproto3. All wire-
 * format decisions live in the library; this file is glue.
 *
 * See spec/wire-format.md §1–3 and spec/anti-probe.md §1 for the
 * normative behaviour this glue must honour.
 */

#include "net/net-type3-dispatch.h"
#include "net/net-type3-stats.h"
#include "net/net-connections.h"
#include "net/net-msg.h"

#include "jobs/jobs.h"
#include "common/precise-time.h"
#include "common/kprintf.h"

#include <t3.h>

#include <sys/stat.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>

/* ======================================================================
 * Kill-switch state
 * ====================================================================== */

#define KILL_SWITCH_MARKER "/etc/teleproxy-ws-v2/disabled"

typedef enum {
    KS_IDLE        = 0,  /* marker absent — serving normally */
    KS_DRAINING    = 1,  /* marker present + drain: new conns silently closed */
    KS_HARD_CLOSED = 2   /* marker present + hard-close: all conns closed */
} kill_switch_state_t;

/* Atomic state (kill_switch_state_t as int).
 * Written from main-loop poll; read from NET-CPU parse_execute thread.
 * Both are single-threaded in this engine, but atomic is cheap and
 * makes the concurrency intent explicit. */
static _Atomic int g_ks_state   = 0;   /* kill_switch_state_t */
static int         g_ks_hard_close = 0;  /* 0=drain (default), 1=hard-close */

/* ======================================================================
 * Dispatcher RNG session (for t3_silent_close_delay_sample_ns)
 *
 * t3_silent_close_delay_sample_ns requires a t3_session_t with bound
 * callbacks including a working RNG.  We maintain a single long-lived
 * dispatcher session whose only purpose is to drive the timing function.
 * The session holds no cryptographic material — it is used for RNG only.
 * ====================================================================== */

static t3_secret_t  *g_dispatch_secret  = NULL;
static t3_session_t *g_dispatch_session = NULL;

/* Minimal RNG shim backed by /dev/urandom */
static int dispatch_rng(void *ctx, uint8_t *buf, size_t len) {
    (void)ctx;
    static int urandom_fd = -1;
    if (urandom_fd < 0) {
        urandom_fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
        if (urandom_fd < 0) return -1;
    }

    size_t total_read = 0;
    while (total_read < len) {
        ssize_t n = read(urandom_fd, buf + total_read, len - total_read);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1; /* unexpected EOF */
        total_read += n;
    }
    return 0;
}

/* Stub callbacks required by t3_session_bind_callbacks — not used. */
static int64_t disp_lower_send(void *c, const uint8_t *b, size_t l)  { (void)c;(void)b;(void)l; return -1; }
static int64_t disp_lower_recv(void *c, uint8_t *b, size_t l)        { (void)c;(void)b;(void)l; return -1; }
static int64_t disp_frame_send(void *c, const uint8_t *b, size_t l, int f) { (void)c;(void)b;(void)l;(void)f; return -1; }
static int64_t disp_frame_recv(void *c, uint8_t *b, size_t l, int *f) { (void)c;(void)b;(void)l;(void)f; return -1; }
static uint64_t disp_mono_ns(void *c) { (void)c; return (uint64_t)(precise_now * 1e9); }

/*
 * Sample a uniform delay in [50ms, 200ms] via the lib API.
 * Falls back to a safe constant (125 ms) if the session is unavailable.
 * Story 4-5 Dev Notes: do NOT inline bare modulo arithmetic — the lib API
 * t3_silent_close_delay_sample_ns is the single source of delay sampling.
 */
static uint64_t sample_silent_close_delay_ns(void) {
    if (g_dispatch_session) {
        uint64_t delay_ns = 0;
        if (t3_silent_close_delay_sample_ns(g_dispatch_session, &delay_ns) == T3_OK) {
            return delay_ns;
        }
    }
    /* Fallback: mid-range constant (should not happen in production). */
    return 125000000ULL;  /* 125 ms */
}

/* ======================================================================
 * type3_dispatch_init
 * ====================================================================== */

int type3_dispatch_init(const char *kill_switch_mode) {
    /* Parse kill-switch mode */
    if (kill_switch_mode && strcmp(kill_switch_mode, "hard-close") == 0) {
        g_ks_hard_close = 1;
    } else if (!kill_switch_mode || strcmp(kill_switch_mode, "drain") == 0) {
        g_ks_hard_close = 0;
    } else {
        kprintf("type3_dispatch_init: unknown kill_switch_mode '%s', using 'drain'\n",
                kill_switch_mode);
        return -1;
    }

    /* Build a minimal dispatcher session for the RNG timer:
     *
     * We need a valid t3_session_t (with callbacks_bound=1) so that
     * t3_silent_close_delay_sample_ns can call cb->rng().
     *
     * We use a hardcoded dummy Type3 secret (ee + 16-byte zero key + "t")
     * solely to satisfy the lifecycle API.  This secret is NEVER used for
     * any cryptographic operation in the dispatcher path.
     *
     * Encoding: 0xee prefix (Type3 marker) + 16-byte key + host "t" (0x74).
     */
    static const uint8_t dummy_encoded[] = {
        0xee,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
        0x74  /* 't' */
    };

    t3_result_t rc = t3_secret_parse(dummy_encoded, sizeof(dummy_encoded),
                                      &g_dispatch_secret);
    if (rc != T3_OK) {
        kprintf("type3_dispatch_init: t3_secret_parse failed (%s) — RNG delay disabled\n",
                t3_strerror(rc));
        /* Non-fatal: fallback delay (125 ms constant) will be used */
        return 0;
    }

    rc = t3_session_new(g_dispatch_secret, &g_dispatch_session);
    if (rc != T3_OK) {
        kprintf("type3_dispatch_init: t3_session_new failed (%s)\n", t3_strerror(rc));
        t3_secret_free(g_dispatch_secret);
        g_dispatch_secret = NULL;
        return 0;
    }

    static t3_callbacks_t disp_cb;
    memset(&disp_cb, 0, sizeof(disp_cb));
    disp_cb.struct_size  = sizeof(disp_cb);
    disp_cb.lower_send   = disp_lower_send;
    disp_cb.lower_recv   = disp_lower_recv;
    disp_cb.frame_send   = disp_frame_send;
    disp_cb.frame_recv   = disp_frame_recv;
    disp_cb.rng          = dispatch_rng;
    disp_cb.monotonic_ns = disp_mono_ns;

    rc = t3_session_bind_callbacks(g_dispatch_session, &disp_cb);
    if (rc != T3_OK) {
        kprintf("type3_dispatch_init: bind_callbacks failed (%s)\n", t3_strerror(rc));
        t3_session_free(g_dispatch_session);
        t3_secret_free(g_dispatch_secret);
        g_dispatch_session = NULL;
        g_dispatch_secret  = NULL;
        return 0;
    }

    kprintf("type3_dispatch_init: ready (kill_switch_mode=%s)\n",
            g_ks_hard_close ? "hard-close" : "drain");
    return 0;
}

/* ======================================================================
 * type3_dispatch_cleanup
 * ====================================================================== */

void type3_dispatch_cleanup(void) {
    if (g_dispatch_session) {
        t3_session_free(g_dispatch_session);
        g_dispatch_session = NULL;
    }
    if (g_dispatch_secret) {
        t3_secret_free(g_dispatch_secret);
        g_dispatch_secret = NULL;
    }
}

/* ======================================================================
 * type3_dispatch_silent_close — Task 1.3
 * Schedule delayed FIN via upstream job timer.
 * AC #2: no WS close frame is sent.
 * ====================================================================== */

void type3_dispatch_silent_close(connection_job_t C, uint64_t delay_ns) {
    /* Convert ns to seconds for the upstream double-based timer. */
    double delay_sec = (double)delay_ns / 1.0e9;
    if (delay_sec < 0.050) delay_sec = 0.050;   /* floor at 50 ms */
    if (delay_sec > 0.200) delay_sec = 0.200;   /* ceil at 200 ms */
    /* job_timer_insert fires alarm() on the connection at precise_now + delay.
     * The existing tcp_rpcs_ext_alarm handler calls fail_connection, which
     * performs a clean TCP FIN without sending a WebSocket close frame.
     * This is the upstream-sanctioned path for timed connection teardown. */
    job_timer_insert(C, precise_now + delay_sec);
}

/* ======================================================================
 * type3_dispatch_on_crypto_init  — Task 1.1
 * AC #1, #2, #3, #4
 * Called from AR-S2 hook in net-tcp-rpc-ext-server.c when
 *   ws_state == WS_STATE_ACTIVE && !c->crypto
 * ====================================================================== */

type3_dispatch_outcome_t type3_dispatch_on_crypto_init(connection_job_t C) {
    struct connection_info *c = CONN_INFO(C);

    /* Kill-switch: reject new connections when KS is active (AC #5). */
    int ks = atomic_load_explicit(&g_ks_state, memory_order_relaxed);
    if (ks != KS_IDLE) {
        vkprintf(1, "Type3 dispatch: kill-switch state=%d, silent close\n", ks);
        uint64_t delay_ns = sample_silent_close_delay_ns();
        type3_dispatch_silent_close(C, delay_ns);
        T3_STAT_INC(t3_stat_silent_close_rate_limited);
        T3_STAT_INC(t3_stat_connections_total_type3_silent_close);
        type3_stats_record_probe_drop_ns((long long)delay_ns);
        return TYPE3_DISPATCH_DROP_SILENT;
    }

    /* Need at least 4 bytes for the Session Header (AC #1). */
    if (c->in.total_bytes < 4) {
        /* Insufficient data — not a Type3 session or still arriving.
         * Fall through to existing MTProto detection (passthrough). */
        return TYPE3_DISPATCH_PASSTHROUGH;
    }

    /* Peek at the first 4 bytes without consuming them.
     * The WS frame unwrapper has already placed unmasked payload in c->in.
     * AC #1: call t3_header_parse from libteleproto3 (not inline logic). */
    uint8_t header_buf[4];
    if (rwm_fetch_lookup(&c->in, header_buf, 4) != 4) {
        return TYPE3_DISPATCH_PASSTHROUGH;
    }

    t3_header_t hdr;
    t3_result_t rc = t3_header_parse(header_buf, &hdr);

    if (rc != T3_OK) {
        /* AC #2: parse failure → silent close with uniform 50–200 ms delay.
         * No WS close frame (job_timer_insert fires fail_connection, not ws_close).
         * Counter teleproto3_silent_close_total{reason} incremented. */
        uint64_t delay_ns = sample_silent_close_delay_ns();

        const char *reason_label;
        long long *reason_counter;
        switch (rc) {
        case T3_ERR_UNSUPPORTED_VERSION:
            reason_label   = "bad_version";
            reason_counter = &t3_stat_silent_close_bad_version;
            break;
        case T3_ERR_MALFORMED:
            reason_label   = "unknown_cmd";
            reason_counter = &t3_stat_silent_close_unknown_cmd;
            break;
        case T3_ERR_INVALID_ARG:
            reason_label   = "short";
            reason_counter = &t3_stat_silent_close_short;
            break;
        default:
            reason_label   = "bad_flags";
            reason_counter = &t3_stat_silent_close_bad_flags;
            break;
        }

        vkprintf(2, "Type3 dispatch: header parse failed [%s] (%s), "
                    "silent close after %llu ms\n",
                 reason_label, t3_strerror(rc),
                 (unsigned long long)(delay_ns / 1000000));

        /* AC #2: schedule delayed FIN (no WS close frame) */
        type3_dispatch_silent_close(C, delay_ns);

        /* AC #2: increment teleproto3_silent_close_total{reason} */
        T3_STAT_INC(*reason_counter);
        T3_STAT_INC(t3_stat_connections_total_type3_bad_header);
        T3_STAT_INC(t3_stat_connections_total_type3_silent_close);
        type3_stats_record_probe_drop_ns((long long)delay_ns);

        /* Do not consume the 4-byte header; caller will SKIP_ALL_BYTES
         * and teardown the connection anyway. */
        return TYPE3_DISPATCH_DROP_SILENT;
    }

    /* AC #3: successful parse → session proceeds to MTPROTO_PASSTHROUGH.
     * The hook returns ACCEPT; the caller falls through to the existing
     * obfuscated2 + MTProto path with the Session Header already consumed. */
    vkprintf(2, "Type3 dispatch: accepted Session Header "
                "cmd=0x%02x ver=0x%02x flags=0x%04x\n",
             hdr.command_type, hdr.version, hdr.flags);

    /* Consume the 4-byte Session Header. */
    rwm_skip_data(&c->in, 4);

    T3_STAT_INC(t3_stat_connections_total_type3_accept);
    T3_STAT_ADD(t3_stat_connections_active, 1);

    return TYPE3_DISPATCH_ACCEPT;
}

/* ======================================================================
 * type3_dispatch_kill_switch_poll — Task 3.1, 3.2, 3.3
 * AC #5: stat() KILL_SWITCH_MARKER at 1 Hz from main loop.
 * ====================================================================== */

void type3_dispatch_kill_switch_poll(void) {
    int present = (access(KILL_SWITCH_MARKER, F_OK) == 0);

    kill_switch_state_t new_state;
    if (!present) {
        new_state = KS_IDLE;
    } else if (g_ks_hard_close) {
        new_state = KS_HARD_CLOSED;
    } else {
        new_state = KS_DRAINING;
    }

    int old_state = atomic_exchange_explicit(&g_ks_state, (int)new_state,
                                             memory_order_relaxed);

    if ((int)new_state != old_state) {
        vkprintf(0, "Type3 kill-switch: state %d → %d (marker %s)\n",
                 old_state, (int)new_state,
                 present ? "present" : "absent");
        /* Update the teleproto3_kill_switch_state Prometheus gauge */
        T3_STAT_STORE(t3_stat_kill_switch_state, (long long)new_state);
    }

    /*
     * Task 3.3 — Race note (per AC #5 and Story 1-8 Dev Notes):
     * A create→delete within one polling tick is NOT guaranteed to be
     * observed.  Operators relying on observable kill-switch activation
     * MUST hold the marker present for ≥ 1 second.  This 1000 ms
     * worst-case activation latency is the contracted guarantee, not a bug.
     */
}
