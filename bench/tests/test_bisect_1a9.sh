#!/usr/bin/env bash
# test_bisect_1a9.sh — Story 1a-9 threshold bisection script
#
# AC #2: runs SINK and ECHO at 1, 2, 4, 6, 8, 10 MiB against a locally-launched
# bench-enabled proxy and records the exact size where connection_reset first
# appears. Output is committed alongside the integration test so the regression
# range is auditable.
#
# Run this on a Linux host where the bench binary has been built:
#   cd teleproto3/server && make TELEPROTO3_BENCH=1 TELEPROTO3_DISPATCH_HOOK=1
#
# Usage:
#   ./test_bisect_1a9.sh                      # run all sizes and report
#   BISECT_OUTPUT=/tmp/bisect.json ./test_bisect_1a9.sh   # write JSON report
#
# Exit codes:
#   0  — PASS (all sizes succeed = no threshold bug present after fix)
#   1  — FAIL (some sizes fail = threshold bug still present)
#   77 — SKIP (missing binary or deps)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BENCH_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
PROJECT_ROOT="$(cd "$BENCH_DIR/../.." && pwd)"
SERVER_DIR="$PROJECT_ROOT/teleproto3/server"

BISECT_OUTPUT="${BISECT_OUTPUT:-}"
SIZES_MIB=(1 2 4 6 8 10)
MODES=("sink" "echo")

PASS=0; FAIL=0; SKIP=0

pass()  { PASS=$((PASS+1));  printf '  PASS [%s]\n'       "$1"; }
fail()  { FAIL=$((FAIL+1));  printf '  FAIL [%s]: %s\n'   "$1" "${2:-}" >&2; }
skip()  { SKIP=$((SKIP+1));  printf '  SKIP [%s]: %s\n'   "$1" "${2:-}"; }

printf '\n=== Story 1a-9: threshold bisection (SINK+ECHO at 1/2/4/6/8/10 MiB) ===\n\n'

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
    skip "bench_binary" "no binary — build: cd teleproto3/server && make TELEPROTO3_BENCH=1 TELEPROTO3_DISPATCH_HOOK=1"
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
# 1. Server lifecycle helpers
# ---------------------------------------------------------------------------
SERVER_PID=""
TMPDIR_BISECT="$(mktemp -d)"
# Results stored as "mode mib error_class" lines in a temp file
RESULTS_FILE="$TMPDIR_BISECT/results.txt"
touch "$RESULTS_FILE"

cleanup() {
    if [ -n "$SERVER_PID" ]; then
        kill -TERM "$SERVER_PID" 2>/dev/null || true
        for _ in 1 2 3 4; do
            kill -0 "$SERVER_PID" 2>/dev/null || break
            sleep 0.5
        done
        kill -KILL "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
        SERVER_PID=""
    fi
}

trap 'cleanup; rm -rf "$TMPDIR_BISECT"' EXIT

start_server() {
    local port="$1" secret="$2" logfile="$3"
    "$BENCH_BIN" \
        -H "$port" \
        -S "$secret" \
        --direct \
        --enable-bench-handler \
        >"$logfile" 2>&1 &
    SERVER_PID=$!
    python3 - >/dev/null 2>&1 <<PYEOF
import socket, time
for _ in range(50):
    try:
        s = socket.socket(); s.settimeout(0.2); s.connect(("127.0.0.1", $port)); s.close(); exit(0)
    except Exception: time.sleep(0.1)
exit(1)
PYEOF
}

# ---------------------------------------------------------------------------
# 2. Single-run helper
# ---------------------------------------------------------------------------
run_one() {
    local mode="$1" size_bytes="$2" port="$3"
    python3 - <<PYEOF
import sys, asyncio, os
sys.path.insert(0, "$PROJECT_ROOT")

from teleproto3.bench.type3_protocol import connect_type3, T3_CMD_BENCH
from teleproto3.bench.bench_client import (
    ConnAdapter, mode_sink, mode_echo,
    BENCH_SUB_MODE_SINK, BENCH_SUB_MODE_ECHO,
)

PORT = $port
SIZE = $size_bytes
MODE = "$mode"

async def run():
    try:
        conn, _session = await asyncio.wait_for(
            connect_type3(
                server="127.0.0.1", port=PORT, path="/ws",
                secret=bytes(16), command_type=T3_CMD_BENCH, tls=False,
            ),
            timeout=10.0,
        )
    except Exception as e:
        print(f"error_class:handshake_fail", flush=True)
        return

    payload = os.urandom(SIZE)
    adapter = ConnAdapter(conn)

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
            print(f"error_class:{result.get('error_class', 'ok')}", flush=True)
    except (ConnectionResetError, EOFError, asyncio.IncompleteReadError):
        print("error_class:connection_reset", flush=True)
    except asyncio.TimeoutError:
        print("error_class:timeout", flush=True)
    except Exception as e:
        print(f"error_class:other", flush=True)
    finally:
        try: await conn.close()
        except Exception: pass

asyncio.run(run())
PYEOF
}

# ---------------------------------------------------------------------------
# 3. Bisection loop
# ---------------------------------------------------------------------------
PORT=$(python3 -c "import socket; s=socket.socket(); s.bind(('',0)); p=s.getsockname()[1]; s.close(); print(p)")
SECRET="deadbeef00000000deadbeef00000000"
SERVER_LOG="$TMPDIR_BISECT/server.log"

printf '%s\n' '--- starting server ---'
if ! start_server "$PORT" "$SECRET" "$SERVER_LOG"; then
    fail "server_start" "server did not start; log: $(head -3 "$SERVER_LOG" 2>/dev/null)"
    exit 1
fi
pass "server_start"

printf '\n%s\n' '--- bisection runs ---'
printf '%-8s  %-8s  %s\n' "MODE" "MiB" "RESULT"
printf '%-8s  %-8s  %s\n' "--------" "--------" "------"

FIRST_FAIL_SINK=""
FIRST_FAIL_ECHO=""

for mib in "${SIZES_MIB[@]}"; do
    size=$(( mib * 1024 * 1024 ))
    for mode in "${MODES[@]}"; do
        raw_result="$(run_one "$mode" "$size" "$PORT" 2>/dev/null || echo "error_class:run_error")"
        ec="$(printf '%s' "$raw_result" | grep -oE 'error_class:[^ ]+' | cut -d: -f2 || echo 'unknown')"
        printf '%-8s  %-8s  %s\n' "$mode" "${mib} MiB" "$ec"
        printf '%s %s %s\n' "$mode" "$mib" "$ec" >> "$RESULTS_FILE"

        if [ "$ec" = "ok" ]; then
            pass "${mode}_${mib}MiB"
        else
            fail "${mode}_${mib}MiB" "$ec"
            if [ "$mode" = "sink" ] && [ -z "$FIRST_FAIL_SINK" ]; then
                FIRST_FAIL_SINK="$mib"
            fi
            if [ "$mode" = "echo" ] && [ -z "$FIRST_FAIL_ECHO" ]; then
                FIRST_FAIL_ECHO="$mib"
            fi
        fi
    done
done

cleanup

# ---------------------------------------------------------------------------
# 4. Summary
# ---------------------------------------------------------------------------
printf '\n%s\n' '--- bisection summary ---'
if [ -n "$FIRST_FAIL_SINK" ]; then
    printf 'SINK first failure: %d MiB\n' "$FIRST_FAIL_SINK"
else
    printf 'SINK: all sizes OK\n'
fi
if [ -n "$FIRST_FAIL_ECHO" ]; then
    printf 'ECHO first failure: %d MiB\n' "$FIRST_FAIL_ECHO"
else
    printf 'ECHO: all sizes OK\n'
fi

# ---------------------------------------------------------------------------
# 5. Optional JSON output (AC #2: committed alongside test)
# ---------------------------------------------------------------------------
if [ -n "$BISECT_OUTPUT" ]; then
    RESULTS_DATA="$(cat "$RESULTS_FILE")"
    SINK_FF="${FIRST_FAIL_SINK:-null}"
    ECHO_FF="${FIRST_FAIL_ECHO:-null}"
    python3 - <<PYEOF
import json
sizes = [int(x) for x in "${SIZES_MIB[*]}".split()]
results_raw = """$RESULTS_DATA"""
rows = {}
for line in results_raw.strip().splitlines():
    parts = line.split()
    if len(parts) == 3:
        mode, mib, ec = parts
        rows.setdefault(mode, {})[mib] = ec

data = {
    "story": "1a-9",
    "binary": "$BENCH_BIN",
    "bisect_mib": sizes,
    "results": {
        mode: {str(s): rows.get(mode, {}).get(str(s), "missing") for s in sizes}
        for mode in ["sink", "echo"]
    },
    "first_fail": {
        "sink": None if "$SINK_FF" == "null" else int("$SINK_FF"),
        "echo": None if "$ECHO_FF" == "null" else int("$ECHO_FF"),
    },
}
with open("$BISECT_OUTPUT", "w") as f:
    json.dump(data, f, indent=2)
print(f"Bisection report written to: $BISECT_OUTPUT")
PYEOF
fi

printf '\n=== Results: %d passed, %d failed, %d skipped ===\n' "$PASS" "$FAIL" "$SKIP"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
