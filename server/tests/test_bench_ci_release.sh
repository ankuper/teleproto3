#!/usr/bin/env bash
# test_bench_ci_release.sh — red-phase acceptance tests for Stories 1a-2/1a-3/1a-5
#
# CI/lint assertions: bench symbols absent in release, python compile/format,
# shellcheck on bench scripts.
#
# TDD RED PHASE: will FAIL/SKIP until bench tooling is implemented.
# Returns 0 on all pass/skip, 1 on any failure.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

PASS=0; FAIL=0; SKIP=0

pass() { PASS=$((PASS+1)); printf '  PASS [%s]\n' "$1"; }
fail() { FAIL=$((FAIL+1)); printf '  FAIL [%s]: %s\n' "$1" "${2:-}" >&2; }
skip() { SKIP=$((SKIP+1)); printf '  SKIP [%s]: %s\n' "$1" "${2:-}"; }

printf '\n=== Story 1a-2/1a-3/1a-5: CI/lint assertions ===\n\n'

# ---------------------------------------------------------------------------
# 1. bench_symbols_absent_release (1a-2 AC#4)
# ---------------------------------------------------------------------------
echo '--- bench_symbols_absent_release ---'
RELEASE_BIN="$PROJECT_ROOT/objs/bin/mtproto-proxy"
if [ ! -f "$RELEASE_BIN" ]; then
    skip "bench_symbols_absent_release" "binary $RELEASE_BIN not found"
else
    if ! command -v nm >/dev/null 2>&1; then
        skip "bench_symbols_absent_release" "nm not found in PATH"
    else
        bench_count="$(nm "$RELEASE_BIN" 2>/dev/null | grep -c bench_handler || true)"
        if [ "$bench_count" -eq 0 ]; then
            pass "bench_symbols_absent_release"
        else
            fail "bench_symbols_absent_release" \
                "expected 0 bench_handler symbols in release binary, found $bench_count"
        fi
    fi
fi

# ---------------------------------------------------------------------------
# 2. python_compile_check (1a-3 AC#9)
# ---------------------------------------------------------------------------
echo '--- python_compile_check ---'
BENCH_CLIENT="$PROJECT_ROOT/teleproto3/bench/bench_client.py"
if [ ! -f "$BENCH_CLIENT" ]; then
    skip "python_compile_check" "$BENCH_CLIENT not found"
else
    if ! command -v python3 >/dev/null 2>&1; then
        skip "python_compile_check" "python3 not found in PATH"
    else
        if python3 -m py_compile "$BENCH_CLIENT" 2>/dev/null; then
            pass "python_compile_check"
        else
            fail "python_compile_check" "py_compile failed on bench_client.py"
        fi
    fi
fi

# ---------------------------------------------------------------------------
# 3. python_black_check (1a-3 AC#9)
# ---------------------------------------------------------------------------
echo '--- python_black_check ---'
REPORT_PY="$PROJECT_ROOT/teleproto3/bench/report.py"
if [ ! -f "$BENCH_CLIENT" ] || [ ! -f "$REPORT_PY" ]; then
    skip "python_black_check" "bench_client.py and/or report.py not found"
else
    if ! command -v black >/dev/null 2>&1; then
        skip "python_black_check" "black not found in PATH"
    else
        if black --check "$BENCH_CLIENT" "$REPORT_PY" >/dev/null 2>&1; then
            pass "python_black_check"
        else
            fail "python_black_check" "black --check failed on bench Python files"
        fi
    fi
fi

# ---------------------------------------------------------------------------
# 4. shellcheck_bench (1a-5 AC#10)
# ---------------------------------------------------------------------------
echo '--- shellcheck_bench ---'
BENCH_SH="$PROJECT_ROOT/teleproto3/bench/bench.sh"
if [ ! -f "$BENCH_SH" ]; then
    skip "shellcheck_bench" "$BENCH_SH not found"
else
    if ! command -v shellcheck >/dev/null 2>&1; then
        skip "shellcheck_bench" "shellcheck not found in PATH"
    else
        if shellcheck "$BENCH_SH" 2>/dev/null; then
            pass "shellcheck_bench"
        else
            fail "shellcheck_bench" "shellcheck found issues in bench.sh"
        fi
    fi
fi

# ---------------------------------------------------------------------------
# 5. shellcheck_smoke (1a-5 AC#10)
# ---------------------------------------------------------------------------
echo '--- shellcheck_smoke ---'
SMOKE_SH="$PROJECT_ROOT/teleproto3/bench/smoke.sh"
if [ ! -f "$SMOKE_SH" ]; then
    skip "shellcheck_smoke" "$SMOKE_SH not found"
else
    if ! command -v shellcheck >/dev/null 2>&1; then
        skip "shellcheck_smoke" "shellcheck not found in PATH"
    else
        if shellcheck "$SMOKE_SH" 2>/dev/null; then
            pass "shellcheck_smoke"
        else
            fail "shellcheck_smoke" "shellcheck found issues in smoke.sh"
        fi
    fi
fi

# ---------------------------------------------------------------------------
printf '\n=== Results: %d passed, %d failed, %d skipped ===\n' "$PASS" "$FAIL" "$SKIP"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
