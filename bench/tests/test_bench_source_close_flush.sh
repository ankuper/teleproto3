#!/usr/bin/env bash
# test_bench_source_close_flush.sh — Story 1a-8 integration test
#
# RED phase reproducer for the SOURCE close-flush race:
#   bench_drain_connection returns -1 on BENCH_RC_SOURCE_DONE; the
#   dispatcher calls fail_connection(C,-1) which aborts the TCP socket
#   without flushing c->out; client receives RST instead of payload + WS
#   close 1000.
#
# Decision log (Task 1 spec):
#   Harness: subprocess-launched proxy + Python client.
#   Rationale: the bug is in the interaction between bench-session.c and
#   the dispatcher hook in net-tcp-rpc-ext-server.c. A pure C socket
#   harness would need to re-implement the WS framing + Type3 session
#   header, duplicating work already covered by type3_protocol.py.
#   subprocess launch lets us exercise the real binary on the real socket
#   path. The Python client (bench_client.py + type3_protocol.py) is the
#   same code the live bench uses, so a passing test here directly
#   corresponds to a green bench.sh --smoke.
#
# Parent commit at time of RED-phase authoring: 599747b
#   ("Stories 1a-1 / 1a-2 / 1a-3: review-pass close-out 2026-05-07")
#   This test MUST fail on 599747b and MUST pass after the 1a-8 fix.
#
# Requires:
#   - teleproty3/server/objs/bin/teleproxy built with
#       make TELEPROTO3_BENCH=1 TELEPROTO3_DISPATCH_HOOK=1
#     (Linux-only; SKIP is emitted on macOS / missing binary)
#   - Python ≥ 3.9 with 'cryptography' package installed
#
# Exit codes:
#   0  — PASS
#   1  — FAIL
#   77 — SKIP (missing binary or deps)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BENCH_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
PROJECT_ROOT="$(cd "$BENCH_DIR/../.." && pwd)"
SERVER_DIR="$PROJECT_ROOT/teleproto3/server"

PASS=0; FAIL=0; SKIP=0

pass() { PASS=$((PASS+1)); printf '  PASS [%s]\n' "$1"; }
fail() { FAIL=$((FAIL+1)); printf '  FAIL [%s]: %s\n' "$1" "${2:-}" >&2; }
skip() { SKIP=$((SKIP+1)); printf '  SKIP [%s]: %s\n' "$1" "${2:-}"; }

printf '\n=== Story 1a-8: SOURCE close-flush integration test ===\n\n'

# ---------------------------------------------------------------------------
# 0. Prerequisites
# ---------------------------------------------------------------------------
printf '%s\n' '--- prerequisites ---'

BENCH_BIN=""
for cand in \
    "$SERVER_DIR/objs/bin/teleproxy" \
    "$SERVER_DIR/objs/bin/mtproto-proxy"; do
    if [ -f "$cand" ]; then BENCH_BIN="$cand"; break; fi
done

if [ -z "$BENCH_BIN" ]; then
    skip "bench_binary" \
        "no binary found — build with: cd teleproto3/server && make TELEPROTO3_BENCH=1 TELEPROTO3_DISPATCH_HOOK=1"
    printf '\n=== Results: %d passed, %d failed, %d skipped ===\n' "$PASS" "$FAIL" "$SKIP"
    exit 77
fi

if ! nm "$BENCH_BIN" 2>/dev/null | grep "bench_drain_connection" >/dev/null 2>&1; then
    skip "bench_binary" "binary lacks bench symbols (build with TELEPROTO3_BENCH=1)"
    printf '\n=== Results: %d passed, %d failed, %d skipped ===\n' "$PASS" "$FAIL" "$SKIP"
    exit 77
fi

if ! python3 -c "import cryptography" 2>/dev/null; then
    skip "python_deps" "cryptography package not installed (pip install cryptography)"
    printf '\n=== Results: %d passed, %d failed, %d skipped ===\n' "$PASS" "$FAIL" "$SKIP"
    exit 77
fi

pass "prerequisites"

# ---------------------------------------------------------------------------
# 1. bench_source_close_flush — core test
#    Launch server, run SOURCE N=64, assert all bytes + no connection_reset.
# ---------------------------------------------------------------------------
printf '%s\n' '--- bench_source_close_flush ---'

# Pick a free port
PORT=$(python3 -c "import socket; s=socket.socket(); s.bind(('',0)); p=s.getsockname()[1]; s.close(); print(p)")

# Dummy 16-byte AES secret (server accepts it; bench path doesn't use it)
SERVER_SECRET="deadbeef00000000deadbeef00000000"

TMPDIR_TEST="$(mktemp -d)"
SERVER_PID=""

cleanup() {
    if [ -n "$SERVER_PID" ]; then
        # TERM first; if still alive after 2 s, KILL. Helper writev can
        # legitimately block for up to 30 s on a wedged peer (see helper's
        # FLUSH_BUDGET_NS), so escalation matters for clean test teardown.
        kill -TERM "$SERVER_PID" 2>/dev/null || true
        for _ in 1 2 3 4; do
            kill -0 "$SERVER_PID" 2>/dev/null || break
            sleep 0.5
        done
        kill -KILL "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    rm -rf "$TMPDIR_TEST"
}
trap cleanup EXIT

# Start server
"$BENCH_BIN" \
    -H "$PORT" \
    -S "$SERVER_SECRET" \
    --direct \
    --enable-bench-handler \
    >"$TMPDIR_TEST/server.log" 2>&1 &
SERVER_PID=$!

# Wait for server to start accepting connections (poll up to 5 s via Python).
# Capture stderr so a Python error doesn't masquerade as "server didn't start".
PROBE_LOG="$TMPDIR_TEST/probe.log"
SERVER_READY=0
if python3 - >"$PROBE_LOG" 2>&1 <<PYEOF
import socket, time
for _ in range(50):
    try:
        s = socket.socket()
        s.settimeout(0.2)
        s.connect(("127.0.0.1", $PORT))
        s.close()
        exit(0)
    except Exception:
        time.sleep(0.1)
exit(1)
PYEOF
then
    SERVER_READY=1
fi

if [ "$SERVER_READY" -eq 0 ]; then
    fail "bench_source_close_flush" \
        "server did not start in 5 s; server.log: $(head -5 "$TMPDIR_TEST/server.log" 2>/dev/null || echo '(empty)'); probe.log: $(head -5 "$PROBE_LOG" 2>/dev/null || echo '(empty)')"
    printf '\n=== Results: %d passed, %d failed, %d skipped ===\n' "$PASS" "$FAIL" "$SKIP"
    exit 1
fi

# Run SOURCE N=64 bench session via embedded Python
RESULT="$(python3 - <<PYEOF
import sys, asyncio

# Locate bench modules
sys.path.insert(0, "$PROJECT_ROOT")

from teleproto3.bench.type3_protocol import (
    connect_type3,
    T3_CMD_BENCH,
)
from teleproto3.bench.bench_client import (
    ConnAdapter,
    mode_source,
    BENCH_SUB_MODE_SOURCE,
)

PORT = $PORT
N = 64

async def run():
    try:
        conn, _session = await asyncio.wait_for(
            connect_type3(
                server="127.0.0.1",
                port=PORT,
                path="/ws",
                secret=bytes(16),   # unused on BENCH path
                command_type=T3_CMD_BENCH,
                tls=False,
            ),
            timeout=5.0,
        )
    except Exception as e:
        print(f"CONNECT_FAIL:{e}", flush=True)
        return

    try:
        # Send sub-mode byte (SOURCE = 0x03) as a WS frame
        await conn.send(bytes([BENCH_SUB_MODE_SOURCE]))

        adapter = ConnAdapter(conn)
        result = await asyncio.wait_for(
            mode_source(reader=adapter, writer=adapter, size=N),
            timeout=5.0,
        )
        print(f"bytes_received:{result['bytes_received']}", flush=True)
        print(f"error_class:{result.get('error_class', 'ok')}", flush=True)
    except (ConnectionResetError, EOFError, asyncio.IncompleteReadError) as e:
        # bench_client.py treats this same set as the close-flush RED signal.
        # On Linux the kernel may surface RST as ConnectionResetError; on
        # other paths a clean EOF after partial bytes shows as IncompleteRead.
        print(f"error_class:connection_reset", flush=True)
        print(f"detail:{e!r}", flush=True)
    except Exception as e:
        print(f"error_class:other", flush=True)
        print(f"detail:{e!r}", flush=True)
    finally:
        await conn.close()

asyncio.run(run())
PYEOF
)"

printf 'Result: %s\n' "$RESULT"

if echo "$RESULT" | grep -q "bytes_received:$((64))" && \
   echo "$RESULT" | grep -q "error_class:ok"; then
    pass "bench_source_close_flush"
else
    # Provide detailed failure message to aid root-cause identification
    if echo "$RESULT" | grep -q "connection_reset"; then
        fail "bench_source_close_flush" \
            "client got connection_reset — close-flush race present (expected on unpatched commit 599747b)"
    elif echo "$RESULT" | grep -q "CONNECT_FAIL"; then
        fail "bench_source_close_flush" "WS/Type3 handshake failed: $RESULT"
    else
        fail "bench_source_close_flush" "unexpected result: $RESULT"
    fi
fi

# ---------------------------------------------------------------------------
printf '\n=== Results: %d passed, %d failed, %d skipped ===\n' "$PASS" "$FAIL" "$SKIP"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
