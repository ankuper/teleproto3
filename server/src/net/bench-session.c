/*
 * bench-session.c — per-connection registry for Type3 BENCH sessions.
 *
 * Kept separate from bench-handler.c so the unit test
 * (tests/test_bench_handler.c) can link only the pure-logic TU without
 * pulling in upstream net-connections.h (which chains through OpenSSL).
 *
 * Story 1a-2. Whole TU is a no-op when TELEPROTO3_BENCH is undefined.
 */

#ifdef TELEPROTO3_BENCH

#include "bench-handler.h"
#include "net/net-connections.h"
#include "net/net-msg.h"
#include "net/net-websocket.h"
#include "common/kprintf.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <time.h>
#include <sys/uio.h>     /* writev / struct iovec */
#include <unistd.h>      /* close (kept for completeness; teardown owned by upstream) */
#include <fcntl.h>       /* F_GETFL / F_SETFL / O_NONBLOCK */
#include "net/net-msg-buffers.h"  /* MSG_STD_BUFFER for chunk-cap math */

typedef struct {
    int fd;
    int generation;
    bench_conn_t *conn;
    time_t last_activity_ts;  /* P11 (R2): bumped on activity, used by reaper */
} bench_slot_t;

/* D4: accessed only from the single net-worker thread — see bench-handler.h */
static bench_slot_t g_bench_slots[BENCH_MAX_SESSIONS];

static int bench_slot_match(const bench_slot_t *s, const struct connection_info *c) {
    return s->conn != NULL && s->fd == c->fd && s->generation == c->generation;
}

/* P11 (R2 / D1): reap slots whose last_activity_ts is older than
 * BENCH_SLOT_IDLE_SEC. Called at the top of bench_session_install before
 * the linear free-slot scan. Removes dependence on a connection-close
 * hook in upstream net code (UPSTREAM.md keep-clean). Known race: a
 * client whose frame arrives within the reap window receives a TCP RST
 * instead of a graceful WS-close — acceptable for dev-self-use. */
static void bench_reap_stale_slots(time_t now) {
    for (int i = 0; i < BENCH_MAX_SESSIONS; i++) {
        if (g_bench_slots[i].conn == NULL) continue;
        if (now - g_bench_slots[i].last_activity_ts > BENCH_SLOT_IDLE_SEC) {
            free(g_bench_slots[i].conn);
            g_bench_slots[i].conn             = NULL;
            g_bench_slots[i].fd               = 0;
            g_bench_slots[i].generation       = 0;
            g_bench_slots[i].last_activity_ts = 0;
        }
    }
}

bench_conn_t *bench_session_install(struct connection_info *c) {
    if (!g_bench_handler_enabled || !c) return NULL;
    time_t now = time(NULL);
    bench_reap_stale_slots(now);  /* P11: housekeeping before allocation */
    for (int i = 0; i < BENCH_MAX_SESSIONS; i++) {
        if (bench_slot_match(&g_bench_slots[i], c)) {
            g_bench_slots[i].last_activity_ts = now;
            return g_bench_slots[i].conn;
        }
    }
    /* Find a free slot. */
    int slot = -1;
    for (int i = 0; i < BENCH_MAX_SESSIONS; i++) {
        if (g_bench_slots[i].conn == NULL) { slot = i; break; }
    }
    /* D1: refuse-when-full — no eviction avoids use-after-free.
     * For dev-self-use with 64 slots this never triggers in practice. */
    if (slot < 0) {
        vkprintf(0, "Type3 BENCH: registry full (%d slots) — refusing new session\n",
                 BENCH_MAX_SESSIONS);
        return NULL;
    }

    bench_conn_t *bc = calloc(1, sizeof(*bc));
    if (!bc) return NULL;
    if (bench_handler_init(bc) != 0) { free(bc); return NULL; }
    g_bench_slots[slot].fd               = c->fd;
    g_bench_slots[slot].generation       = c->generation;
    g_bench_slots[slot].conn             = bc;
    g_bench_slots[slot].last_activity_ts = now;
    return bc;
}

bench_conn_t *bench_session_lookup(struct connection_info *c) {
    if (!c) return NULL;
    for (int i = 0; i < BENCH_MAX_SESSIONS; i++) {
        if (bench_slot_match(&g_bench_slots[i], c)) return g_bench_slots[i].conn;
    }
    return NULL;
}

void bench_session_destroy(struct connection_info *c) {
    if (!c) return;
    for (int i = 0; i < BENCH_MAX_SESSIONS; i++) {
        if (bench_slot_match(&g_bench_slots[i], c)) {
            free(g_bench_slots[i].conn);
            g_bench_slots[i].conn             = NULL;
            g_bench_slots[i].fd               = 0;
            g_bench_slots[i].generation       = 0;
            g_bench_slots[i].last_activity_ts = 0;
            return;
        }
    }
}

/* Emit a WebSocket close frame directly into c->out.
 * RFC 6455 §5.5.1: opcode=0x8 (CLOSE), FIN=1, payload=2-byte BE status code.
 * Server→client frames are unmasked.
 *
 * P4 (R2): exposed (non-static) so the AR-S2 dispatcher can emit a 1013
 * "Try Again Later" close when bench_session_install refuses (registry full),
 * giving clients a protocol-level reason rather than a raw TCP close. */
void bench_emit_ws_close(struct connection_info *c, uint16_t status_code) {
    uint8_t frame[4];
    frame[0] = 0x88;                            /* FIN=1, opcode=CLOSE(8) */
    frame[1] = 0x02;                            /* payload length = 2, no mask */
    frame[2] = (uint8_t)(status_code >> 8);
    frame[3] = (uint8_t)(status_code & 0xFF);
    rwm_push_data(&c->out, frame, 4);
}

/*
 * bench_flush_out_to_socket — 1a-8: synchronously drain c->out to the
 * kernel socket send buffer via writev(2) before fail_connection(C,-1).
 *
 * fail_connection sets JS_ABORT which tears down the connection without
 * flushing c->out (a userspace buffer). writev here copies the queued
 * bytes (WS data frame + WS close 1000) into the kernel send queue so
 * TCP delivers them with a graceful FIN instead of RST. Only used on
 * the BENCH SOURCE path; quality bar is dev-self-use.
 *
 * Cross-thread shortcut: the canonical c->out → kernel-socket writer
 * (cpu_tcp_server_writer / net_server_socket_read_write in
 * net-tcp-connections.c) runs on the net-thread and asserts
 * assert_net_net_thread(). This helper writes to c->fd directly from the
 * CPU/parse thread. The shortcut is justified only because the caller
 * (bench_drain_connection) returns -1 immediately after, and the dispatcher
 * then calls fail_connection(C,-1), so the connection is being torn down
 * and no concurrent net-thread writev can be in flight on this fd.
 *
 * Bound: a 30-second wall-clock budget protects against a stalled peer
 * holding the TCP receive window at zero — even though --enable-bench-handler
 * is dev-self-use, the runtime CLI flag does not gate which worker thread
 * serves which connection, so an unbounded blocking writev on a co-resident
 * proxy could starve other clients sharing this thread.
 */
static void bench_flush_out_to_socket(struct connection_info *c) {
    if (c == NULL || c->fd < 0) return;
    if (c->out.total_bytes == 0) return;

    /* Switch fd to blocking so writev delivers all bytes even for large N
     * (e.g. 50 MB) where the non-blocking kernel send buffer fills mid-stream.
     * If F_GETFL or F_SETFL fails, skip the flush rather than write in an
     * undefined fd-flag state — the dispatcher's fail_connection still runs,
     * which preserves the pre-fix behaviour for this corner. */
    int saved_flags = fcntl(c->fd, F_GETFL, 0);
    if (saved_flags < 0) {
        vkprintf(0, "Type3 BENCH: bench_flush_out_to_socket: F_GETFL failed "
                    "(errno=%d) — skipping flush, %d bytes lost\n",
                    errno, c->out.total_bytes);
        return;
    }
    if (fcntl(c->fd, F_SETFL, saved_flags & ~O_NONBLOCK) < 0) {
        vkprintf(0, "Type3 BENCH: bench_flush_out_to_socket: F_SETFL blocking "
                    "failed (errno=%d) — skipping flush, %d bytes lost\n",
                    errno, c->out.total_bytes);
        return;
    }

    struct timespec t_start;
    clock_gettime(CLOCK_MONOTONIC, &t_start);
    const long FLUSH_BUDGET_NS = 30L * 1000000000L;  /* 30s wall-clock cap */

    struct iovec iov[256];
    while (c->out.total_bytes > 0) {
        struct timespec t_now;
        clock_gettime(CLOCK_MONOTONIC, &t_now);
        long elapsed_ns = (long)(t_now.tv_sec - t_start.tv_sec) * 1000000000L
                        + (t_now.tv_nsec - t_start.tv_nsec);
        if (elapsed_ns > FLUSH_BUDGET_NS) {
            vkprintf(0, "Type3 BENCH: bench_flush_out_to_socket: 30s budget "
                        "exceeded with %d bytes remaining — bailing\n",
                        c->out.total_bytes);
            break;
        }
        /* Cap chunk so rwm_prepare_iovec does not return -1 when c->out has
         * more than 256 msg_parts (returns -1 when iov_len exhausted before
         * total_bytes described). 256 * MSG_STD_BUFFER (= 512 KiB) per
         * iteration is plenty for SOURCE bench up to 50 MB. */
        int chunk = c->out.total_bytes;
        if (chunk > 256 * MSG_STD_BUFFER) chunk = 256 * MSG_STD_BUFFER;
        int niov = rwm_prepare_iovec(&c->out, iov, 256, chunk);
        if (niov <= 0) break;
        ssize_t nw = writev(c->fd, iov, niov);
        if (nw < 0) {
            if (errno == EINTR) continue;
            vkprintf(0, "Type3 BENCH: bench_flush_out_to_socket: writev failed "
                        "(errno=%d) — %d bytes lost\n", errno, c->out.total_bytes);
            break;
        }
        if (nw == 0) break;
        rwm_skip_data(&c->out, (int)nw);
    }
    fcntl(c->fd, F_SETFL, saved_flags);

    if (c->out.total_bytes > 0) {
        vkprintf(0, "Type3 BENCH: bench_flush_out_to_socket: %d bytes remain "
                    "after flush — SOURCE data may be truncated\n",
                    c->out.total_bytes);
    }
    /* Leave c->out in valid-empty state so the upstream writer/teardown
     * does not re-send stale data. msg_part_decref(NULL) is NULL-tolerant
     * (net-msg.c:95-121: while-loop on NULL never enters), so a subsequent
     * rwm_free from JS_ABORT teardown on this empty raw_message is safe —
     * only side-effect is one extra MODULE_STAT->rwm_total_msgs decrement,
     * an off-by-one in stats, not a functional defect. */
    rwm_free(&c->out);
    rwm_init(&c->out, 0);
}

/* ================================================================= *
 * Story 1a-9: bench-connection marker — prevents bench→MTProto      *
 * fall-through when the bench session is destroyed mid-stream.      *
 *                                                                    *
 * g_bench_marks stores (fd, generation) pairs of connections that   *
 * have been assigned TYPE3_DISPATCH_BENCH by the AR-S2 hook.        *
 * The marker outlives the bench_conn_t session: after              *
 * bench_session_destroy() the slot in g_bench_slots has conn=NULL   *
 * and fd/generation zeroed, but the matching entry in g_bench_marks *
 * remains so the dispatcher can detect "was bench, session gone"    *
 * and close the connection instead of re-dispatching to MTProto.    *
 *                                                                    *
 * Sizing: BENCH_MAX_SESSIONS marks (same as session slots) is       *
 * sufficient because at most BENCH_MAX_SESSIONS bench connections   *
 * exist simultaneously. Stale entries from closed connections are   *
 * reclaimed by bench_connection_clear().                            *
 * ================================================================= */

typedef struct {
    int fd;
    int generation;
} bench_mark_t;

/* D4: same single-net-thread assumption as g_bench_slots. */
static bench_mark_t g_bench_marks[BENCH_MAX_SESSIONS];

void bench_connection_mark(struct connection_info *c) {
    if (!c) return;
    /* Check already marked. */
    for (int i = 0; i < BENCH_MAX_SESSIONS; i++) {
        if (g_bench_marks[i].fd         == c->fd &&
            g_bench_marks[i].generation == c->generation)
            return;
    }
    /* Find a free slot (fd == 0 means unused; fd 0 is never a real bench fd
     * because fd 0 is stdin, which a server process never uses for clients). */
    for (int i = 0; i < BENCH_MAX_SESSIONS; i++) {
        if (g_bench_marks[i].fd == 0) {
            g_bench_marks[i].fd         = c->fd;
            g_bench_marks[i].generation = c->generation;
            return;
        }
    }
    /* Table full — log and ignore; worst case: one connection may fall through
     * to MTProto (same behaviour as before this fix). */
    vkprintf(0, "Type3 BENCH: bench_connection_mark: mark table full (%d slots)\n",
             BENCH_MAX_SESSIONS);
}

int bench_connection_is_marked(struct connection_info *c) {
    if (!c) return 0;
    for (int i = 0; i < BENCH_MAX_SESSIONS; i++) {
        if (g_bench_marks[i].fd         == c->fd &&
            g_bench_marks[i].generation == c->generation)
            return 1;
    }
    return 0;
}

void bench_connection_clear(struct connection_info *c) {
    if (!c) return;
    for (int i = 0; i < BENCH_MAX_SESSIONS; i++) {
        if (g_bench_marks[i].fd         == c->fd &&
            g_bench_marks[i].generation == c->generation) {
            g_bench_marks[i].fd         = 0;
            g_bench_marks[i].generation = 0;
            return;
        }
    }
}

/*
 * bench_drain_connection — drain plaintext bytes from c->in through the
 * bench handler and write output (if any) back to c->out as WS frames.
 *
 * Returns 0 to continue, -1 if the caller must close the connection.
 *
 * C7 + P11 (R2 / D1): bench_session_destroy is called on every -1 return
 * path. Slots leaked by close paths that DON'T trigger a -1 return (peer
 * disconnect, idle timeout, TCP RST) are reclaimed by the idle reaper at
 * the next bench_session_install (BENCH_SLOT_IDLE_SEC = 300s); see
 * bench-handler.h "IDLE-REAPING" header comment.
 */
int bench_drain_connection(connection_job_t C) {
    struct connection_info *c = CONN_INFO(C);
    bench_conn_t *bsess = bench_session_lookup(c);
    if (!bsess) return -1;
    /* P11 (R2): bump activity timestamp so the reaper does not collect a
     * slot that is actively serving traffic. */
    {
        time_t now = time(NULL);
        for (int i = 0; i < BENCH_MAX_SESSIONS; i++) {
            if (bench_slot_match(&g_bench_slots[i], c)) {
                g_bench_slots[i].last_activity_ts = now;
                break;
            }
        }
    }

    /* Pull all currently-available plaintext bytes out of c->in. */
    while (c->in.total_bytes > 0) {
        unsigned char buf[8192];
        int n = c->in.total_bytes;
        if (n > (int)sizeof(buf)) n = (int)sizeof(buf);

        /* D2: For ECHO, limit fetch to available output buffer space to
         * prevent silent byte-drop when out_buf fills before fetch completes.
         * SINK and SOURCE don't write to out_buf per received byte, so they
         * are unaffected. */
        if (bsess->bench_state.mode_byte_seen &&
            bsess->bench_state.mode == BENCH_MODE_ECHO) {
            int avail_out = BENCH_OUT_CAP - bsess->out_len;
            if (avail_out <= 0) break;  /* buffer full — flush on return, retry */
            if (n > avail_out) n = avail_out;
        }

        if (rwm_fetch_data(&c->in, buf, n) != n) break;

        int rc = bench_handler_recv(bsess, buf, n);

        if (rc == BENCH_RC_SOURCE_DONE) {
            /* C5: SOURCE finished — flush final output and send WS close 1000. */
            if (bsess->out_len > 0) {
                ws_write_frame_header(&c->out, bsess->out_len);
                rwm_push_data(&c->out, bsess->out_buf, bsess->out_len);
                bsess->out_len = 0;
            }
            bench_emit_ws_close(c, 1000);
            bench_flush_out_to_socket(c);  /* 1a-8: deliver before TCP close */
            bench_session_destroy(c);  /* C7 */
            return -1;
        }
        if (rc == -1003) {
            /* C3: invalid sub-mode — send WS close 1003 (Unsupported Data). */
            vkprintf(0, "Type3 BENCH: invalid sub-mode — closing with WS 1003\n");
            bench_emit_ws_close(c, 1003);
            bench_session_destroy(c);  /* C7 */
            return -1;
        }
        if (rc < 0) {
            vkprintf(0, "Type3 BENCH: handler error %d — closing\n", rc);
            bench_session_destroy(c);  /* C7 */
            return -1;
        }

        if (bsess->out_len > 0) {
            ws_write_frame_header(&c->out, bsess->out_len);
            rwm_push_data(&c->out, bsess->out_buf, bsess->out_len);
            bsess->out_len = 0;
        }
    }

    /* C6: SOURCE mode with N > BENCH_OUT_CAP — continue emitting CSPRNG bytes
     * across multiple BENCH_OUT_CAP-sized chunks within this single drain call.
     * No new client input is needed; bench_handler_recv(len=0) continues emit. */
    while (bsess->bench_state.mode_byte_seen &&
           bsess->bench_state.mode == BENCH_MODE_SOURCE &&
           bsess->bench_state.source_len_pending == 0 &&
           bsess->bench_state.source_remaining > 0) {
        int rc = bench_handler_recv(bsess, NULL, 0);

        if (rc == BENCH_RC_SOURCE_DONE) {
            if (bsess->out_len > 0) {
                ws_write_frame_header(&c->out, bsess->out_len);
                rwm_push_data(&c->out, bsess->out_buf, bsess->out_len);
                bsess->out_len = 0;
            }
            bench_emit_ws_close(c, 1000);
            bench_flush_out_to_socket(c);  /* 1a-8: deliver before TCP close */
            bench_session_destroy(c);  /* C7 */
            return -1;
        }
        if (rc < 0) {
            vkprintf(0, "Type3 BENCH: SOURCE continuation error %d — closing\n", rc);
            bench_session_destroy(c);  /* C7 */
            return -1;
        }

        if (bsess->out_len > 0) {
            ws_write_frame_header(&c->out, bsess->out_len);
            rwm_push_data(&c->out, bsess->out_buf, bsess->out_len);
            bsess->out_len = 0;
        }
    }

    return 0;
}

#endif /* TELEPROTO3_BENCH */
