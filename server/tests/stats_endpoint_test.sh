#!/bin/sh
# stats_endpoint_test.sh — verify Type3 Prometheus metrics format and NFR19 compliance.
#
# Story 4.6 Task 4.1 — AC#1 (Prometheus format), AC#3 (no per-user data).
#
# Usage:
#   STATS_PORT=8888 ./tests/stats_endpoint_test.sh
#
# The script starts a teleproxy instance in the background, curls /stats,
# validates the metric names and format, checks for absence of per-IP data,
# then exits 0 on pass / 1 on fail.
#
# Required env vars (with defaults):
#   STATS_PORT      — port to curl (default 8888)
#   TELEPROXY_BIN   — path to the teleproxy binary (default objs/bin/teleproxy)
#   TELEPROXY_SECRET— 32-hex-char secret for the proxy; auto-generated if unset
#
# The test is designed to run locally (no Docker).  CI that requires Docker
# should use the docker-compose integration test wrappers instead.

set -eu

STATS_PORT="${STATS_PORT:-8888}"
TELEPROXY_BIN="${TELEPROXY_BIN:-$(dirname "$0")/../objs/bin/teleproxy}"
TELEPROXY_SECRET="${TELEPROXY_SECRET:-$(head -c 16 /dev/urandom | od -An -tx1 | tr -d ' \n' 2>/dev/null || openssl rand -hex 16)}"

PASS=0
FAIL=0

pass() { printf 'PASS [%s]\n' "$1"; PASS=$((PASS + 1)); }
fail() { printf 'FAIL [%s]: %s\n' "$1" "$2" >&2; FAIL=$((FAIL + 1)); }

# ---------------------------------------------------------------------------
# Guard: binary must exist
# ---------------------------------------------------------------------------
if [ ! -x "$TELEPROXY_BIN" ]; then
    fail "4.6-ENV-001" "teleproxy binary not found at $TELEPROXY_BIN — run 'make' first"
    printf '\n=== RESULT: FAIL (%d failures) ===\n' "$FAIL" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# Start proxy in background; bind on an ephemeral port to avoid conflicts
# ---------------------------------------------------------------------------
PROXY_PORT=13131
TMPDIR_TEST=$(mktemp -d)
LOG_FILE="$TMPDIR_TEST/teleproxy.log"
PID_FILE="$TMPDIR_TEST/teleproxy.pid"

cleanup() {
    if [ -f "$PID_FILE" ]; then
        kill "$(cat "$PID_FILE")" 2>/dev/null || true
    fi
    rm -rf "$TMPDIR_TEST"
}
trap cleanup EXIT INT TERM

# Launch with http_stats enabled and custom stats port
"$TELEPROXY_BIN" \
    -H "$PROXY_PORT" \
    -S "$TELEPROXY_SECRET" \
    --http-stats \
    -p "$STATS_PORT" \
    >"$LOG_FILE" 2>&1 &
echo $! >"$PID_FILE"

# Wait for the stats port to open (up to 5 s)
i=0
while [ "$i" -lt 50 ]; do
    if curl -sf "http://127.0.0.1:$STATS_PORT/stats" >/dev/null 2>&1; then
        break
    fi
    sleep 0.1
    i=$((i + 1))
done

STATS_OUTPUT=$(curl -sf "http://127.0.0.1:$STATS_PORT/stats" 2>/dev/null || true)

if [ -z "$STATS_OUTPUT" ]; then
    fail "4.6-ENV-002" "could not reach http://127.0.0.1:$STATS_PORT/stats after 5s"
    printf '\n=== RESULT: FAIL (%d failures) ===\n' "$FAIL" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# AC#1 — Prometheus text format
# ---------------------------------------------------------------------------
if printf '%s\n' "$STATS_OUTPUT" | grep -q '^# HELP teleproto3_'; then
    pass "4.6-AC1-001: HELP lines present for teleproto3 metrics"
else
    fail "4.6-AC1-001" "no '# HELP teleproto3_*' lines in /stats output"
fi

if printf '%s\n' "$STATS_OUTPUT" | grep -q '^# TYPE teleproto3_'; then
    pass "4.6-AC1-002: TYPE lines present for teleproto3 metrics"
else
    fail "4.6-AC1-002" "no '# TYPE teleproto3_*' lines in /stats output"
fi

# ---------------------------------------------------------------------------
# AC#2 — required metric names present
# ---------------------------------------------------------------------------
check_metric() {
    label="$1"; metric="$2"
    if printf '%s\n' "$STATS_OUTPUT" | grep -q "^${metric}"; then
        pass "$label: metric '$metric' present"
    else
        fail "$label" "metric '$metric' missing from /stats output"
    fi
}

check_metric "4.6-AC2-001" "teleproto3_connections_active"
check_metric "4.6-AC2-002" 'teleproto3_connections_total{command_type='
check_metric "4.6-AC2-003" 'teleproto3_silent_close_total{reason='
check_metric "4.6-AC2-004" 'teleproto3_kill_switch_state'
check_metric "4.6-AC2-005" 'teleproto3_bytes_total{direction='
check_metric "4.6-AC2-006" 'teleproto3_ws_handshake_failures_total{error_class='
check_metric "4.6-AC2-007" 'teleproto3_probe_drop_duration_ns{quantile='

# Check all required command_type/result label values
check_metric "4.6-AC2-008" 'teleproto3_connections_total{command_type="Type3",result="accept"}'
check_metric "4.6-AC2-009" 'teleproto3_connections_total{command_type="Type3",result="silent_close"}'
check_metric "4.6-AC2-010" 'teleproto3_connections_total{command_type="Type3",result="bad_header"}'

# Check quantile values
check_metric "4.6-AC2-011" 'teleproto3_probe_drop_duration_ns{quantile="0.5"}'
check_metric "4.6-AC2-012" 'teleproto3_probe_drop_duration_ns{quantile="0.95"}'
check_metric "4.6-AC2-013" 'teleproto3_probe_drop_duration_ns{quantile="0.99"}'

# ---------------------------------------------------------------------------
# AC#3 — no per-user data (NFR19)
# ---------------------------------------------------------------------------
# Per-user data patterns: IP addresses in labels, connection IDs, secret bytes
if printf '%s\n' "$STATS_OUTPUT" | grep -qE 'ip="[0-9]+\.[0-9]+|ip="\['; then
    fail "4.6-AC3-001" "per-user IP addresses found in teleproto3 metric labels"
else
    pass "4.6-AC3-001: no per-IP labels in teleproto3 metrics"
fi

# Ensure no secret bytes appear (hex blobs > 8 chars not in known safe context)
# We check that no teleproto3_* metric line contains a raw secret-looking hex blob
if printf '%s\n' "$STATS_OUTPUT" | grep '^teleproto3_' | grep -qE '[0-9a-f]{32}'; then
    fail "4.6-AC3-002" "possible secret bytes found in teleproto3 metric output"
else
    pass "4.6-AC3-002: no secret-length hex blobs in teleproto3 metrics"
fi

# ---------------------------------------------------------------------------
# AC#5 — metric names identical to Story 1-8 ABI (spot-check exact names)
# ---------------------------------------------------------------------------
for m in \
    "teleproto3_connections_active" \
    "teleproto3_kill_switch_state" \
    "teleproto3_bytes_total" \
    "teleproto3_silent_close_total" \
    "teleproto3_ws_handshake_failures_total" \
    "teleproto3_probe_drop_duration_ns"
do
    if printf '%s\n' "$STATS_OUTPUT" | grep -q "^${m}"; then
        pass "4.6-AC5: metric name '${m}' matches story 1-8 ABI"
    else
        fail "4.6-AC5" "ABI metric '${m}' missing — do not rename without dual-emit window"
    fi
done

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
printf '\n'
if [ "$FAIL" -gt 0 ]; then
    printf '=== RESULT: FAIL (%d failures, %d passes) ===\n' "$FAIL" "$PASS" >&2
    exit 1
fi
printf '=== RESULT: PASS (%d checks) ===\n' "$PASS"
exit 0
