/*
 * net-type3-stats.c — Type3 aggregate-only observability counters.
 *
 * Implements the v0.1.0 Prometheus metric set (teleproto3_* prefix) defined
 * in net-type3-stats.h and doc'd in story 4.6 AC#2.  Metric names are an
 * ABI commitment consumed by the conformance harness (story 1.9).
 *
 * NFR19 compliance: no per-user IP, no per-connection ID, no secret bytes
 * are stored or emitted.  The scrape endpoint is the existing upstream
 * /stats HTTP handler (127.0.0.1-only binding — loopback is the sole and
 * sufficient defence; no nginx rate-limiting is added per Dev Notes).
 *
 * Probe-drop histogram uses a 3-bin exponential-decay approximation
 * (p50/p95/p99).  Single-global approach is intentional for v0.1.0;
 * per-CPU sharding is deferred to v0.1.1 per the existing TODO pattern.
 *
 * <!-- ban-list-doc: proxy|proxy-server|bypass|censorship|прокси|پروکسی|代理 -->
 */

#include "net/net-type3-stats.h"
#include "common/common-stats.h"

#include <stdint.h>

/* ------------------------------------------------------------------ */
/* v0.1.0 metric globals — all ATOMIC_RELAXED                          */
/* ------------------------------------------------------------------ */

/* teleproto3_connections_active */
long long t3_stat_connections_active = 0;

/* teleproto3_connections_total{command_type,result} */
long long t3_stat_connections_total_type1_accept      = 0;
long long t3_stat_connections_total_type2_accept      = 0;
long long t3_stat_connections_total_type3_accept      = 0;
long long t3_stat_connections_total_type1_passthrough = 0;
long long t3_stat_connections_total_type2_passthrough = 0;
long long t3_stat_connections_total_type3_passthrough = 0;
long long t3_stat_connections_total_type3_silent_close= 0;
long long t3_stat_connections_total_type3_bad_header  = 0;

/* teleproto3_silent_close_total{reason} */
long long t3_stat_silent_close_short       = 0;
long long t3_stat_silent_close_unknown_cmd = 0;
long long t3_stat_silent_close_bad_version = 0;
long long t3_stat_silent_close_bad_flags   = 0;
long long t3_stat_silent_close_rate_limited= 0;

/* teleproto3_bytes_total{direction} */
long long t3_stat_bytes_in  = 0;
long long t3_stat_bytes_out = 0;

/* teleproto3_kill_switch_state — 0=idle, 1=draining, 2=hard_closed */
long long t3_stat_kill_switch_state = 0;

/* teleproto3_ws_handshake_failures_total{error_class} */
long long t3_stat_ws_handshake_fail_upgrade = 0;
long long t3_stat_ws_handshake_fail_key     = 0;
long long t3_stat_ws_handshake_fail_timeout = 0;
long long t3_stat_ws_handshake_fail_other   = 0;

/* teleproto3_probe_drop_duration_ns histogram quantile estimates */
long long t3_stat_probe_drop_ns_p50 = 0;
long long t3_stat_probe_drop_ns_p95 = 0;
long long t3_stat_probe_drop_ns_p99 = 0;

/* ------------------------------------------------------------------ */
/* Probe-drop latency histogram (simple exponential moving estimate)   */
/* ------------------------------------------------------------------ */

/*
 * type3_stats_record_probe_drop_ns — record one silent-close latency sample.
 *
 * We maintain running estimates of p50, p95, p99 using a fixed-step stochastic
 * gradient approach (Robbins-Monroe).  The step sizes below give stable
 * convergence after ~100 samples.  Suitable for single-process v0.1.0;
 * under workers the per-CPU shard aggregation is deferred to v0.1.1.
 *
 * No lock needed: monotone CAS-free update under ATOMIC_RELAXED is safe for
 * a scalar estimate that degrades gracefully under races (worst case: one
 * lost update per core burst — acceptable for an observability histogram).
 */
static void update_histogram_bin(long long *bin, long long ns, long long up_step, long long down_step) {
    long long expected = __atomic_load_n(bin, __ATOMIC_RELAXED);
    long long desired;
    do {
        desired = expected + ((ns > expected) ? up_step : -down_step);
        if (desired < 0) { desired = 0; }
        if (desired > 500000000LL) { desired = 500000000LL; }
    } while (!__atomic_compare_exchange_n(bin, &expected, desired, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED));
}

void type3_stats_record_probe_drop_ns (long long ns) {
    update_histogram_bin(&t3_stat_probe_drop_ns_p50, ns, 1000LL, 1000LL);
    update_histogram_bin(&t3_stat_probe_drop_ns_p95, ns, 19000LL, 1000LL);
    update_histogram_bin(&t3_stat_probe_drop_ns_p99, ns, 99000LL, 1000LL);
}

/* ------------------------------------------------------------------ */
/* Prometheus emission — appended to the upstream /stats endpoint      */
/* ------------------------------------------------------------------ */

/*
 * type3_stats_emit_prometheus — append teleproto3_* metrics to sb.
 *
 * Called from mtfront_prepare_prometheus_stats() in mtproto-proxy-stats.c
 * so that Type3 metrics appear in the existing /stats endpoint.  No
 * separate listener is opened (Dev Notes §"upstream stats integration").
 *
 * Metric names are the v0.1.0 ABI.  Do NOT rename without a transitional
 * dual-emit window of one minor version and a release-notes deprecation.
 */
void type3_stats_emit_prometheus (stats_buffer_t *sb) {
    /* teleproto3_connections_active */
    sb_printf (sb,
        "# HELP teleproto3_connections_active Type3 sessions currently active.\n"
        "# TYPE teleproto3_connections_active gauge\n"
        "teleproto3_connections_active %lld\n",
        T3_STAT_LOAD (t3_stat_connections_active));

    /* teleproto3_connections_total{command_type,result} */
    sb_printf (sb,
        "# HELP teleproto3_connections_total Connections handled by command type and result.\n"
        "# TYPE teleproto3_connections_total counter\n"
        "teleproto3_connections_total{command_type=\"Type1\",result=\"accept\"} %lld\n"
        "teleproto3_connections_total{command_type=\"Type2\",result=\"accept\"} %lld\n"
        "teleproto3_connections_total{command_type=\"Type3\",result=\"accept\"} %lld\n"
        "teleproto3_connections_total{command_type=\"Type1\",result=\"passthrough\"} %lld\n"
        "teleproto3_connections_total{command_type=\"Type2\",result=\"passthrough\"} %lld\n"
        "teleproto3_connections_total{command_type=\"Type3\",result=\"passthrough\"} %lld\n"
        "teleproto3_connections_total{command_type=\"Type3\",result=\"silent_close\"} %lld\n"
        "teleproto3_connections_total{command_type=\"Type3\",result=\"bad_header\"} %lld\n",
        T3_STAT_LOAD (t3_stat_connections_total_type1_accept),
        T3_STAT_LOAD (t3_stat_connections_total_type2_accept),
        T3_STAT_LOAD (t3_stat_connections_total_type3_accept),
        T3_STAT_LOAD (t3_stat_connections_total_type1_passthrough),
        T3_STAT_LOAD (t3_stat_connections_total_type2_passthrough),
        T3_STAT_LOAD (t3_stat_connections_total_type3_passthrough),
        T3_STAT_LOAD (t3_stat_connections_total_type3_silent_close),
        T3_STAT_LOAD (t3_stat_connections_total_type3_bad_header));

    /* teleproto3_silent_close_total{reason} */
    sb_printf (sb,
        "# HELP teleproto3_silent_close_total Silent-close events by reason.\n"
        "# TYPE teleproto3_silent_close_total counter\n"
        "teleproto3_silent_close_total{reason=\"short\"} %lld\n"
        "teleproto3_silent_close_total{reason=\"unknown_cmd\"} %lld\n"
        "teleproto3_silent_close_total{reason=\"bad_version\"} %lld\n"
        "teleproto3_silent_close_total{reason=\"bad_flags\"} %lld\n"
        "teleproto3_silent_close_total{reason=\"rate_limited\"} %lld\n",
        T3_STAT_LOAD (t3_stat_silent_close_short),
        T3_STAT_LOAD (t3_stat_silent_close_unknown_cmd),
        T3_STAT_LOAD (t3_stat_silent_close_bad_version),
        T3_STAT_LOAD (t3_stat_silent_close_bad_flags),
        T3_STAT_LOAD (t3_stat_silent_close_rate_limited));

    /* teleproto3_bytes_total{direction} */
    sb_printf (sb,
        "# HELP teleproto3_bytes_total Bytes transferred by direction.\n"
        "# TYPE teleproto3_bytes_total counter\n"
        "teleproto3_bytes_total{direction=\"in\"} %lld\n"
        "teleproto3_bytes_total{direction=\"out\"} %lld\n",
        T3_STAT_LOAD (t3_stat_bytes_in),
        T3_STAT_LOAD (t3_stat_bytes_out));

    /* teleproto3_kill_switch_state */
    sb_printf (sb,
        "# HELP teleproto3_kill_switch_state Kill-switch state: 0=idle 1=draining 2=hard_closed.\n"
        "# TYPE teleproto3_kill_switch_state gauge\n"
        "teleproto3_kill_switch_state %lld\n",
        T3_STAT_LOAD (t3_stat_kill_switch_state));

    /* teleproto3_ws_handshake_failures_total{error_class} */
    sb_printf (sb,
        "# HELP teleproto3_ws_handshake_failures_total WebSocket handshake failures by error class.\n"
        "# TYPE teleproto3_ws_handshake_failures_total counter\n"
        "teleproto3_ws_handshake_failures_total{error_class=\"upgrade\"} %lld\n"
        "teleproto3_ws_handshake_failures_total{error_class=\"key\"} %lld\n"
        "teleproto3_ws_handshake_failures_total{error_class=\"timeout\"} %lld\n"
        "teleproto3_ws_handshake_failures_total{error_class=\"other\"} %lld\n",
        T3_STAT_LOAD (t3_stat_ws_handshake_fail_upgrade),
        T3_STAT_LOAD (t3_stat_ws_handshake_fail_key),
        T3_STAT_LOAD (t3_stat_ws_handshake_fail_timeout),
        T3_STAT_LOAD (t3_stat_ws_handshake_fail_other));

    /* teleproto3_probe_drop_duration_ns{quantile} */
    sb_printf (sb,
        "# HELP teleproto3_probe_drop_duration_ns Probe-drop silent-close latency histogram (ns).\n"
        "# TYPE teleproto3_probe_drop_duration_ns histogram\n"
        "teleproto3_probe_drop_duration_ns{quantile=\"0.5\"} %lld\n"
        "teleproto3_probe_drop_duration_ns{quantile=\"0.95\"} %lld\n"
        "teleproto3_probe_drop_duration_ns{quantile=\"0.99\"} %lld\n",
        T3_STAT_LOAD (t3_stat_probe_drop_ns_p50),
        T3_STAT_LOAD (t3_stat_probe_drop_ns_p95),
        T3_STAT_LOAD (t3_stat_probe_drop_ns_p99));
}
