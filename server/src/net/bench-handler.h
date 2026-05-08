/*
 * bench-handler.h — Type3 bench echo handler (Story 1a-2).
 *
 * Routes Type3 sessions with command_type=0x04 (T3_CMD_BENCH) to one of
 * three sub-modes (sink / echo / source) for throughput measurement.
 *
 * DOUBLE GATE — both must be on for any code in this module to execute:
 *   - Build-time:  -DTELEPROTO3_BENCH  (else the whole TU is empty)
 *   - Runtime:     g_bench_handler_enabled  (set by --enable-bench-handler)
 *
 * Production builds compile WITHOUT TELEPROTO3_BENCH; bench symbols are
 * absent from the release artefact (see tests/test_bench_ci_release.sh).
 *
 * Quality bar: dev-self-use only. Not on the production hot path.
 */

#ifndef TELEPROTO3_SERVER_NET_BENCH_HANDLER_H
#define TELEPROTO3_SERVER_NET_BENCH_HANDLER_H

#ifdef TELEPROTO3_BENCH

#include <stdint.h>

/* Sub-mode opcodes — first byte of the post-handshake bench payload.
 * Wire-format authority: spec/wire-format.md §3.1 (amendment W-003). */
#define BENCH_MODE_SINK    0x01u
#define BENCH_MODE_ECHO    0x02u
#define BENCH_MODE_SOURCE  0x03u

/* Output buffer capacity. ECHO/SOURCE write into bench_conn.out_buf;
 * the dispatch glue (net-type3-dispatch.c) drains this into c->out via
 * rwm_push_data and resets out_len. Capacity is sized to comfortably
 * hold one TLS record worth of bench payload. */
#define BENCH_OUT_CAP      4096

/* SOURCE-mode max length (P12 / D2 R2): hard cap on client-supplied N to
 * prevent adversarial 4 GiB CSPRNG emit blowing up the net-worker thread.
 * AC #3 hot-path safety under malformed input. 64 MiB covers all realistic
 * bench scenarios with ×10 headroom; values above are rejected with -1003. */
#define BENCH_SOURCE_MAX   (64u << 20)

/* Per-connection bench state. */
typedef struct bench_handler_state {
    uint8_t  mode;              /* 0 = uninitialised, 0x01/0x02/0x03 = sink/echo/source */
    uint8_t  initialised;       /* 1 once bench_handler_init returned 0 */
    uint8_t  mode_byte_seen;    /* 1 once first payload byte consumed */
    uint8_t  source_len_pending; /* SOURCE: bytes of the 4-byte LE length still to consume */
    uint8_t  source_len_buf[4];
    uint8_t  length_decoded;    /* SOURCE: 1 once source_len_buf has been materialised into source_remaining (P6: replaces fragile bytes_processed==0 sentinel) */
    uint64_t bytes_processed;
    uint32_t source_remaining;  /* SOURCE: bytes left to emit */
} bench_handler_state_t;

/* Aggregate stats — exposed via bench_handler_get_stats() and the
 * Prometheus stats endpoint (gated on TELEPROTO3_BENCH). */
typedef struct bench_stats {
    uint64_t sink_bytes;
    uint64_t echo_bytes;
    uint64_t source_bytes;
} bench_stats_t;

/* Self-contained bench session.
 *
 * The real upstream `connection_info` does not have these fields; the dispatch
 * glue allocates one bench_conn_t per Type3 connection in BENCH mode and
 * keeps a (connection_info * → bench_conn_t *) map locally. The unit test
 * (tests/test_bench_handler.c) instantiates this struct directly. */
typedef struct bench_conn {
    bench_handler_state_t bench_state;
    uint8_t   out_buf[BENCH_OUT_CAP];
    int       out_len;       /* bytes currently in out_buf, 0..BENCH_OUT_CAP */
    uint8_t   _pad[64];      /* reserved for future fields */
} bench_conn_t;

/* Runtime gate — default 0 (OFF). Set to 1 by --enable-bench-handler.
 * Defined in bench-handler.c. */
extern int g_bench_handler_enabled;

/* bench_handler_recv return codes (negative values are signals/errors). */
#define BENCH_RC_SOURCE_DONE  (-100)  /* SOURCE emitted all N bytes — caller must WS-close */

/* Lifecycle.
 * bench_handler_init returns 0 if both gates are on, -1 otherwise.
 * bench_handler_recv processes one chunk of post-handshake payload; on the
 * first call it consumes the mode byte. Returns:
 *   >= 0               bytes of useful payload consumed (excluding header bytes)
 *   BENCH_RC_SOURCE_DONE  SOURCE finished — caller flushes out_buf then WS-closes
 *   -1                 handler not initialised / runtime gate off
 *   -1003              invalid sub-mode → caller MUST issue WS close 1003
 *   -2                 other internal error (CSPRNG fail in SOURCE mode)
 *
 * For ECHO and SOURCE, output bytes are written into c->out_buf up to
 * BENCH_OUT_CAP. The caller is responsible for draining out_buf and
 * resetting out_len to 0 between calls.
 *
 * SOURCE continuation: calling with data=NULL, len=0 when mode_byte_seen==1,
 * source_len_pending==0, and source_remaining>0 continues emitting CSPRNG
 * bytes without consuming any input (used by the C6 drain loop). */
int  bench_handler_init(bench_conn_t *c);
int  bench_handler_recv(bench_conn_t *c, const void *data, int len);

/* Read snapshot of the per-mode byte counters. Cheap (relaxed atomic loads). */
bench_stats_t bench_handler_get_stats(void);

/* ---------------------------------------------------------------- *
 * Per-connection session registry (used by the AR-S2 dispatch glue).*
 *                                                                    *
 * The bench data path BYPASSES obfuscated2/AES-CTR; bench bytes are *
 * routed at the AR-S2 (post-WS-unmask) layer directly through       *
 * bench_handler_recv. This deviates from the original Dev Notes hot-*
 * path-through-AES-CTR aspiration and is documented as a Story 1a-2 *
 * scope decision (revisit during Story 1a-3 if needed).             *
 *                                                                    *
 * The registry is keyed by (fd, generation) and is intentionally    *
 * small (BENCH_MAX_SESSIONS) — the bench server is dev-self-use and *
 * never carries production load.                                    *
 *                                                                    *
 * D4 THREADING ASSUMPTION: g_bench_slots[] is accessed only from the *
 * single net-worker thread (the same epoll-event thread that calls   *
 * bench_drain_connection). No locking is required as long as this    *
 * assumption holds. Verify before enabling multi-threaded dispatch   *
 * (e.g. SO_REUSEPORT workers).                                       *
 *                                                                    *
 * IDLE-REAPING (P11 / D1 R2): C7 originally mandated wiring          *
 * bench_session_destroy into the upstream connection-close path. To  *
 * keep upstream net code untouched (UPSTREAM.md keep-clean), the     *
 * registry instead reaps slots whose last_activity_ts is older than  *
 * BENCH_SLOT_IDLE_SEC at every bench_session_install call. Known     *
 * trade-off: a client whose frame arrives within the reap window     *
 * receives a TCP RST instead of a graceful WS-close — acceptable for *
 * dev-self-use; revisit if bench is ever exposed beyond loopback.    *
 * ---------------------------------------------------------------- */

#define BENCH_MAX_SESSIONS 64
#define BENCH_SLOT_IDLE_SEC 300  /* P11: reap slots idle for 5 minutes */

struct connection_info; /* upstream — opaque to bench-handler.h consumers */

/* Install a fresh bench session for connection `c`. Returns NULL if the
 * registry is full or the runtime gate is off. The returned bench_conn_t *
 * is owned by the registry and lives until bench_session_destroy(). */
bench_conn_t *bench_session_install(struct connection_info *c);

/* Look up the bench session attached to `c` (if any). */
bench_conn_t *bench_session_lookup(struct connection_info *c);

/* Tear down the bench session attached to `c` (no-op if absent). */
void          bench_session_destroy(struct connection_info *c);

/* P4 (R2): emit a WebSocket close frame directly into c->out. Exposed for
 * use by the AR-S2 dispatcher when it needs to refuse a bench session
 * (e.g. registry-full → emit 1013 Try Again Later). */
void          bench_emit_ws_close(struct connection_info *c, uint16_t status_code);

/* ---------------------------------------------------------------- *
 * Story 1a-9: bench-connection marker.                              *
 *                                                                    *
 * Tracks (fd, generation) pairs that have been assigned a BENCH     *
 * dispatch outcome. Separate from the session registry: the marker  *
 * survives bench_session_destroy() so the AR-S2 dispatch hook can   *
 * detect "connection was BENCH but session is gone" and close the   *
 * connection gracefully instead of falling through to MTProto.      *
 *                                                                    *
 * Without this guard, a premature bench_session_destroy() causes    *
 * bench_session_lookup() to return NULL; type3_dispatch_on_crypto_  *
 * init() is then called on random bench payload and may return      *
 * TYPE3_DISPATCH_ACCEPT, which opens a spurious proxy_pass outbound *
 * connection to a Telegram DC — the "proxy pass connection" storm   *
 * observed in the 1a-5 full-matrix run at ≥ 10 MiB SINK/ECHO.      *
 * ---------------------------------------------------------------- */

/* Mark connection c as a bench connection (call after TYPE3_DISPATCH_BENCH
 * + bench_session_install succeeds). No-op if the connection is already
 * marked or the table is full. */
void bench_connection_mark(struct connection_info *c);

/* Return 1 if connection c was ever marked as a bench connection, 0 otherwise. */
int  bench_connection_is_marked(struct connection_info *c);

/* Remove the bench-connection mark for c (called when the connection is
 * confirmed closed; clears the slot for reuse). */
void bench_connection_clear(struct connection_info *c);

#endif /* TELEPROTO3_BENCH */

#endif /* TELEPROTO3_SERVER_NET_BENCH_HANDLER_H */
