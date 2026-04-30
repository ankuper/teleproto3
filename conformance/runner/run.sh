#!/usr/bin/env bash
# run.sh — POSIX runner for the Type3 conformance harness.
# Drives an IUT (subprocess or TCP endpoint) through the compliance-level
# scenario set and pipes per-scenario NDJSON to verify.py.
#
# Usage:
#   ./run.sh --impl <path-to-iut> [--level core|full|extended]
#   ./run.sh --endpoint host:port  [--level core|full|extended]
#
#   --impl and --endpoint are mutually exclusive.
#   Supplying neither or both exits 2 (harness setup error).
#
# Compliance levels and directory sets:
#   core     → conformance/scenarios/mandatory/
#   full     → conformance/scenarios/mandatory/ + full/
#   extended → conformance/scenarios/mandatory/ + full/ + extended/
#
# Exit codes:
#   0  all scenarios in the selected level passed
#   1  one or more scenarios failed
#   2  harness setup error (IUT not runnable, missing vectors, bad args)
#
# This is a reference implementation of the Type3 protocol conformance
# runner. spec/ wins over this file. File a bug or errata at the
# teleproto3 issue tracker.

set -euo pipefail

LEVEL="core"
IMPL=""
ENDPOINT=""

while [ $# -gt 0 ]; do
    case "$1" in
        --impl)     IMPL="$2";     shift 2 ;;
        --endpoint) ENDPOINT="$2"; shift 2 ;;
        --level)    LEVEL="$2";    shift 2 ;;
        -h|--help)
            sed -n '2,21p' "$0"
            exit 0
            ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done

# Mutual-exclusion: neither or both → exit 2
if [ -z "$IMPL" ] && [ -z "$ENDPOINT" ]; then
    echo "error: one of --impl or --endpoint is required" >&2
    exit 2
fi
if [ -n "$IMPL" ] && [ -n "$ENDPOINT" ]; then
    echo "error: --endpoint and --impl are mutually exclusive" >&2
    exit 2
fi

# Validate endpoint format
if [ -n "$ENDPOINT" ]; then
    case "$ENDPOINT" in
        *:*) ;;
        *) echo "error: --endpoint must be host:port" >&2; exit 2 ;;
    esac
fi

# Validate subprocess IUT
if [ -n "$IMPL" ]; then
    [ -x "$IMPL" ] || { echo "IUT not executable: $IMPL" >&2; exit 2; }
fi

# Validate level
case "$LEVEL" in
    core|full|extended) ;;
    *) echo "invalid --level: $LEVEL" >&2; exit 2 ;;
esac

HERE="$(cd "$(dirname "$0")" && pwd)"
SCENARIOS_ROOT="$HERE/../scenarios"
VECTORS="$HERE/../vectors/unit.json"

[ -f "$VECTORS" ] || { echo "missing vectors file: $VECTORS" >&2; exit 2; }

# LEVEL→DIRS mapping (style-guide §4)
case "$LEVEL" in
    core)     DIRS="mandatory" ;;
    full)     DIRS="mandatory full" ;;
    extended) DIRS="mandatory full extended" ;;
esac

VERIFY_PY="$HERE/verify.py"
[ -f "$VERIFY_PY" ] || { echo "missing verify.py: $VERIFY_PY" >&2; exit 2; }

# Ignore SIGPIPE to avoid silent termination if verify.py crashes
trap '' PIPE

# Temporary FIFO for NDJSON piping (secure directory to avoid TOCTOU)
TMPDIR="$(mktemp -d -t run_sh_ndjson.XXXXXX)"
NDJSON_PIPE="$TMPDIR/pipe"
mkfifo "$NDJSON_PIPE"
trap 'rm -rf "$TMPDIR"' EXIT

# Run verify.py consuming the FIFO in the background
python3 "$VERIFY_PY" < "$NDJSON_PIPE" &
VERIFY_PID=$!

# Open FIFO for writing (keep writer end open until we are done)
# Use <> to avoid blocking if verify.py fails to start
exec 3<>"$NDJSON_PIPE"

HARNESS_ERROR=0
FAIL_COUNT=0

# Dispatch a single scenario manifest file to the IUT
dispatch_scenario() {
    local manifest="$1"
    local scenario_id
    scenario_id="$(basename "$manifest" .yaml)"
    scenario_id="$(basename "$scenario_id" .json)"

    # Determine section from parent directory name
    local parent_dir
    parent_dir="$(basename "$(dirname "$manifest")")"
    local section="unknown"
    case "$parent_dir" in
        mandatory|full|extended) section="$parent_dir" ;;
    esac

    local observed_hex=""
    local result="FAIL"
    local detail=""

    if [ -n "$IMPL" ]; then
        # Subprocess mode: pipe scenario id to IUT via stdin, read hex response
        if observed_hex="$(printf '%s\n' "$scenario_id" | "$IMPL" 2>/dev/null)"; then
            observed_hex="$(echo "$observed_hex" | tr -d '\n"\\')"
            result="PASS"
        else
            result="FAIL"
            detail="IUT exited non-zero"
        fi
    else
        # Endpoint mode: open TCP socket, send scenario_id line, read response
        local host port
        port="${ENDPOINT##*:}"
        host="${ENDPOINT%:*}"
        host="${host#[}"
        host="${host%]}"
        if observed_hex="$(printf '%s\n' "$scenario_id" | nc -w5 "$host" "$port" 2>/dev/null)"; then
            observed_hex="$(echo "$observed_hex" | tr -d '\n"\\')"
            result="PASS"
        else
            result="FAIL"
            detail="endpoint unreachable or timed out"
        fi
    fi

    # Emit one NDJSON line to verify.py
    if [ "$result" = "FAIL" ] && [ -n "$detail" ]; then
        printf '{"scenario":"%s","section":"%s","result":"FAIL","observed":"","detail":"%s"}\n' \
            "$scenario_id" "$section" "$detail" >&3
        FAIL_COUNT=$((FAIL_COUNT + 1))
    else
        printf '{"scenario":"%s","section":"%s","result":"%s","observed":"%s"}\n' \
            "$scenario_id" "$section" "$result" "$observed_hex" >&3
        if [ "$result" = "FAIL" ]; then
            FAIL_COUNT=$((FAIL_COUNT + 1))
        fi
    fi
}

# Walk selected directories
for dir in $DIRS; do
    dir_path="$SCENARIOS_ROOT/$dir"
    if [ ! -d "$dir_path" ]; then
        echo "warning: scenario directory not found: $dir_path" >&2
        continue
    fi
    for manifest in "$dir_path"/*.yaml "$dir_path"/*.json; do
        [ -f "$manifest" ] || continue
        dispatch_scenario "$manifest"
    done
done

# Close the writer end of the FIFO so verify.py sees EOF
exec 3>&-

# Wait for verify.py to finish
set +e
wait "$VERIFY_PID"
VERIFY_EXIT=$?
set -e

if [ "$VERIFY_EXIT" -eq 2 ]; then
    HARNESS_ERROR=1
elif [ "$VERIFY_EXIT" -ne 0 ]; then
    FAIL_COUNT=$((FAIL_COUNT + 1))
fi

if [ "$HARNESS_ERROR" -ne 0 ]; then
    exit 2
fi
if [ "$FAIL_COUNT" -gt 0 ]; then
    exit 1
fi
exit 0
