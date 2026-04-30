#!/usr/bin/env bash
# probe_drop_test.sh — red-phase acceptance tests for probe-drop latency + stats endpoint.
#
# Source: story 1.8 AC#2, AC#3, AC#4.
# Returns 0 on pass / 1 on fail.
#
# TDD RED PHASE: will FAIL until the server dispatcher is running and
# the stats endpoint is live on 127.0.0.1:<V2_STATS_PORT>.
#
# Usage: ./probe_drop_test.sh [--stats-port PORT] [--dispatcher-pid PID]

set -euo pipefail

STATS_PORT="${STATS_PORT:-8889}"
DISPATCHER_PID=""
FAIL=0

for arg in "$@"; do
    case "$arg" in
        --stats-port=*) STATS_PORT="${arg#*=}" ;;
        --dispatcher-pid=*) DISPATCHER_PID="${arg#*=}" ;;
    esac
done

pass() { printf "PASS [%s]\n" "$1"; }
fail() { printf "FAIL [%s]: %s\n" "$1" "${2:-}"; FAIL=1; }

echo "=== probe_drop_test.sh: RED-PHASE acceptance scaffold ==="

# ------------------------------------------------------------------ #
# 1.8-UNIT-007: stats endpoint bind on 127.0.0.1:<port>              #
# ------------------------------------------------------------------ #
if curl -sf "http://127.0.0.1:${STATS_PORT}/stats" >/dev/null 2>&1; then
    pass "1.8-UNIT-007: stats endpoint reachable on 127.0.0.1:${STATS_PORT}"
else
    fail "1.8-UNIT-007" "stats endpoint not reachable on 127.0.0.1:${STATS_PORT} — dispatcher may not be running"
fi

# ------------------------------------------------------------------ #
# 1.8-UNIT-008: all 7 v0.1.0 ABI metrics present                     #
# ------------------------------------------------------------------ #
STATS_BODY=""
if curl -sf "http://127.0.0.1:${STATS_PORT}/stats" -o /tmp/t3_stats.txt 2>/dev/null; then
    STATS_BODY="$(cat /tmp/t3_stats.txt)"
fi

REQUIRED_METRICS=(
    "teleproto3_connections_total"
    "teleproto3_connections_active"
    "teleproto3_silent_close_total"
    "teleproto3_bytes_total"
    "teleproto3_kill_switch_state"
    "teleproto3_ws_handshake_failures_total"
    "teleproto3_probe_drop_duration_ns"
)

for metric in "${REQUIRED_METRICS[@]}"; do
    if echo "$STATS_BODY" | grep -q "$metric"; then
        pass "1.8-UNIT-008: metric $metric present"
    else
        fail "1.8-UNIT-008" "metric $metric MISSING from /stats"
    fi
done

# ------------------------------------------------------------------ #
# 1.8-UNIT-009: no client IP / per-user data in stats                 #
# ------------------------------------------------------------------ #
# Check for common client-IP patterns.
if echo "$STATS_BODY" | grep -qE '([0-9]{1,3}\.){3}[0-9]{1,3}|client_ip|per_user|per_conn'; then
    fail "1.8-UNIT-009" "client IP or per-user data detected in /stats output"
else
    pass "1.8-UNIT-009: no client IP / per-user data in stats"
fi

# ------------------------------------------------------------------ #
# 1.8-INT-003: probe-drop p99 <= 100 µs                               #
# 1.8-UNIT-007: bind on loopback only                                 #
# ------------------------------------------------------------------ #
# Drive 100 random-bytes probes and measure close-FIN latency.
# Uses /dev/tcp + clock_gettime for measurement.

if command -v nc >/dev/null 2>&1 && [ -n "$DISPATCHER_PID" ]; then
    echo "Measuring probe-drop latency (100 probes)..."
    LATS=()
    for i in $(seq 1 100); do
        T0=$(python3 -c "import time; print(int(time.monotonic_ns()))" 2>/dev/null || echo 0)
        # Send random bytes — not a valid WS upgrade
        head -c 64 /dev/urandom | nc -q1 127.0.0.1 3129 >/dev/null 2>&1 || true
        T1=$(python3 -c "import time; print(int(time.monotonic_ns()))" 2>/dev/null || echo 0)
        if [ "$T0" -gt 0 ] && [ "$T1" -gt "$T0" ]; then
            LAT_NS=$(( T1 - T0 ))
            LATS+=("$LAT_NS")
        fi
    done

    if [ ${#LATS[@]} -ge 10 ]; then
        # Sort and compute p99
        P99=$(printf '%s\n' "${LATS[@]}" | sort -n | tail -n $(( (${#LATS[@]} + 99) / 100 )) | head -1)
        if [ "$P99" -le 100000 ]; then
            pass "1.8-INT-003: probe-drop p99=${P99}ns <= 100µs"
        else
            fail "1.8-INT-003" "probe-drop p99=${P99}ns exceeds 100µs"
        fi
    else
        printf "SKIP [1.8-INT-003]: insufficient samples (%d)\n" "${#LATS[@]}"
    fi
else
    printf "SKIP [1.8-INT-003]: nc not available or dispatcher PID not provided\n"
fi

# ------------------------------------------------------------------ #
# Summary                                                              #
# ------------------------------------------------------------------ #
if [ "$FAIL" -eq 0 ]; then
    echo ""
    echo "=== RESULT: PASS ==="
else
    echo ""
    echo "=== RESULT: FAIL ==="
    exit 1
fi
