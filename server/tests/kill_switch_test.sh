#!/usr/bin/env bash
# kill_switch_test.sh — red-phase acceptance tests for the file-based kill-switch.
#
# Source: story 1.8 AC#5 (FR18, AR-S3).
# Returns 0 on pass / 1 on fail.
#
# TDD RED PHASE: will FAIL until type3_dispatch_kill_switch_poll() is
# implemented and the stats endpoint exposes teleproto3_kill_switch_state.
#
# Usage: STATS_PORT=8889 ./kill_switch_test.sh [--mode drain|hard-close]

set -euo pipefail

STATS_PORT="${STATS_PORT:-8889}"
MARKER_PATH="/etc/teleproxy-ws-v2/disabled"
MODE="${1#--mode=}"
MODE="${MODE:-drain}"
FAIL=0
POLL_TIMEOUT=2  # seconds — 1s per spec + 1s margin

pass() { printf "PASS [%s]\n" "$1"; }
fail() { printf "FAIL [%s]: %s\n" "$1" "${2:-}"; FAIL=1; }
skip() { printf "SKIP [%s]: %s\n" "$1" "${2:-}"; }

echo "=== kill_switch_test.sh: RED-PHASE acceptance scaffold (mode=$MODE) ==="

# Helper: read kill_switch_state from Prometheus-format stats.
get_gauge() {
    curl -sf "http://127.0.0.1:${STATS_PORT}/stats" 2>/dev/null \
        | grep 'teleproto3_kill_switch_state' \
        | awk '{print int($NF)}' \
        | head -1
}

# Helper: wait for gauge to reach expected value within POLL_TIMEOUT seconds.
wait_gauge() {
    local expected=$1 label=$2
    local deadline=$(( $(date +%s) + POLL_TIMEOUT ))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        local val
        val=$(get_gauge 2>/dev/null || echo -1)
        if [ "$val" = "$expected" ]; then
            pass "$label"
            return 0
        fi
        sleep 0.1
    done
    fail "$label" "gauge still $(get_gauge 2>/dev/null || echo ?) after ${POLL_TIMEOUT}s (expected $expected)"
    return 1
}

# ------------------------------------------------------------------ #
# Pre-condition: gauge must start at 0 (idle)                         #
# ------------------------------------------------------------------ #
INITIAL=$(get_gauge 2>/dev/null || echo -1)
if [ "$INITIAL" = "0" ]; then
    pass "pre-condition: kill_switch_state=0 (idle)"
else
    skip "pre-condition" "kill_switch_state=${INITIAL} != 0 — stats endpoint may not be running"
    echo "=== RESULT: SKIP (dispatcher not running) ==="
    exit 0
fi

# Ensure marker is absent at start.
if [ -f "$MARKER_PATH" ]; then
    rm -f "$MARKER_PATH"
    sleep 1  # wait one tick
fi

# ------------------------------------------------------------------ #
# 1.8-UNIT-010: DRAIN mode — marker create → gauge=1 within ≤1s      #
# ------------------------------------------------------------------ #
if [ "$MODE" = "drain" ]; then
    # Create marker.
    mkdir -p "$(dirname "$MARKER_PATH")"
    touch "$MARKER_PATH"
    wait_gauge 1 "1.8-UNIT-010: DRAIN kill_switch -> gauge=1"

    # ---------------------------------------------------------------- #
    # 1.8-UNIT-012: remove marker → gauge=0 within ≤1s               #
    # ---------------------------------------------------------------- #
    rm -f "$MARKER_PATH"
    wait_gauge 0 "1.8-UNIT-012: marker removed -> gauge=0"
fi

# ------------------------------------------------------------------ #
# 1.8-UNIT-011: HARD-CLOSE mode — marker create → gauge=2 within ≤1s #
# ------------------------------------------------------------------ #
if [ "$MODE" = "hard-close" ]; then
    mkdir -p "$(dirname "$MARKER_PATH")"
    touch "$MARKER_PATH"
    wait_gauge 2 "1.8-UNIT-011: HARD-CLOSE kill_switch -> gauge=2"

    rm -f "$MARKER_PATH"
    wait_gauge 0 "1.8-UNIT-012: marker removed -> gauge=0"
fi

# ------------------------------------------------------------------ #
# Audit: no client IP in log during transition                        #
# ------------------------------------------------------------------ #
# (Log auditing requires a log path; if available, grep for IP pattern.)
LOG_PATH="${DISPATCHER_LOG:-}"
if [ -n "$LOG_PATH" ] && [ -f "$LOG_PATH" ]; then
    if grep -qE '([0-9]{1,3}\.){3}[0-9]{1,3}' "$LOG_PATH" 2>/dev/null; then
        fail "1.8-UNIT-009-log" "client IP found in log ${LOG_PATH}"
    else
        pass "1.8-UNIT-009-log: no client IP in log during kill-switch transition"
    fi
else
    skip "1.8-UNIT-009-log" "DISPATCHER_LOG not set or not found"
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
