#!/usr/bin/env bash
# test_smoke_harness.sh — red-phase acceptance tests for Story 1a-6
#
# Smoke harness: exit codes, output isolation, CSV structure, correctness,
# idempotency.
#
# TDD RED PHASE: will FAIL/SKIP until smoke.sh is implemented.
# Returns 0 on all pass/skip, 1 on any failure.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
BENCH_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

PASS=0; FAIL=0; SKIP=0

pass() { PASS=$((PASS+1)); printf '  PASS [%s]\n' "$1"; }
fail() { FAIL=$((FAIL+1)); printf '  FAIL [%s]: %s\n' "$1" "${2:-}" >&2; }
skip() { SKIP=$((SKIP+1)); printf '  SKIP [%s]: %s\n' "$1" "${2:-}"; }

printf '\n=== Story 1a-6: smoke harness ===\n\n'

SMOKE_SH="$BENCH_DIR/smoke.sh"

if [ ! -f "$SMOKE_SH" ]; then
    skip "exit_code_2_missing_credentials" "smoke.sh not found at $SMOKE_SH"
    skip "exit_code_2_missing_python" "smoke.sh not found"
    skip "output_isolation" "smoke.sh not found"
    skip "csv_row_count" "smoke.sh not found"
    skip "sha256_match_echo" "smoke.sh not found"
    skip "all_error_class_ok" "smoke.sh not found"
    skip "idempotent" "smoke.sh not found"

    printf '\n=== Results: %d passed, %d failed, %d skipped ===\n' "$PASS" "$FAIL" "$SKIP"
    [ "$FAIL" -eq 0 ] && exit 0 || exit 1
fi

# ---------------------------------------------------------------------------
# 1. exit_code_2_missing_credentials (AC#2, AC#5)
# ---------------------------------------------------------------------------
printf '%s\n' '--- exit_code_2_missing_credentials ---'

cred_rc=0
cred_output="$(
    env -u BENCH_SECRET -u BENCH_DOMAIN -u BENCH_PORT \
        bash "$SMOKE_SH" 2>&1
)" || cred_rc=$?

if [ "$cred_rc" -eq 2 ]; then
    if echo "$cred_output" | grep -qiE '(credential|secret|BENCH_SECRET|missing|required)'; then
        pass "exit_code_2_missing_credentials"
    else
        fail "exit_code_2_missing_credentials" \
            "exit code 2 but no actionable message about credentials"
    fi
else
    fail "exit_code_2_missing_credentials" \
        "expected exit code 2, got $cred_rc"
fi

# ---------------------------------------------------------------------------
# 2. exit_code_2_missing_python (AC#2, AC#5)
# ---------------------------------------------------------------------------
printf '%s\n' '--- exit_code_2_missing_python ---'

if env PATH="/usr/bin:/bin" command -v python3 >/dev/null 2>&1; then
    skip "exit_code_2_missing_python" \
        "python3 is in /usr/bin or /bin — cannot test missing-python scenario"
else
    py_rc=0
    env PATH="/usr/bin:/bin" \
        BENCH_SECRET="ff00112233445566778899aabbccddeeff" \
        BENCH_DOMAIN="test.example.com" \
        bash "$SMOKE_SH" 2>/dev/null || py_rc=$?
    if [ "$py_rc" -eq 2 ]; then
        pass "exit_code_2_missing_python"
    else
        fail "exit_code_2_missing_python" \
            "expected exit code 2 with no python3, got $py_rc"
    fi
fi

# ---------------------------------------------------------------------------
# Helper: check if bench server is reachable for live tests
# ---------------------------------------------------------------------------
SERVER_REACHABLE=false
if [ -f "$BENCH_DIR/.credentials" ]; then
    # shellcheck disable=SC1091
    . "$BENCH_DIR/.credentials" 2>/dev/null || true
fi
BENCH_DOMAIN="${BENCH_DOMAIN:-}"
BENCH_PORT="${BENCH_PORT:-443}"

if [ -n "$BENCH_DOMAIN" ]; then
    if bash -c "echo >/dev/tcp/$BENCH_DOMAIN/$BENCH_PORT" 2>/dev/null; then
        SERVER_REACHABLE=true
    fi
fi

# ---------------------------------------------------------------------------
# 3. output_isolation (AC#4)
#    Writes to smoke-results/, NOT to runs.csv in bench dir
# ---------------------------------------------------------------------------
printf '%s\n' '--- output_isolation ---'

if ! $SERVER_REACHABLE; then
    skip "output_isolation" "server not reachable at ${BENCH_DOMAIN:-<unset>}:$BENCH_PORT"
else
    rm -rf "$BENCH_DIR/smoke-results" 2>/dev/null || true
    bench_csv_before=""
    [ -f "$BENCH_DIR/runs.csv" ] && bench_csv_before="$(cat "$BENCH_DIR/runs.csv")"

    (cd "$BENCH_DIR" && bash "$SMOKE_SH" 2>&1) >/dev/null || true

    if [ -d "$BENCH_DIR/smoke-results" ]; then
        bench_csv_after=""
        [ -f "$BENCH_DIR/runs.csv" ] && bench_csv_after="$(cat "$BENCH_DIR/runs.csv")"
        if [ "$bench_csv_before" = "$bench_csv_after" ]; then
            pass "output_isolation"
        else
            fail "output_isolation" "runs.csv in bench dir was modified"
        fi
    else
        fail "output_isolation" "smoke-results/ directory not created"
    fi
fi

# ---------------------------------------------------------------------------
# 4. csv_row_count (AC#6)
#    smoke-results/runs.csv has exactly 6 data rows (3 modes x 2 runs)
# ---------------------------------------------------------------------------
printf '%s\n' '--- csv_row_count ---'

if ! $SERVER_REACHABLE; then
    skip "csv_row_count" "server not reachable"
else
    SMOKE_CSV="$BENCH_DIR/smoke-results/runs.csv"
    if [ ! -f "$SMOKE_CSV" ]; then
        (cd "$BENCH_DIR" && bash "$SMOKE_SH" 2>&1) >/dev/null || true
    fi
    if [ ! -f "$SMOKE_CSV" ]; then
        fail "csv_row_count" "smoke-results/runs.csv not found after smoke run"
    else
        data_rows="$(tail -n +2 "$SMOKE_CSV" | grep -cv '^\s*$' || echo 0)"
        if [ "$data_rows" -eq 6 ]; then
            pass "csv_row_count"
        else
            fail "csv_row_count" "expected 6 data rows, got $data_rows"
        fi
    fi
fi

# ---------------------------------------------------------------------------
# 5. sha256_match_echo (AC#6)
# ---------------------------------------------------------------------------
printf '%s\n' '--- sha256_match_echo ---'

if ! $SERVER_REACHABLE; then
    skip "sha256_match_echo" "server not reachable"
else
    SMOKE_CSV="$BENCH_DIR/smoke-results/runs.csv"
    if [ ! -f "$SMOKE_CSV" ]; then
        fail "sha256_match_echo" "smoke-results/runs.csv not found"
    else
        echo_match="$(grep -i 'echo' "$SMOKE_CSV" | grep -c 'true' || echo 0)"
        if [ "$echo_match" -ge 1 ]; then
            pass "sha256_match_echo"
        else
            fail "sha256_match_echo" \
                "no echo rows with sha256_match=true (found $echo_match)"
        fi
    fi
fi

# ---------------------------------------------------------------------------
# 6. all_error_class_ok (AC#6)
# ---------------------------------------------------------------------------
printf '%s\n' '--- all_error_class_ok ---'

if ! $SERVER_REACHABLE; then
    skip "all_error_class_ok" "server not reachable"
else
    SMOKE_CSV="$BENCH_DIR/smoke-results/runs.csv"
    if [ ! -f "$SMOKE_CSV" ]; then
        fail "all_error_class_ok" "smoke-results/runs.csv not found"
    else
        data_rows="$(tail -n +2 "$SMOKE_CSV" | grep -cv '^\s*$' || echo 0)"
        ok_rows="$(tail -n +2 "$SMOKE_CSV" | grep -c ',ok$' || echo 0)"
        if [ "$data_rows" -gt 0 ] && [ "$ok_rows" -eq "$data_rows" ]; then
            pass "all_error_class_ok"
        else
            fail "all_error_class_ok" \
                "not all rows have error_class=ok ($ok_rows/$data_rows)"
        fi
    fi
fi

# ---------------------------------------------------------------------------
# 7. idempotent (AC#7)
#    Run smoke.sh twice -> both exit 0
# ---------------------------------------------------------------------------
printf '%s\n' '--- idempotent ---'

if ! $SERVER_REACHABLE; then
    skip "idempotent" "server not reachable"
else
    rc1=0; rc2=0
    (cd "$BENCH_DIR" && bash "$SMOKE_SH" 2>&1) >/dev/null || rc1=$?
    (cd "$BENCH_DIR" && bash "$SMOKE_SH" 2>&1) >/dev/null || rc2=$?

    if [ "$rc1" -eq 0 ] && [ "$rc2" -eq 0 ]; then
        pass "idempotent"
    else
        fail "idempotent" "expected both runs exit 0, got rc1=$rc1 rc2=$rc2"
    fi
fi

# ---------------------------------------------------------------------------
printf '\n=== Results: %d passed, %d failed, %d skipped ===\n' "$PASS" "$FAIL" "$SKIP"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
