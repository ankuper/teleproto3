#!/bin/sh
# e2e-smoke.sh — End-to-end smoke test for teleproxy v2 (Story 4.10)
#
# Tests: WS upgrade, stats endpoint, DC connectivity, kill-switch, PROXY protocol
#
# Usage:
#   TELEPROXY_PORT=3130 STATS_PORT=8888 STATE_DIR=/etc/teleproxy-ws-v2 bash e2e-smoke.sh
#
# Environment:
#   TELEPROXY_HOST   — teleproxy host (default: 127.0.0.1)
#   TELEPROXY_PORT   — teleproxy WS port (default: 3130)
#   STATS_PORT       — stats HTTP port (default: 8888)
#   WS_PATH          — WebSocket path (default: /)
#   STATE_DIR        — state dir for kill-switch test (default: /etc/teleproxy-ws-v2)
#   PROXY_PROTOCOL   — if "true", run PROXY protocol test (default: false)
#   SKIP_DC_CHECK    — if "true", skip DC connectivity assertion (default: false)
#
# Exit codes: 0 = all critical tests passed, 1 = failures

TELEPROXY_HOST="${TELEPROXY_HOST:-127.0.0.1}"
TELEPROXY_PORT="${TELEPROXY_PORT:-3130}"
STATS_PORT="${STATS_PORT:-8888}"
WS_PATH="${WS_PATH:-/}"
STATE_DIR="${STATE_DIR:-/etc/teleproxy-ws-v2}"
PROXY_PROTOCOL="${PROXY_PROTOCOL:-false}"
SKIP_DC_CHECK="${SKIP_DC_CHECK:-false}"

PASS=0
FAIL=0

pass() { PASS=$((PASS+1)); printf '  PASS [%s]\n' "$1"; }
fail() { FAIL=$((FAIL+1)); printf '  FAIL [%s]: %s\n' "$1" "${2:-}" >&2; }

START_TS="$(date +%s 2>/dev/null || echo 0)"

printf '\n=== teleproxy v2 E2E smoke test ===\n'
printf '  Host:      %s:%s%s\n' "$TELEPROXY_HOST" "$TELEPROXY_PORT" "$WS_PATH"
printf '  Stats:     http://%s:%s/stats\n' "$TELEPROXY_HOST" "$STATS_PORT"
printf '  State dir: %s\n\n' "$STATE_DIR"

# ---------------------------------------------------------------------------
# AC#3 — Stats endpoint (checked first so we can gate other tests)
# ---------------------------------------------------------------------------
printf '--- Test 1: Stats endpoint (AC#3) ---\n'
stats_output="$(curl -sf --max-time 10 "http://${TELEPROXY_HOST}:${STATS_PORT}/stats" 2>/dev/null || echo '')"
if [ -z "$stats_output" ]; then
    fail "1-stats-reachable" "stats endpoint not reachable at http://${TELEPROXY_HOST}:${STATS_PORT}/stats"
    printf '\nFATAL: stats endpoint not available — aborting (is teleproxy running?)\n' >&2
    exit 1
else
    pass "1-stats-reachable"
fi

# Validate Prometheus format — must have at least one teleproxy_* or teleproto3_* metric
if printf '%s\n' "$stats_output" | grep -qE '^(teleproxy|teleproto3)_'; then
    pass "1-stats-prometheus-format"
else
    fail "1-stats-prometheus-format" "no teleproxy_* or teleproto3_* metrics in stats output"
fi

# ---------------------------------------------------------------------------
# AC#1 — WebSocket upgrade
# ---------------------------------------------------------------------------
printf '\n--- Test 2: WebSocket upgrade (AC#1) ---\n'
ws_key="$(head -c 16 /dev/urandom | od -An -tx1 | tr -d ' \n' | head -c 22)=="
ws_code="$(curl -sI \
    -H "Upgrade: websocket" \
    -H "Connection: Upgrade" \
    -H "Sec-WebSocket-Version: 13" \
    -H "Sec-WebSocket-Key: ${ws_key}" \
    --max-time 10 \
    "http://${TELEPROXY_HOST}:${TELEPROXY_PORT}${WS_PATH}" 2>/dev/null \
    | head -1 | grep -o '[0-9][0-9][0-9]' || echo '')"

if [ "$ws_code" = "101" ]; then
    pass "2-ws-upgrade-101"
else
    fail "2-ws-upgrade-101" "expected HTTP 101, got '${ws_code:-no response}'"
fi

# ---------------------------------------------------------------------------
# AC#2 — DC connectivity (optional, may be skipped in CI)
# ---------------------------------------------------------------------------
printf '\n--- Test 3: DC connectivity (AC#2) ---\n'
if [ "$SKIP_DC_CHECK" = "true" ]; then
    printf '  SKIP [3-dc-connectivity] (SKIP_DC_CHECK=true)\n'
else
    # Check stats for non-zero DC connections
    dc_conns="$(printf '%s\n' "$stats_output" \
        | awk '$1 ~ /^(teleproxy_direct_dc_connections_active|direct_dc_connections_active)$/ { print $2; found=1 } END { if (!found) print 0 }')"
    if [ "${dc_conns:-0}" -gt 0 ] 2>/dev/null; then
        pass "3-dc-connectivity (active_dc_conns=${dc_conns})"
    else
        # Fallback: check if any DC connections were ever created
        dc_created="$(printf '%s\n' "$stats_output" \
            | awk '$1 ~ /direct_dc_connections_created/ { print $2; found=1 } END { if (!found) print 0 }')"
        if [ "${dc_created:-0}" -gt 0 ] 2>/dev/null; then
            pass "3-dc-connectivity (dc_connections_created=${dc_created})"
        else
            fail "3-dc-connectivity" "no DC connections active or created (dc_active=${dc_conns:-0} dc_created=${dc_created:-0})"
        fi
    fi
fi

# ---------------------------------------------------------------------------
# AC#4 — Kill-switch: create disabled file → connections rejected; remove → accepted
# ---------------------------------------------------------------------------
printf '\n--- Test 4: Kill-switch (AC#4) ---\n'
if [ ! -d "$STATE_DIR" ]; then
    printf '  SKIP [4-kill-switch] (STATE_DIR=%s not found — not running as root or deployment missing)\n' "$STATE_DIR"
else
    # Enable kill-switch
    touch "${STATE_DIR}/disabled" 2>/dev/null || {
        printf '  SKIP [4-kill-switch] (cannot write to %s — not running as root)\n' "$STATE_DIR"
        SKIP_KILL_SWITCH=1
    }

    if [ -z "${SKIP_KILL_SWITCH:-}" ]; then
        sleep 2  # poll interval is 1s; wait 2s to be safe

        ks_code="$(curl -sI \
            -H "Upgrade: websocket" \
            -H "Connection: Upgrade" \
            -H "Sec-WebSocket-Version: 13" \
            -H "Sec-WebSocket-Key: ${ws_key}" \
            --max-time 5 \
            "http://${TELEPROXY_HOST}:${TELEPROXY_PORT}${WS_PATH}" 2>/dev/null \
            | head -1 | grep -o '[0-9][0-9][0-9]' || echo '')"

        if [ "$ks_code" != "101" ]; then
            pass "4-kill-switch-disabled (code=${ks_code:-closed})"
        else
            fail "4-kill-switch-disabled" "expected non-101 after kill-switch, got 101"
        fi

        # Remove kill-switch
        rm -f "${STATE_DIR}/disabled"
        sleep 2

        ks_restore_code="$(curl -sI \
            -H "Upgrade: websocket" \
            -H "Connection: Upgrade" \
            -H "Sec-WebSocket-Version: 13" \
            -H "Sec-WebSocket-Key: ${ws_key}" \
            --max-time 10 \
            "http://${TELEPROXY_HOST}:${TELEPROXY_PORT}${WS_PATH}" 2>/dev/null \
            | head -1 | grep -o '[0-9][0-9][0-9]' || echo '')"

        if [ "$ks_restore_code" = "101" ]; then
            pass "4-kill-switch-restored"
        else
            fail "4-kill-switch-restored" "expected 101 after kill-switch removed, got '${ks_restore_code}'"
        fi
    fi
fi

# ---------------------------------------------------------------------------
# AC#5 — PROXY protocol: real client IP appears in stats (not 127.0.0.1)
# ---------------------------------------------------------------------------
printf '\n--- Test 5: PROXY protocol (AC#5) ---\n'
if [ "$PROXY_PROTOCOL" != "true" ]; then
    printf '  SKIP [5-proxy-protocol] (PROXY_PROTOCOL != true)\n'
else
    pp_before="$(printf '%s\n' "$stats_output" \
        | awk '$1 == "proxy_protocol_connections" { print $2; found=1 } END { if (!found) print 0 }')"

    # Send valid PROXY v1 header with TEST-NET-1 client IP
    REAL_CLIENT_IP="192.0.2.100"
    (
        printf 'PROXY TCP4 %s 127.0.0.1 54321 %s\r\n' "$REAL_CLIENT_IP" "$TELEPROXY_PORT"
        printf '0000000000000000'
    ) | nc -N -w 2 "$TELEPROXY_HOST" "$TELEPROXY_PORT" >/dev/null 2>&1 || true

    sleep 0.5
    stats_after="$(curl -sf --max-time 10 "http://${TELEPROXY_HOST}:${STATS_PORT}/stats" 2>/dev/null || echo '')"
    pp_after="$(printf '%s\n' "$stats_after" \
        | awk '$1 == "proxy_protocol_connections" { print $2; found=1 } END { if (!found) print 0 }')"

    if [ "${pp_after:-0}" -gt "${pp_before:-0}" ] 2>/dev/null; then
        pass "5-proxy-protocol-connections-incremented"
    else
        fail "5-proxy-protocol-connections-incremented" \
            "counter did not increment (before=${pp_before} after=${pp_after})"
    fi
fi

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
END_TS="$(date +%s 2>/dev/null || echo 0)"
ELAPSED=$(( END_TS - START_TS )) 2>/dev/null || ELAPSED=0

printf '\n=== Results ===\n'
printf 'PASS: %d  FAIL: %d  TIME: %ds\n' "$PASS" "$FAIL" "$ELAPSED"

# AC#7: warn if > 60s
if [ "$ELAPSED" -gt 60 ] 2>/dev/null; then
    printf 'WARN: test took %ds (AC#7 target is <60s)\n' "$ELAPSED" >&2
fi

if [ "$FAIL" -gt 0 ]; then
    printf '\nSome tests FAILED.\n' >&2
    exit 1
fi

printf '\nAll tests passed.\n'
exit 0
