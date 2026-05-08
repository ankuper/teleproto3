#!/usr/bin/env bash
# bench.sh — Type3 throughput bench driver for Epic 1a (Story 1a-5)
#
# Usage:
#   ./bench.sh                       # full 3×3×11 matrix + iperf3 baseline
#   ./bench.sh --smoke               # 1MB × 3 modes × 3 runs (9 total, INDETERMINATE gate)
#   ./bench.sh --commit              # run + bundle artefacts to _bmad-output/measurements/
#   ./bench.sh --dry-run             # print planned runs without executing
#   ./bench.sh --smoke --commit      # combinable
#   ./bench.sh --commit --dry-run    # prints planned actions without mutating state
#
# Credentials (sourced from .credentials in CWD or bench dir; bench-only, mandatory):
#   BENCH_DOMAIN    — bench VPS hostname (NOT prod proxy; safety guard)
#   BENCH_PATH      — WebSocket path (default /ws)
#   BENCH_SECRET    — Type3 secret hex (bench instance only)
#   BENCH_PORT      — server port (default 443)
#   IPERF3_PORT     — iperf3 listen port on bench VPS (default 5201)
#
# iperf3 server deployment: see docs/bench-sandbox-runbook.md
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BENCH_PY="$SCRIPT_DIR/bench_client.py"
REPORT_PY="$SCRIPT_DIR/report.py"
RESULTS_DIR="$SCRIPT_DIR/results"

# GNU coreutils `timeout` is required for per-run watchdogs. macOS ships
# without it; `gtimeout` (brew install coreutils) is the standard alias.
if command -v timeout >/dev/null 2>&1; then
    TIMEOUT_BIN="timeout"
elif command -v gtimeout >/dev/null 2>&1; then
    TIMEOUT_BIN="gtimeout"
else
    echo "error: 'timeout' (or 'gtimeout') not found — install GNU coreutils (brew install coreutils)" >&2
    exit 1
fi

# --- Flag parsing ---
DRY_RUN=false
COMMIT=false
SMOKE=false

for arg in "$@"; do
    case "$arg" in
        --dry-run) DRY_RUN=true ;;
        --commit)  COMMIT=true ;;
        --smoke)   SMOKE=true ;;
        *)
            echo "error: unknown flag: $arg" >&2
            echo "usage: bench.sh [--smoke] [--commit] [--dry-run]" >&2
            exit 1
            ;;
    esac
done

# PID-suffixed session id avoids collisions when two --commit runs land in same second
SESSION_TS="$(date +%Y-%m-%d-%H%M%S)-$$"
BUNDLE_DIR="$PROJECT_ROOT/_bmad-output/measurements/teleproxy-bench-${SESSION_TS}"
INDEX_MD="$PROJECT_ROOT/_bmad-output/measurements/INDEX.md"

# --- Matrix parameters ---
SIZES_FULL=(1048576 10485760 52428800)
MODES=(sink echo source)

# Returns fixture path for a given size (bash 3.2-compatible, no declare -A)
_fixture_for_size() {
    case "$1" in
        1048576)  echo "$SCRIPT_DIR/fixtures/fixture-1mb.bin" ;;
        10485760) echo "$SCRIPT_DIR/fixtures/fixture-10mb.bin" ;;
        52428800) echo "$SCRIPT_DIR/fixtures/fixture-50mb.bin" ;;
        *) echo "" ;;
    esac
}

if $SMOKE; then
    SIZES=(1048576)
    N_RUNS=3   # 1 warmup + 2 valid; gate emits INDETERMINATE per AC#9
else
    SIZES=("${SIZES_FULL[@]}")
    N_RUNS=11  # 1 warmup + 10 valid
fi

TOTAL_CELLS=$(( ${#SIZES[@]} * ${#MODES[@]} ))

# --- Dry-run: print planned matrix BEFORE credential check ---
if $DRY_RUN; then
    TOTAL_RUNS=0
    for size in "${SIZES[@]}"; do
        for mode in "${MODES[@]}"; do
            for ((run_index=0; run_index<N_RUNS; run_index++)); do
                echo "[dry-run] run ${run_index}: mode=${mode} size=${size} bench_client.py"
                TOTAL_RUNS=$((TOTAL_RUNS+1))
            done
        done
    done
    RUN_LABEL="$( $SMOKE && echo 'smoke' || echo 'full' )"
    echo "[dry-run] ${RUN_LABEL}: ${TOTAL_RUNS} total runs planned (${TOTAL_CELLS} cells × ${N_RUNS} runs each)"
    echo "[dry-run] matrix: ${#SIZES[@]} sizes × ${#MODES[@]} modes × ${N_RUNS} runs = ${TOTAL_RUNS}"
fi

# --- Credential sourcing (bench-only, mandatory; NO prod-creds fallback by design) ---
# Look in CWD first, then bench dir (NOT project root to avoid test interference)
_cred_loaded=false
for _cred_path in ".credentials" "$SCRIPT_DIR/.credentials"; do
    if [ -f "$_cred_path" ]; then
        # shellcheck disable=SC1090
        . "$_cred_path"
        _cred_loaded=true
        break
    fi
done

BENCH_DOMAIN="${BENCH_DOMAIN:-}"
BENCH_PATH="${BENCH_PATH:-/ws}"
BENCH_SECRET="${BENCH_SECRET:-}"
BENCH_PORT="${BENCH_PORT:-443}"
IPERF3_PORT="${IPERF3_PORT:-5201}"

if [ -z "$BENCH_DOMAIN" ] || [ -z "$BENCH_SECRET" ]; then
    echo "error: missing credentials: set BENCH_DOMAIN + BENCH_SECRET in .credentials or env" >&2
    echo "  Create teleproto3/bench/.credentials with:" >&2
    echo "    BENCH_DOMAIN=\"your-bench-vps.example.com\"" >&2
    echo "    BENCH_SECRET=\"ff<32-hex-chars>\"" >&2
    echo "  NOTE: bench-specific credentials are MANDATORY by design (safety guard against" >&2
    echo "        accidentally pumping bench traffic through the prod proxy)." >&2
    exit 1
fi

# --- Exit early for dry-run (no state mutation in dry-run mode) ---
if $DRY_RUN; then
    echo "[dry-run] credentials: BENCH_DOMAIN=${BENCH_DOMAIN} BENCH_PORT=${BENCH_PORT}"
    echo "[dry-run] handshake-probe: python3 ${BENCH_PY} --mode sink --fixture <1KB> --runs 1 --timeout 5"
    echo "[dry-run] iperf3 -c ${BENCH_DOMAIN} -p ${IPERF3_PORT} -t 30 -J > iperf3_baseline.json"
    if $COMMIT; then
        echo "[dry-run] --commit: bundle dir would be created at $BUNDLE_DIR"
        echo "[dry-run] --commit: INDEX.md would receive a single row at $INDEX_MD"
    fi
    exit 0
fi

# --- Reachability probe (TCP) ---
echo "Probing bench server ${BENCH_DOMAIN}:${BENCH_PORT}..."
if ! bash -c "echo >/dev/tcp/${BENCH_DOMAIN}/${BENCH_PORT}" 2>/dev/null; then
    echo "error: cannot connect to ${BENCH_DOMAIN}:${BENCH_PORT} — bench server not reachable" >&2
    exit 1
fi
echo "Bench server reachable at ${BENCH_DOMAIN}:${BENCH_PORT}"

# --- Handshake-probe (Type3 round-trip via bench_client) ---
# Catches nginx-up-but-handler-down, secret mismatch, server crash after bind, etc.
# Uses SINK mode: client sends bytes, server consumes silently. SINK is the
# only sub-mode that does not exercise the server's early-close path
# (SOURCE auto-closes after N bytes; ECHO is bandwidth-bound).
echo "Handshake-probe (sink 1KB)..."
HANDSHAKE_TMP="$(mktemp)"
HANDSHAKE_FIXTURE="$(mktemp)"
trap 'rm -f "$HANDSHAKE_TMP" "$HANDSHAKE_FIXTURE"' EXIT
dd if=/dev/zero of="$HANDSHAKE_FIXTURE" bs=1024 count=1 status=none
handshake_args=(
    --mode sink --fixture "$HANDSHAKE_FIXTURE" --runs 1 --timeout 5
    --port "$BENCH_PORT" --output "$HANDSHAKE_TMP"
)
[ "${BENCH_NO_TLS:-0}" = "1" ] && handshake_args+=(--no-tls)
if ! BENCH_DOMAIN="$BENCH_DOMAIN" BENCH_PATH="$BENCH_PATH" BENCH_SECRET="$BENCH_SECRET" \
        "$TIMEOUT_BIN" 10 python3 "$BENCH_PY" "${handshake_args[@]}" >/dev/null 2>&1; then
    echo "error: handshake failed — bench server reachable on TCP but Type3 round-trip broken" >&2
    echo "       check: TELEPROTO3_BENCH=1, --enable-bench-handler, secret mismatch, server crash" >&2
    exit 1
fi
# Verify the run produced an 'ok' row (not connection_reset / timeout etc.).
# CSV writer emits \r\n line endings, so match ,ok before optional CR + EOL.
if ! grep -qE $',ok\r?$' "$HANDSHAKE_TMP" 2>/dev/null; then
    echo "error: handshake produced no 'ok' row in $HANDSHAKE_TMP:" >&2
    cat "$HANDSHAKE_TMP" >&2
    exit 1
fi
echo "Handshake OK"

# --- iperf3 baseline (informational; ratio is NOT in gate per AC#7) ---
mkdir -p "$RESULTS_DIR"
IPERF3_JSON="$RESULTS_DIR/iperf3_baseline.json"

echo "Running iperf3 baseline against ${BENCH_DOMAIN}:${IPERF3_PORT} (30s)..."
if command -v iperf3 >/dev/null 2>&1; then
    if ! iperf3 -c "${BENCH_DOMAIN}" -p "${IPERF3_PORT}" -t 30 -J > "$IPERF3_JSON"; then
        echo "WARNING: iperf3 unavailable; ratio column will read N/A. Bench gate is unaffected (validity-based per AC#6/AC#7)." >&2
        echo '{}' > "$IPERF3_JSON"
    fi
else
    echo "WARNING: iperf3 not found — install via 'brew install iperf3' or 'apt install iperf3'" >&2
    echo "WARNING: iperf3 unavailable; ratio column will read N/A. Bench gate is unaffected (validity-based per AC#6/AC#7)." >&2
    echo '{}' > "$IPERF3_JSON"
fi

# --- Matrix runs ---
RUNS_CSV="$RESULTS_DIR/runs.csv"

# Truncate runs.csv at session start so historical data does not pollute aggregation
: > "$RUNS_CSV"

echo ""
echo "Starting bench matrix: ${#SIZES[@]} sizes × ${#MODES[@]} modes × ${N_RUNS} runs = $(( ${#SIZES[@]} * ${#MODES[@]} * N_RUNS )) total"
echo ""

FIRST_CELL=true
for size in "${SIZES[@]}"; do
    for mode in "${MODES[@]}"; do
        if ! $FIRST_CELL; then
            echo "Sleeping 30s (inter-cell pacing for TCP/CC state decay)..."
            sleep 30
        fi
        FIRST_CELL=false

        size_mb=$(( size / 1048576 ))
        echo "=== Cell: ${size_mb}MB / ${mode} / ${N_RUNS} runs ==="

        for ((run_index=0; run_index<N_RUNS; run_index++)); do
            if [ "$run_index" -gt 0 ]; then
                sleep 5
            fi

            args=(
                --mode "$mode"
                --output "$RUNS_CSV"
                --port "$BENCH_PORT"
                --runs 1
                --run-index "$run_index"
                --timeout 120
            )

            if [ "$mode" = "source" ]; then
                args+=(--size "$size")
            else
                fixture="$(_fixture_for_size "$size")"
                args+=(--fixture "$fixture")
            fi

            # TLS is controlled by the server; pass --no-tls only when on non-TLS port
            # Default: TLS on (port 443); override with BENCH_NO_TLS=1 env var
            if [ "${BENCH_NO_TLS:-0}" = "1" ]; then
                args+=(--no-tls)
            fi

            warmup_flag=""
            [ "$run_index" -eq 0 ] && warmup_flag=" [warmup]"

            echo "  run ${run_index}${warmup_flag}: mode=${mode} size=${size}B"

            BENCH_DOMAIN="$BENCH_DOMAIN" \
            BENCH_PATH="$BENCH_PATH" \
            BENCH_SECRET="$BENCH_SECRET" \
                "$TIMEOUT_BIN" 180 python3 "$BENCH_PY" "${args[@]}"
        done

        echo ""
    done
done

# --- Generate report ---
SUMMARY_MD="$RESULTS_DIR/summary.md"
REPORT_ARGS=("$RUNS_CSV" "$IPERF3_JSON" --output "$SUMMARY_MD")
$SMOKE && REPORT_ARGS+=(--smoke)

if [ -f "$RUNS_CSV" ] && [ -f "$REPORT_PY" ]; then
    echo "Generating report..."
    if GATE="$(python3 "$REPORT_PY" "${REPORT_ARGS[@]}")"; then
        echo ""
        cat "$SUMMARY_MD"
        echo ""
        echo "Acceptance gate: $GATE"
    else
        GATE="ERROR"
        echo "Acceptance gate: ERROR (report.py exited non-zero; see stderr above)" >&2
    fi
else
    GATE="ERROR"
    echo "WARNING: report.py or runs.csv not found — skipping report generation" >&2
fi

# --- Commit bundle (single INDEX.md row, written at end with the real gate) ---
if $COMMIT; then
    mkdir -p "$BUNDLE_DIR"
    MEASUREMENTS_DIR="$(dirname "$BUNDLE_DIR")"
    mkdir -p "$MEASUREMENTS_DIR"

    if [ ! -f "$INDEX_MD" ]; then
        {
            echo "# Bench Measurement Index"
            echo ""
            echo "| Timestamp | Mode | Cells | Gate |"
            echo "|-----------|------|-------|------|"
        } > "$INDEX_MD"
    fi

    cp "$RUNS_CSV" "$BUNDLE_DIR/runs.csv"
    cp "$IPERF3_JSON" "$BUNDLE_DIR/iperf3_baseline.json"
    [ -f "$SUMMARY_MD" ] && cp "$SUMMARY_MD" "$BUNDLE_DIR/summary.md"

    RUN_MODE="$( $SMOKE && echo 'smoke' || echo 'full' )"
    echo "| ${SESSION_TS} | ${RUN_MODE} | ${TOTAL_CELLS} | ${GATE} |" >> "$INDEX_MD"

    echo "Bundle saved to: $BUNDLE_DIR"
    echo "Index updated: $INDEX_MD"
fi

echo ""
echo "Done. Gate result: $GATE"
