#!/usr/bin/env bash
# dispatch_integration_test.sh — integration tests for the Type3 dispatcher.
#
# Source: story 4-5 Task 5.1, AC #1, #2, #3.
# Tests: valid Session Header → accepted; invalid → silent close within 200ms.
#
# Requires: teleproxy running in Docker (docker-compose.type3-dispatch-test.yml)
# or a running instance reachable at TELEPROXY_WS_HOST:TELEPROXY_WS_PORT.
#
# Returns 0 on pass / 1 on fail.

set -euo pipefail

TELEPROXY_WS_HOST="${TELEPROXY_WS_HOST:-127.0.0.1}"
TELEPROXY_WS_PORT="${TELEPROXY_WS_PORT:-3130}"
STATS_PORT="${STATS_PORT:-8889}"
SILENT_CLOSE_MAX_MS="${SILENT_CLOSE_MAX_MS:-200}"
FAIL=0

pass() { printf "PASS [%s]\n" "$1"; }
fail() { printf "FAIL [%s]: %s\n" "$1" "${2:-}"; FAIL=1; }
skip() { printf "SKIP [%s]: %s\n" "$1" "${2:-}"; }

echo "=== dispatch_integration_test.sh: Type3 dispatcher integration ==="
echo "  Target: ws://${TELEPROXY_WS_HOST}:${TELEPROXY_WS_PORT}"

# ------------------------------------------------------------------ #
# Helpers                                                              #
# ------------------------------------------------------------------ #

# Perform a WS upgrade and send binary payload, measure time to FIN.
# Args: payload_hex (8 hex chars = 4 bytes), expect_accept (0|1)
# Returns time-to-close in milliseconds on stdout.
ws_probe() {
    local payload_hex="$1"
    local expect_accept="${2:-0}"

    # Use Python3 for WS + timing (widely available in CI images).
    python3 - <<PYEOF
import socket, hashlib, base64, os, time, struct, sys

host = "${TELEPROXY_WS_HOST}"
port = ${TELEPROXY_WS_PORT}
payload = bytes.fromhex("${payload_hex}")

sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sock.settimeout(2.0)
try:
    sock.connect((host, port))
except Exception as e:
    print(f"CONN_FAIL: {e}", file=sys.stderr)
    sys.exit(2)

# WS upgrade handshake
ws_key = base64.b64encode(os.urandom(16)).decode()
upgrade = (
    f"GET / HTTP/1.1\r\n"
    f"Host: {host}\r\n"
    f"Upgrade: websocket\r\n"
    f"Connection: Upgrade\r\n"
    f"Sec-WebSocket-Key: {ws_key}\r\n"
    f"Sec-WebSocket-Version: 13\r\n"
    f"\r\n"
).encode()
sock.sendall(upgrade)

# Read 101 Switching Protocols
resp = b""
while b"\r\n\r\n" not in resp:
    chunk = sock.recv(4096)
    if not chunk:
        print("UPGRADE_FAIL: server closed without 101", file=sys.stderr)
        sys.exit(2)
    resp += chunk
if b"101" not in resp:
    print(f"UPGRADE_FAIL: {resp[:80]}", file=sys.stderr)
    sys.exit(2)

# Send Session Header as WS Binary frame (no masking for server→client direction;
# but WS client MUST mask: use a zero mask key for simplicity in tests).
mask = bytes(4)   # zero mask = plaintext payload passes through
frame = bytes([0x82]) + bytes([0x84]) + mask  # FIN+binary, masked, len=4
frame += bytes(b ^ m for b, m in zip(payload, mask * 4))
t0 = time.monotonic()
sock.sendall(frame)

# Wait for server to close (FIN/RST) or for a response.
sock.settimeout(0.5)
try:
    data = sock.recv(4096)
    if data:
        # Server sent data (session accepted or unexpected response)
        dt_ms = int((time.monotonic() - t0) * 1000)
        print(f"DATA:{dt_ms}")
    else:
        # Clean FIN
        dt_ms = int((time.monotonic() - t0) * 1000)
        print(f"CLOSE:{dt_ms}")
except socket.timeout:
    # No close within 500ms — server kept connection open (accept case)
    dt_ms = int((time.monotonic() - t0) * 1000)
    print(f"OPEN:{dt_ms}")
finally:
    sock.close()
PYEOF
}

# Check if python3 available
if ! command -v python3 >/dev/null 2>&1; then
    skip "4-5-INT-001" "python3 not available — skipping WS integration tests"
    echo "=== RESULT: SKIP ==="
    exit 0
fi

# Check if teleproxy is reachable
if ! python3 -c "import socket; s=socket.socket(); s.settimeout(1); s.connect(('${TELEPROXY_WS_HOST}', ${TELEPROXY_WS_PORT})); s.close()" 2>/dev/null; then
    skip "4-5-INT-001" "teleproxy not reachable at ${TELEPROXY_WS_HOST}:${TELEPROXY_WS_PORT} — skipping"
    echo "=== RESULT: SKIP (server not running) ==="
    exit 0
fi

# ------------------------------------------------------------------ #
# 4-5-INT-001: Valid Session Header → session accepted (no close)     #
# AC #1, #3                                                            #
# ------------------------------------------------------------------ #
# Valid header: cmd=0x01 (Type3), ver=0x01, flags=0x0000 (LE)
VALID_HEADER="01010000"
result=$(ws_probe "$VALID_HEADER" 1 2>/dev/null || echo "PROBE_ERR")
case "$result" in
    OPEN:*|DATA:*)
        pass "4-5-INT-001: valid Session Header → session accepted (connection kept open)"
        ;;
    CLOSE:*)
        fail "4-5-INT-001: valid Session Header → unexpected close"
        ;;
    *)
        fail "4-5-INT-001: probe error: $result"
        ;;
esac

# ------------------------------------------------------------------ #
# 4-5-INT-002: Invalid Session Header → silent close within 200ms    #
# AC #2: no WS close frame, FIN within 50–200ms                       #
# ------------------------------------------------------------------ #
# Invalid header: sentinel byte 0xFF → MALFORMED
INVALID_HEADER="ffffffff"
result=$(ws_probe "$INVALID_HEADER" 0 2>/dev/null || echo "PROBE_ERR")
case "$result" in
    CLOSE:*)
        dt_ms="${result#CLOSE:}"
        if [ "$dt_ms" -le "$SILENT_CLOSE_MAX_MS" ]; then
            pass "4-5-INT-002: invalid Session Header → silent close in ${dt_ms}ms (≤${SILENT_CLOSE_MAX_MS}ms)"
        else
            fail "4-5-INT-002: silent close took ${dt_ms}ms, expected ≤${SILENT_CLOSE_MAX_MS}ms"
        fi
        ;;
    OPEN:*)
        fail "4-5-INT-002: invalid Session Header → connection NOT closed (kept open)"
        ;;
    DATA:*)
        # Any data response is acceptable if it closes afterward — but WS close frame is banned (AC#2)
        fail "4-5-INT-002: server sent data response to invalid header (WS close frame?)"
        ;;
    *)
        fail "4-5-INT-002: probe error: $result"
        ;;
esac

# ------------------------------------------------------------------ #
# 4-5-INT-003: Invalid header version → silent close within 200ms    #
# AC #2: T3_ERR_UNSUPPORTED_VERSION path                              #
# ------------------------------------------------------------------ #
# Bad version: cmd=0x01, version=0x00 (sentinel), flags=0x0000
BAD_VERSION_HEADER="01000000"
result=$(ws_probe "$BAD_VERSION_HEADER" 0 2>/dev/null || echo "PROBE_ERR")
case "$result" in
    CLOSE:*)
        dt_ms="${result#CLOSE:}"
        if [ "$dt_ms" -le "$SILENT_CLOSE_MAX_MS" ]; then
            pass "4-5-INT-003: bad version → silent close in ${dt_ms}ms"
        else
            fail "4-5-INT-003: bad version close took ${dt_ms}ms, expected ≤${SILENT_CLOSE_MAX_MS}ms"
        fi
        ;;
    *)
        fail "4-5-INT-003: expected CLOSE for bad version, got: $result"
        ;;
esac

# ------------------------------------------------------------------ #
# Summary                                                              #
# ------------------------------------------------------------------ #
echo ""
if [ "$FAIL" -eq 0 ]; then
    echo "=== RESULT: PASS ==="
else
    echo "=== RESULT: FAIL ==="
    exit 1
fi
