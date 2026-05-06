/*
 * net-type3-stats.c — simple atomic counters for Type3 dispatch.
 *
 * TODO(server-v0.1.0): upgrade to per-CPU counters once wired into
 * upstream's job/thread model. The single-global variant here is
 * acceptable for v0.1.0 admin read-outs.
 */

#define _GNU_SOURCE 1
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <unistd.h>
#include <errno.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <fcntl.h>

#include "net-type3-stats.h"
#include "net-proxy-protocol.h"
#include "common/kprintf.h"

/* ------------------------------------------------------------------ *
 * Global metric store                                                 *
 * ------------------------------------------------------------------ */
type3_stats_t type3_stats = {0};

/* ------------------------------------------------------------------ *
 * Probe-drop histogram                                                *
 * Simple P-square approximation for p50/p95/p99 (online, O(1) space).*
 * For v0.1.0 we use a simplified sliding-window-min/max approach:    *
 * running mean + stddev allows approximate quantiles. For the test    *
 * harness we record cumulative sum + count; percentiles are estimated *
 * linearly from the sum assuming uniform distribution.                *
 * ------------------------------------------------------------------ */
void type3_stats_record_probe_drop_ns(uint64_t ns) {
    __atomic_add_fetch(&type3_stats.probe_drop_count, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&type3_stats.probe_drop_sum_ns, ns, __ATOMIC_RELAXED);

    /* Running approximate quantiles: use the Welford-style update where
     * we keep a simple exponential-moving estimate updated on each sample.
     * This is sufficient for a v0.1.0 health signal; real quantiles can be
     * computed offline from the sum/count pair. */
    uint64_t cur_p50 = __atomic_load_n(&type3_stats.probe_drop_p50_ns, __ATOMIC_RELAXED);
    uint64_t cur_p95 = __atomic_load_n(&type3_stats.probe_drop_p95_ns, __ATOMIC_RELAXED);
    uint64_t cur_p99 = __atomic_load_n(&type3_stats.probe_drop_p99_ns, __ATOMIC_RELAXED);

    /* EMA with alpha=0.1 */
    uint64_t new_p50 = (cur_p50 == 0) ? ns : (cur_p50 * 9 + ns) / 10;
    uint64_t new_p95 = (cur_p95 == 0) ? ns : (ns > cur_p95 ? (cur_p95 * 19 + ns) / 20 : cur_p95);
    uint64_t new_p99 = (cur_p99 == 0) ? ns : (ns > cur_p99 ? (cur_p99 * 99 + ns) / 100 : cur_p99);

    __atomic_store_n(&type3_stats.probe_drop_p50_ns, new_p50, __ATOMIC_RELAXED);
    __atomic_store_n(&type3_stats.probe_drop_p95_ns, new_p95, __ATOMIC_RELAXED);
    __atomic_store_n(&type3_stats.probe_drop_p99_ns, new_p99, __ATOMIC_RELAXED);
}

/* ------------------------------------------------------------------ *
 * Prometheus-text serialiser (~30 LoC, no extra deps)                *
 * ------------------------------------------------------------------ */

/* Append one counter line: "name{labels} value\n"
 * Returns new write position or NULL on overflow. */
static char *prom_counter(char *p, char *end, const char *name,
                          const char *label_k, const char *label_v,
                          uint64_t value) {
    int n;
    if (label_k) {
        n = snprintf(p, (size_t)(end - p),
                     "%s{%s=\"%s\"} %" PRIu64 "\n", name, label_k, label_v, value);
    } else {
        n = snprintf(p, (size_t)(end - p), "%s %" PRIu64 "\n", name, value);
    }
    if (n < 0 || n >= (int)(end - p)) return NULL;
    return p + n;
}

static char *prom_gauge(char *p, char *end, const char *name,
                        const char *label_k, const char *label_v,
                        int64_t value) {
    int n;
    if (label_k) {
        n = snprintf(p, (size_t)(end - p),
                     "%s{%s=\"%s\"} %" PRId64 "\n", name, label_k, label_v, value);
    } else {
        n = snprintf(p, (size_t)(end - p), "%s %" PRId64 "\n", name, value);
    }
    if (n < 0 || n >= (int)(end - p)) return NULL;
    return p + n;
}

/* Build Prometheus-text body into buf[0..bufsz).
 * Returns number of bytes written (not including NUL), or -1 on overflow. */
static int build_prom_body(char *buf, size_t bufsz) {
    char *p = buf;
    char *end = buf + bufsz - 1; /* leave room for NUL */

#define TRY(expr) do { p = (expr); if (!p) return -1; } while (0)

    /* teleproto3_connections_total */
    TRY(prom_counter(p, end, "teleproto3_connections_total", "command_type", "type1",   type3_stats.connections_type1_passthrough));
    TRY(prom_counter(p, end, "teleproto3_connections_total", "command_type", "type2",   type3_stats.connections_type2_passthrough));
    TRY(prom_counter(p, end, "teleproto3_connections_total", "command_type", "type3",
                     type3_stats.connections_type3_accept
                   + type3_stats.connections_type3_silent_close
                   + type3_stats.connections_type3_bad_header));

    /* teleproto3_connections_active */
    TRY(prom_gauge(p, end, "teleproto3_connections_active", NULL, NULL, type3_stats.connections_active));

    /* teleproto3_silent_close_total */
    TRY(prom_counter(p, end, "teleproto3_silent_close_total", "reason", "short",        type3_stats.silent_close_short));
    TRY(prom_counter(p, end, "teleproto3_silent_close_total", "reason", "unknown_cmd",  type3_stats.silent_close_unknown_cmd));
    TRY(prom_counter(p, end, "teleproto3_silent_close_total", "reason", "bad_version",  type3_stats.silent_close_bad_version));
    TRY(prom_counter(p, end, "teleproto3_silent_close_total", "reason", "bad_flags",    type3_stats.silent_close_bad_flags));
    TRY(prom_counter(p, end, "teleproto3_silent_close_total", "reason", "rate_limited", type3_stats.silent_close_rate_limited));

    /* teleproto3_bytes_total */
    TRY(prom_counter(p, end, "teleproto3_bytes_total", "direction", "in",  type3_stats.bytes_in));
    TRY(prom_counter(p, end, "teleproto3_bytes_total", "direction", "out", type3_stats.bytes_out));

    /* teleproto3_kill_switch_state */
    TRY(prom_gauge(p, end, "teleproto3_kill_switch_state", NULL, NULL, type3_stats.kill_switch_state));

    /* teleproto3_ws_handshake_failures_total */
    TRY(prom_counter(p, end, "teleproto3_ws_handshake_failures_total", "error_class", "bad_key", type3_stats.ws_handshake_failures_bad_key));
    TRY(prom_counter(p, end, "teleproto3_ws_handshake_failures_total", "error_class", "timeout", type3_stats.ws_handshake_failures_timeout));
    TRY(prom_counter(p, end, "teleproto3_ws_handshake_failures_total", "error_class", "other",   type3_stats.ws_handshake_failures_other));

    /* teleproto3_probe_drop_duration_ns quantile histogram */
    TRY(prom_counter(p, end, "teleproto3_probe_drop_duration_ns", "quantile", "0.5",  type3_stats.probe_drop_p50_ns));
    TRY(prom_counter(p, end, "teleproto3_probe_drop_duration_ns", "quantile", "0.95", type3_stats.probe_drop_p95_ns));
    TRY(prom_counter(p, end, "teleproto3_probe_drop_duration_ns", "quantile", "0.99", type3_stats.probe_drop_p99_ns));
    TRY(prom_counter(p, end, "teleproto3_probe_drop_duration_ns_count", NULL, NULL,   type3_stats.probe_drop_count));
    TRY(prom_counter(p, end, "teleproto3_probe_drop_duration_ns_sum",   NULL, NULL,   type3_stats.probe_drop_sum_ns));

    /* proxy_protocol_connections_total / proxy_protocol_errors_total
     * Globals declared in net-proxy-protocol.h, incremented by the parser
     * in net-tcp-rpc-ext-server.c. Exported here so the E2E test can verify
     * PROXY-protocol header parsing via /metrics scrape (story 2.10 AC#1). */
    TRY(prom_counter(p, end, "proxy_protocol_connections_total", NULL, NULL,
                     (uint64_t)proxy_protocol_connections_total));
    TRY(prom_counter(p, end, "proxy_protocol_errors_total",      NULL, NULL,
                     (uint64_t)proxy_protocol_errors_total));

#undef TRY

    *p = '\0';
    return (int)(p - buf);
}

/* ------------------------------------------------------------------ *
 * HTTP scrape endpoint                                                *
 * Binds 127.0.0.1:<port> (default 8889); non-blocking accept loop.   *
 * Loopback-only: no rate-limit needed (NFR19 / Cat 11).               *
 * ------------------------------------------------------------------ */
int type3_stats_emit(int sockfd) {
    char body[8192];
    int body_len = build_prom_body(body, sizeof(body));
    if (body_len < 0) {
        kprintf("type3_stats_emit: buffer overflow building Prometheus body\n");
        body_len = 0;
    }

    char header[256];
    int hlen = snprintf(header, sizeof(header),
                        "HTTP/1.1 200 OK\r\n"
                        "Content-Type: text/plain; version=0.0.4; charset=utf-8\r\n"
                        "Content-Length: %d\r\n"
                        "Connection: close\r\n"
                        "\r\n",
                        body_len);
    if (hlen < 0 || hlen >= (int)sizeof(header)) { return -1; }

    ssize_t written = 0;
    ssize_t r = write(sockfd, header, (size_t)hlen);
    if (r < 0) return -1;
    written += r;
    r = write(sockfd, body, (size_t)body_len);
    if (r < 0) return -1;
    written += r;
    return (int)written;
}

/* Open stats listener on 127.0.0.1:port; non-blocking, SO_REUSEADDR.
 * Returns listening fd on success, -1 on error. */
int type3_stats_listen(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        kprintf("type3_stats_listen: socket: %s\n", strerror(errno));
        return -1;
    }
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        kprintf("type3_stats_listen: fcntl: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); /* 127.0.0.1 only */

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        kprintf("type3_stats_listen: bind 127.0.0.1:%d: %s\n", port, strerror(errno));
        close(fd);
        return -1;
    }
    if (listen(fd, 8) < 0) {
        kprintf("type3_stats_listen: listen: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    vkprintf(1, "type3_stats: listening on 127.0.0.1:%d\n", port);
    return fd;
}
