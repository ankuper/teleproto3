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
# 1. bench_symbols_absent_release (1a-2 AC #4)
# Story 1a-2 builds at server/objs/bin/teleproxy; legacy upstream paths
# (objs/bin/mtproto-proxy) are also accepted to ease local re-runs.
# ---------------------------------------------------------------------------
echo '--- bench_symbols_absent_release ---'
RELEASE_BIN=""
for cand in \
    "$PROJECT_ROOT/teleproto3/server/objs/bin/teleproxy" \
    "$PROJECT_ROOT/teleproto3/server/objs/bin/mtproto-proxy" \
    "$PROJECT_ROOT/objs/bin/teleproxy" \
    "$PROJECT_ROOT/objs/bin/mtproto-proxy"; do
    if [ -f "$cand" ]; then RELEASE_BIN="$cand"; break; fi
done
if [ -z "$RELEASE_BIN" ]; then
    skip "bench_symbols_absent_release" "no server binary found (built objs/bin/teleproxy?)"
else
    if ! command -v nm >/dev/null 2>&1; then
        skip "bench_symbols_absent_release" "nm not found in PATH"
    else
        # C11: guard against stripped binaries reporting 0 symbols as false-pass
        total_syms="$(nm "$RELEASE_BIN" 2>/dev/null | wc -l | tr -d ' ')"
        dyn_syms="$(nm --dynamic "$RELEASE_BIN" 2>/dev/null | wc -l | tr -d ' ')"
        # P9 (R2): use OR — if EITHER table is empty the binary is partially
        # stripped and the audit becomes unreliable for that table. Strict
        # release builds keep both populated.
        if [ "$total_syms" -eq 0 ] || [ "$dyn_syms" -eq 0 ]; then
            fail "bench_symbols_absent_release" \
                "nm returned 0 symbols on at least one table (total=$total_syms dyn=$dyn_syms) — binary may be partially stripped; audit unreliable"
        else
            # C11: extended pattern covers all bench TU entry points
            # P8 (R2): grep -c returns 0 (matches), 1 (no matches), >=2 (real error).
            # The previous `|| true` collapsed all three into "clean pass". Instead
            # we capture the exit code explicitly and only swallow rc == 1.
            bench_pattern='bench_handler|bench_session|bench_drain|bench_csprng|g_bench_'
            count_pattern() {
                local table_dump=$1 label=$2 out gc
                set +e
                out=$(printf '%s' "$table_dump" | grep -cE "$bench_pattern")
                gc=$?
                set -e
                if [ "$gc" -gt 1 ]; then
                    fail "bench_symbols_absent_release" \
                        "grep -c failed on $label table (exit=$gc) — pattern malformed or stream error"
                    printf '0'
                    return 1
                fi
                printf '%s' "$out"
            }
            static_dump="$(nm "$RELEASE_BIN" 2>/dev/null || true)"
            dyn_dump="$(nm --dynamic "$RELEASE_BIN" 2>/dev/null || true)"
            bench_count="$(count_pattern "$static_dump" 'static')"
            dyn_bench_count="$(count_pattern "$dyn_dump" 'dynamic')"
            total_bench=$((bench_count + dyn_bench_count))
            if [ "$total_bench" -eq 0 ]; then
                pass "bench_symbols_absent_release ($RELEASE_BIN)"
            else
                fail "bench_symbols_absent_release" \
                    "expected 0 bench symbols in $RELEASE_BIN, found $total_bench (static=$bench_count dynamic=$dyn_bench_count)"
            fi
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
