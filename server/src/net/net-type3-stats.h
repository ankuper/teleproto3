/*
 * net-type3-stats.h — Type3 aggregate-only observability counters.
 *
 * Declares the v0.1.0 metric set consumed by the admin Prometheus scraper
 * (GET /stats). Names are the ABI commitment: do NOT rename without a
 * transitional dual-emit window per docs/epic-1-style-guide.md §10.
 *
 * NFR19: No per-user IP, no per-connection identifier, no secret bytes.
 * Loopback-only scrape (127.0.0.1) is the sole and sufficient defence.
 *
 * <!-- ban-list-doc: proxy|proxy-server|bypass|censorship|прокси|پروکسی|代理 -->
 */

#ifndef NET_TYPE3_STATS_H
#define NET_TYPE3_STATS_H

#include <stdint.h>

/* ------------------------------------------------------------------ */
/* v0.1.0 metric set — atomic globals (ATOMIC_RELAXED, single-global) */
/* ------------------------------------------------------------------ */

/* teleproto3_connections_active — gauge: Type3 sessions currently open */
extern long long t3_stat_connections_active;

/* teleproto3_connections_total{command_type,result} — counters */
extern long long t3_stat_connections_total_type1_accept;
extern long long t3_stat_connections_total_type2_accept;
extern long long t3_stat_connections_total_type3_accept;
extern long long t3_stat_connections_total_type1_passthrough;
extern long long t3_stat_connections_total_type2_passthrough;
extern long long t3_stat_connections_total_type3_passthrough;
extern long long t3_stat_connections_total_type3_silent_close;
extern long long t3_stat_connections_total_type3_bad_header;

/* teleproto3_silent_close_total{reason} — counters */
extern long long t3_stat_silent_close_short;
extern long long t3_stat_silent_close_unknown_cmd;
extern long long t3_stat_silent_close_bad_version;
extern long long t3_stat_silent_close_bad_flags;
extern long long t3_stat_silent_close_rate_limited;

/* teleproto3_bytes_total{direction} — counters */
extern long long t3_stat_bytes_in;
extern long long t3_stat_bytes_out;

/* teleproto3_kill_switch_state — gauge: 0=idle, 1=draining, 2=hard_closed */
extern long long t3_stat_kill_switch_state;

/* teleproto3_ws_handshake_failures_total{error_class} — counters */
extern long long t3_stat_ws_handshake_fail_upgrade;
extern long long t3_stat_ws_handshake_fail_key;
extern long long t3_stat_ws_handshake_fail_timeout;
extern long long t3_stat_ws_handshake_fail_other;

/* teleproto3_probe_drop_duration_ns histogram samples (lock-free reservoir
 * for p50/p95/p99 quantile approximation via exponential decay).
 * Updated by the dispatcher on each silent-close. */
extern long long t3_stat_probe_drop_ns_p50;
extern long long t3_stat_probe_drop_ns_p95;
extern long long t3_stat_probe_drop_ns_p99;

/* ------------------------------------------------------------------ */
/* Increment helpers (ATOMIC_RELAXED — aggregate counters, no happens-  */
/* before requirement between writers and readers at scrape time)       */
/* ------------------------------------------------------------------ */
#define T3_STAT_INC(counter)        __atomic_add_fetch(&(counter), 1LL,  __ATOMIC_RELAXED)
#define T3_STAT_DEC(counter)        __atomic_sub_fetch(&(counter), 1LL,  __ATOMIC_RELAXED)
#define T3_STAT_ADD(counter, n)     __atomic_add_fetch(&(counter), (long long)(n), __ATOMIC_RELAXED)
#define T3_STAT_STORE(counter, val) __atomic_store_n(&(counter), (long long)(val), __ATOMIC_RELAXED)
#define T3_STAT_LOAD(counter)       __atomic_load_n(&(counter), __ATOMIC_RELAXED)

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

/*
 * type3_stats_record_probe_drop_ns — record one probe-drop latency sample
 * (nanoseconds, measured by the dispatcher from first-byte-in to FIN-out).
 * Uses a simple running-min exponential filter suitable for low-rate updates.
 */
void type3_stats_record_probe_drop_ns (long long ns);

/*
 * type3_stats_emit_prometheus — append Type3 Prometheus-text-format lines
 * to the caller's stats_buffer_t.  Called from mtfront_prepare_prometheus_stats()
 * in mtproto-proxy-stats.c so that Type3 metrics appear in the existing
 * upstream /stats endpoint (no separate listener per Dev Notes §"upstream stats
 * integration").
 */
struct stats_buffer;
void type3_stats_emit_prometheus (struct stats_buffer *sb);

#endif /* NET_TYPE3_STATS_H */
