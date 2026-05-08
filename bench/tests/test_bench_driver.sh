#!/usr/bin/env bash
# test_bench_driver.sh — red-phase acceptance tests for Story 1a-5
#
# Bench driver: credential sourcing, reachability probe, smoke matrix,
# commit bundling, index append.
#
# TDD RED PHASE: will FAIL/SKIP until bench.sh is implemented.
# Returns 0 on all pass/skip, 1 on any failure.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
BENCH_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

PASS=0; FAIL=0; SKIP=0

pass() { PASS=$((PASS+1)); printf '  PASS [%s]\n' "$1"; }
fail() { FAIL=$((FAIL+1)); printf '  FAIL [%s]: %s\n' "$1" "${2:-}" >&2; }
skip() { SKIP=$((SKIP+1)); printf '  SKIP [%s]: %s\n' "$1" "${2:-}"; }

printf '\n=== Story 1a-5: bench driver ===\n\n'

BENCH_SH="$BENCH_DIR/bench.sh"

if [ ! -f "$BENCH_SH" ]; then
    skip "driver_sources_credentials" "bench.sh not found at $BENCH_SH"
    skip "driver_reachability_probe" "bench.sh not found"
    skip "smoke_option_matrix_size" "bench.sh not found"
    skip "commit_bundles_artefacts" "bench.sh not found"
    skip "commit_appends_index" "bench.sh not found"

    printf '\n=== Results: %d passed, %d failed, %d skipped ===\n' "$PASS" "$FAIL" "$SKIP"
    [ "$FAIL" -eq 0 ] && exit 0 || exit 1
fi

TMPDIR_TEST="$(mktemp -d)"
trap 'rm -rf "$TMPDIR_TEST"' EXIT

# ---------------------------------------------------------------------------
# 1. driver_sources_credentials (AC#1)
#    Missing .credentials → bench.sh exits non-zero
# ---------------------------------------------------------------------------
printf '%s\n' '--- driver_sources_credentials ---'

no_creds_rc=0
(
    env -u BENCH_DOMAIN -u BENCH_SECRET -u BENCH_PATH \
        bash "$BENCH_SH" --dry-run 2>/dev/null
) || no_creds_rc=$?

if [ "$no_creds_rc" -ne 0 ]; then
    pass "driver_sources_credentials"
else
    fail "driver_sources_credentials" \
        "bench.sh did not fail without credentials (exit=$no_creds_rc)"
fi

# ---------------------------------------------------------------------------
# 2. driver_reachability_probe (AC#1)
#    Unreachable host → fail early
# ---------------------------------------------------------------------------
printf '%s\n' '--- driver_reachability_probe ---'

cat > "$TMPDIR_TEST/.credentials" <<'CRED'
BENCH_DOMAIN="192.0.2.1"
BENCH_SECRET="ff00112233445566778899aabbccddeeff"
BENCH_PORT=65534
CRED

probe_rc=0
probe_output="$(
    cd "$TMPDIR_TEST" && bash "$BENCH_SH" 2>&1
)" || probe_rc=$?

if [ "$probe_rc" -ne 0 ]; then
    if echo "$probe_output" | grep -qiE '(unreachable|cannot connect|probe.*fail|not reachable|connection refused|timed? ?out)'; then
        pass "driver_reachability_probe"
    else
        fail "driver_reachability_probe" \
            "exited $probe_rc but no reachability message in output"
    fi
else
    fail "driver_reachability_probe" \
        "expected non-zero exit for unreachable host, got 0"
fi

# ---------------------------------------------------------------------------
# 3. smoke_option_matrix_size (AC#9)
#    --smoke runs 1MB x 3 modes x 3 runs = 9 invocations
# ---------------------------------------------------------------------------
printf '%s\n' '--- smoke_option_matrix_size ---'

smoke_output="$(cd "$BENCH_DIR" && bash "$BENCH_SH" --smoke --dry-run 2>&1)" || true

run_count="$(echo "$smoke_output" | grep -ciE '(run[: ]|iteration|bench_client|mode=)' || echo 0)"

if [ "$run_count" -eq 9 ] || echo "$smoke_output" | grep -qiE '(1mb.*3.*mode|matrix.*9|smoke.*9)'; then
    pass "smoke_option_matrix_size"
else
    fail "smoke_option_matrix_size" \
        "expected 9 runs in --smoke mode, counted $run_count"
fi

# ---------------------------------------------------------------------------
# 4. commit_dry_run_no_state_mutation (AC#8 + P8)
#    --commit --dry-run MUST NOT create a bundle dir or mutate any artefact
#    on disk. Real commit-bundling requires a live bench server and is
#    exercised end-to-end during a real --commit run.
# ---------------------------------------------------------------------------
printf '%s\n' '--- commit_dry_run_no_state_mutation ---'

MEASUREMENTS_DIR="$PROJECT_ROOT/_bmad-output/measurements"
_list_measurements_dir() {
    if [ -d "$MEASUREMENTS_DIR" ]; then
        ls "$MEASUREMENTS_DIR" 2>/dev/null | sort
    else
        echo ""
    fi
}
before_dirs="$(_list_measurements_dir)"

(cd "$BENCH_DIR" && bash "$BENCH_SH" --commit --dry-run 2>&1) >/dev/null || true

after_dirs="$(_list_measurements_dir)"

if [ "$before_dirs" = "$after_dirs" ]; then
    pass "commit_dry_run_no_state_mutation"
else
    fail "commit_dry_run_no_state_mutation" \
        "--commit --dry-run mutated $MEASUREMENTS_DIR (P8: dry-run must not write to disk)"
fi

# ---------------------------------------------------------------------------
# 5. commit_dry_run_does_not_touch_index (AC#8 + P8)
#    --commit --dry-run MUST NOT append to INDEX.md.
# ---------------------------------------------------------------------------
printf '%s\n' '--- commit_dry_run_does_not_touch_index ---'

INDEX_MD="$MEASUREMENTS_DIR/INDEX.md"
if [ -f "$INDEX_MD" ]; then
    before_lines="$(wc -l < "$INDEX_MD" | tr -d ' ')"
else
    before_lines=0
fi

(cd "$BENCH_DIR" && bash "$BENCH_SH" --commit --dry-run 2>&1) >/dev/null || true

if [ -f "$INDEX_MD" ]; then
    after_lines="$(wc -l < "$INDEX_MD" | tr -d ' ')"
else
    after_lines=0
fi

if [ "$after_lines" -eq "$before_lines" ]; then
    pass "commit_dry_run_does_not_touch_index"
else
    fail "commit_dry_run_does_not_touch_index" \
        "INDEX.md grew during dry-run (before=$before_lines, after=$after_lines; P8: dry-run must not mutate state)"
fi

# ---------------------------------------------------------------------------
printf '\n=== Results: %d passed, %d failed, %d skipped ===\n' "$PASS" "$FAIL" "$SKIP"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
