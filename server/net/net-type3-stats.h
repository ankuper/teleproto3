/*
 * net-type3-stats.h — admin-local counters for Type3 dispatch.
 *
 * Rule (Cat 11): server-local observability ONLY. No remote emission.
 * Counters are readable via the existing MTProxy admin interface.
 *
 * Normative note: counter names appearing in logs/admin UI MUST NOT
 * leak information useful to a probe (see spec/anti-probe.md §4).
 */

#ifndef TELEPROTO3_SERVER_NET_TYPE3_STATS_H
#define TELEPROTO3_SERVER_NET_TYPE3_STATS_H

#include <stdint.h>

/* ------------------------------------------------------------------ *
 * v0.1.0 metric set — ABI commitment.                                *
 * Names are consumed verbatim by story 1.9 conformance harness.      *
 * Renaming in v0.2.0 requires release-note deprecation + dual-emit.  *
 * ------------------------------------------------------------------ */

/* Aggregate connection counters.
 * Label: command_type ∈ {type1, type2, type3}
 *        result       ∈ {accept, passthrough, silent_close, bad_header} */
typedef struct {
    /* teleproto3_connections_total{command_type=type1, result=passthrough} */
    uint64_t connections_type1_passthrough;
    /* teleproto3_connections_total{command_type=type2, result=passthrough} */
    uint64_t connections_type2_passthrough;
    /* teleproto3_connections_total{command_type=type3, result=accept} */
    uint64_t connections_type3_accept;
    /* teleproto3_connections_total{command_type=type3, result=silent_close} */
    uint64_t connections_type3_silent_close;
    /* teleproto3_connections_total{command_type=type3, result=bad_header} */
    uint64_t connections_type3_bad_header;
    /* teleproto3_connections_active gauge */
    int64_t  connections_active;

    /* teleproto3_silent_close_total{reason=*} counters
     * reason ∈ {short, unknown_cmd, bad_version, bad_flags, rate_limited} */
    uint64_t silent_close_short;
    uint64_t silent_close_unknown_cmd;
    uint64_t silent_close_bad_version;
    uint64_t silent_close_bad_flags;
    uint64_t silent_close_rate_limited;

    /* teleproto3_bytes_total{direction=*} counters */
    uint64_t bytes_in;
    uint64_t bytes_out;

    /* teleproto3_kill_switch_state gauge: 0=idle, 1=draining, 2=hard_closed */
    int      kill_switch_state;

    /* teleproto3_ws_handshake_failures_total{error_class=*} */
    uint64_t ws_handshake_failures_bad_key;
    uint64_t ws_handshake_failures_timeout;
    uint64_t ws_handshake_failures_other;

    /* teleproto3_probe_drop_duration_ns histogram buckets (nanoseconds).
     * Populated by type3_stats_record_probe_drop_ns(). */
    uint64_t probe_drop_count;
    uint64_t probe_drop_sum_ns;
    uint64_t probe_drop_p50_ns;   /* running approx; updated on each sample */
    uint64_t probe_drop_p95_ns;
    uint64_t probe_drop_p99_ns;
} type3_stats_t;

extern type3_stats_t type3_stats;

/* ------------------------------------------------------------------ *
 * Inline atomic mutators (single-global, __ATOMIC_RELAXED — v0.1.0). *
 * Per-CPU sharding deferred to v0.1.1.                                *
 * ------------------------------------------------------------------ */

static inline void type3_stats_incr_type1_passthrough(void) {
    __atomic_add_fetch(&type3_stats.connections_type1_passthrough, 1, __ATOMIC_RELAXED);
}
static inline void type3_stats_incr_type2_passthrough(void) {
    __atomic_add_fetch(&type3_stats.connections_type2_passthrough, 1, __ATOMIC_RELAXED);
}
static inline void type3_stats_incr_type3_accept(void) {
    __atomic_add_fetch(&type3_stats.connections_type3_accept, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&type3_stats.connections_active,       1, __ATOMIC_RELAXED);
}
static inline void type3_stats_incr_type3_silent_close(void) {
    __atomic_add_fetch(&type3_stats.connections_type3_silent_close, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&type3_stats.connections_active,             -1, __ATOMIC_RELAXED);
}
static inline void type3_stats_incr_type3_bad_header(void) {
    __atomic_add_fetch(&type3_stats.connections_type3_bad_header, 1, __ATOMIC_RELAXED);
}
static inline void type3_stats_decr_active(void) {
    __atomic_add_fetch(&type3_stats.connections_active, -1, __ATOMIC_RELAXED);
}

/* Backward-compat aliases used by legacy code paths */
static inline void type3_stats_incr_accept(void)       { type3_stats_incr_type3_accept(); }
static inline void type3_stats_incr_passthrough(void)  { type3_stats_incr_type1_passthrough(); }
static inline void type3_stats_incr_bad_header(void)   { type3_stats_incr_type3_bad_header(); }
static inline void type3_stats_incr_silent_close(void) {
    __atomic_add_fetch(&type3_stats.connections_type3_silent_close, 1, __ATOMIC_RELAXED);
}

/* Reason-labelled silent-close increments */
static inline void type3_stats_silent_close_short(void)       { __atomic_add_fetch(&type3_stats.silent_close_short,        1, __ATOMIC_RELAXED); }
static inline void type3_stats_silent_close_unknown_cmd(void) { __atomic_add_fetch(&type3_stats.silent_close_unknown_cmd,  1, __ATOMIC_RELAXED); }
static inline void type3_stats_silent_close_bad_version(void) { __atomic_add_fetch(&type3_stats.silent_close_bad_version,  1, __ATOMIC_RELAXED); }
static inline void type3_stats_silent_close_bad_flags(void)   { __atomic_add_fetch(&type3_stats.silent_close_bad_flags,    1, __ATOMIC_RELAXED); }
static inline void type3_stats_silent_close_rate_limited(void){ __atomic_add_fetch(&type3_stats.silent_close_rate_limited, 1, __ATOMIC_RELAXED); }

/* Bytes accounting */
static inline void type3_stats_add_bytes_in(uint64_t n)  { __atomic_add_fetch(&type3_stats.bytes_in,  n, __ATOMIC_RELAXED); }
static inline void type3_stats_add_bytes_out(uint64_t n) { __atomic_add_fetch(&type3_stats.bytes_out, n, __ATOMIC_RELAXED); }

/* WS handshake failure increments */
static inline void type3_stats_ws_fail_bad_key(void) { __atomic_add_fetch(&type3_stats.ws_handshake_failures_bad_key, 1, __ATOMIC_RELAXED); }
static inline void type3_stats_ws_fail_timeout(void) { __atomic_add_fetch(&type3_stats.ws_handshake_failures_timeout, 1, __ATOMIC_RELAXED); }
static inline void type3_stats_ws_fail_other(void)   { __atomic_add_fetch(&type3_stats.ws_handshake_failures_other,   1, __ATOMIC_RELAXED); }

/* ------------------------------------------------------------------ *
 * HTTP scrape endpoint.                                               *
 * Binds 127.0.0.1:<port> (default 8889, configurable via --stats-port). *
 * Emits Prometheus text format; closes after each scrape.             *
 * Loopback-only: no rate-limit needed (Cat 11, NFR19).                *
 * ------------------------------------------------------------------ */

/* Initialise stats listener on 127.0.0.1:port (call once at startup).
 * Returns 0 on success, -1 on error (logs reason via kprintf). */
int type3_stats_listen(int port);

/* Emit Prometheus-text counters to sockfd; call from I/O handler.
 * Returns bytes written on success, -1 on error. */
int type3_stats_emit(int sockfd);

/* Record a probe-drop latency sample (nanoseconds from first-byte-in to FIN).
 * Updates probe_drop_count, sum, and running p50/p95/p99 estimates. */
void type3_stats_record_probe_drop_ns(uint64_t ns);

#endif /* TELEPROTO3_SERVER_NET_TYPE3_STATS_H */
