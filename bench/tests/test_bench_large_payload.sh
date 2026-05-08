#!/usr/bin/env bash
# test_bench_large_payload.sh — Story 1a-9 integration test
#
# RED phase reproducer for the ≥ 10 MiB SINK/ECHO threshold bug:
#   After a Type3 BENCH bench session is established, receiving ≥ ~10 MiB in
#   SINK or ECHO mode causes bench_session_lookup() to return NULL on a
#   subsequent parse_execute() call. The AR-S2 dispatch hook then falls
#   through to type3_dispatch_on_crypto_init(), which interprets the random
#   bench payload as an obfuscated-2 session header. If the first 4 bytes
#   happen to parse as a valid Type3 session header (T3_OK), the MTProto
#   proxy_pass path opens an outbound connection to a Telegram DC.
#   The client sees a TCP RST → connection_reset.
#
# Mechanism (confirmed via server log analysis):
#   "New outbound connection #N 94.156.131.252:XXXXX -> 94.156.131.252:443"
#   appears in the bench listener log mid-ECHO stream — the bench connection
#   (fd=7) was re-dispatched to the MTProto proxy_pass path.
#
# This test MUST fail on the unpatched parent commit and MUST pass after the
# 1a-9 fix (was_bench guard + bench_connection_mark / bench_connection_is_marked).
#
# Parent commit at time of RED-phase authoring: 599747b
#   ("Stories 1a-1 / 1a-2 / 1a-3: review-pass close-out 2026-05-07")
#
# Requires:
#   - teleproto3/server/objs/bin/teleproxy (or mtproto-proxy) built with
#       make TELEPROTO3_BENCH=1 TELEPROTO3_DISPATCH_HOOK=1
#     (Linux-only; SKIP is emitted on macOS / missing binary)
#   - Python ≥ 3.9 with 'cryptography' package installed
#
# Exit codes:
#   0  — PASS (all bench sessions completed, no connection_reset)
#   1  — FAIL (connection_reset or other error)
#   77 — SKIP (missing binary or deps)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BENCH_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
PROJECT_ROOT="$(cd "$BENCH_DIR/../.." && pwd)"
SERVER_DIR="$PROJECT_ROOT/teleproto3/server"

PASS=0; FAIL=0; SKIP=0

pass()  { PASS=$((PASS+1));  printf '  PASS [%s]\n'       "$1"; }
fail()  { FAIL=$((FAIL+1));  printf '  FAIL [%s]: %s\n'   "$1" "${2:-}" >&2; }
skip()  { SKIP=$((SKIP+1));  printf '  SKIP [%s]: %s\n'   "$1" "${2:-}"; }

printf '\n=== Story 1a-9: large-payload SINK/ECHO threshold integration test ===\n\n'

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
        "no binary found — build: cd teleproto3/server && make TELEPROTO3_BENCH=1 TELEPROTO3_DISPATCH_HOOK=1"
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
# 1. Server setup
# ---------------------------------------------------------------------------
printf '%s\n' '--- server setup ---'

PORT=$(python3 -c \
    "import socket; s=socket.socket(); s.bind(('',0)); p=s.getsockname()[1]; s.close(); print(p)")
SERVER_SECRET="deadbeef00000000deadbeef00000000"
TMPDIR_TEST="$(mktemp -d)"
SERVER_PID=""

cleanup() {
    if [ -n "$SERVER_PID" ]; then
        kill -TERM "$SERVER_PID" 2>/dev/null || true
        for _ in 1 2 3 4 5 6; do
            kill -0 "$SERVER_PID" 2>/dev/null || break
            sleep 0.5
        done
        kill -KILL "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    rm -rf "$TMPDIR_TEST"
}
trap cleanup EXIT

"$BENCH_BIN" \
    -H "$PORT" \
    -S "$SERVER_SECRET" \
    --direct \
    --enable-bench-handler \
    >"$TMPDIR_TEST/server.log" 2>&1 &
SERVER_PID=$!

PROBE_LOG="$TMPDIR_TEST/probe.log"
SERVER_READY=0
if python3 - >"$PROBE_LOG" 2>&1 <<PYEOF
import socket, time
for _ in range(50):
    try:
        s = socket.socket(); s.settimeout(0.2); s.connect(("127.0.0.1", $PORT)); s.close(); exit(0)
    except Exception: time.sleep(0.1)
exit(1)
PYEOF
then
    SERVER_READY=1
fi

if [ "$SERVER_READY" -eq 0 ]; then
    fail "server_start" \
        "server did not start; log: $(head -5 "$TMPDIR_TEST/server.log" 2>/dev/null)"
    printf '\n=== Results: %d passed, %d failed, %d skipped ===\n' "$PASS" "$FAIL" "$SKIP"
    exit 1
fi
pass "server_start"

# ---------------------------------------------------------------------------
# Helper: run_bench MODE SIZE_BYTES → prints error_class[:sha256_match]
# ---------------------------------------------------------------------------
run_bench_py() {
    local mode="$1" size="$2"
    python3 - <<PYEOF
import sys, asyncio, os, hashlib
sys.path.insert(0, "$PROJECT_ROOT")

from teleproto3.bench.type3_protocol import connect_type3, T3_CMD_BENCH
from teleproto3.bench.bench_client import (
    ConnAdapter, mode_sink, mode_echo,
    BENCH_SUB_MODE_SINK, BENCH_SUB_MODE_ECHO,
)

PORT = $PORT
SIZE = $size
MODE = "$mode"

async def run():
    try:
        conn, _session = await asyncio.wait_for(
            connect_type3(
                server="127.0.0.1", port=PORT, path="/ws",
                secret=bytes(16),
                command_type=T3_CMD_BENCH,
                tls=False,
            ),
            timeout=10.0,
        )
    except Exception as e:
        print(f"CONNECT_FAIL:{e}", flush=True)
        return

    payload = os.urandom(SIZE)
    adapter  = ConnAdapter(conn)

    try:
        if MODE == "sink":
            await conn.send(bytes([BENCH_SUB_MODE_SINK]))
            result = await asyncio.wait_for(
                mode_sink(writer=adapter, payload=payload, chunk_size=65536),
                timeout=120.0,
            )
            print(f"error_class:{result.get('error_class', 'ok')}", flush=True)

        elif MODE == "echo":
            await conn.send(bytes([BENCH_SUB_MODE_ECHO]))
            result = await asyncio.wait_for(
                mode_echo(reader=adapter, writer=adapter, payload=payload,
                          ack_mode="streaming", chunk_size=65536),
                timeout=120.0,
            )
            ec  = result.get("error_class", "ok")
            sha = result.get("sha256_match", "na")
            print(f"error_class:{ec} sha256_match:{sha}", flush=True)

    except (ConnectionResetError, EOFError, asyncio.IncompleteReadError) as e:
        print(f"error_class:connection_reset detail:{e!r}", flush=True)
    except asyncio.TimeoutError:
        print(f"error_class:timeout", flush=True)
    except Exception as e:
        print(f"error_class:other detail:{e!r}", flush=True)
    finally:
        try:
            await conn.close()
        except Exception:
            pass

asyncio.run(run())
PYEOF
}

check_result() {
    local label="$1" result="$2" need_sha="${3:-}"
    local ec
    ec="$(echo "$result" | grep -oP 'error_class:\K\S+' || echo 'unknown')"
    if [ "$ec" != "ok" ]; then
        if [ "$ec" = "connection_reset" ]; then
            fail "$label" "connection_reset — dispatch fall-through to MTProto (expected on unpatched commit)"
        else
            fail "$label" "unexpected error_class=$ec; result=$result"
        fi
        return
    fi
    if [ "$need_sha" = "1" ]; then
        local sha
        sha="$(echo "$result" | grep -oP 'sha256_match:\K\S+' || echo 'na')"
        if [ "$sha" != "true" ]; then
            fail "$label" "sha256_match=$sha (echo data corrupted)"
            return
        fi
    fi
    pass "$label"
}

# ---------------------------------------------------------------------------
# 2. Positive control — 1 MiB (should pass on all commits)
# ---------------------------------------------------------------------------
printf '%s\n' '--- positive controls: 1 MiB ---'

R="$(run_bench_py sink $((1*1024*1024)) 2>/dev/null)"
printf '  sink 1MiB  → %s\n' "$R"
check_result "sink_1mib_positive_control" "$R"

R="$(run_bench_py echo $((1*1024*1024)) 2>/dev/null)"
printf '  echo 1MiB  → %s\n' "$R"
check_result "echo_1mib_positive_control" "$R" "1"

# ---------------------------------------------------------------------------
# 3. Threshold tests — 10 MiB (fails on unpatched commit, passes post-fix)
# ---------------------------------------------------------------------------
printf '%s\n' '--- threshold tests: 10 MiB ---'

R="$(run_bench_py sink $((10*1024*1024)) 2>/dev/null)"
printf '  sink 10MiB → %s\n' "$R"
check_result "sink_10mib" "$R"

R="$(run_bench_py echo $((10*1024*1024)) 2>/dev/null)"
printf '  echo 10MiB → %s\n' "$R"
check_result "echo_10mib" "$R" "1"

# ---------------------------------------------------------------------------
# 4. High-water mark — 50 MiB (AC #1 requirement)
# ---------------------------------------------------------------------------
printf '%s\n' '--- high-water mark: 50 MiB ---'

R="$(run_bench_py sink $((50*1024*1024)) 2>/dev/null)"
printf '  sink 50MiB → %s\n' "$R"
check_result "sink_50mib" "$R"

R="$(run_bench_py echo $((50*1024*1024)) 2>/dev/null)"
printf '  echo 50MiB → %s\n' "$R"
check_result "echo_50mib" "$R" "1"

# ---------------------------------------------------------------------------
# 5. Server log snippet (aids root-cause audit when test fails)
# ---------------------------------------------------------------------------
if [ "$FAIL" -gt 0 ]; then
    printf '\n%s\n' '--- server log tail (for root-cause analysis) ---'
    tail -20 "$TMPDIR_TEST/server.log" 2>/dev/null || true
fi

printf '\n=== Results: %d passed, %d failed, %d skipped ===\n' "$PASS" "$FAIL" "$SKIP"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
