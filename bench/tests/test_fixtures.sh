#!/usr/bin/env bash
# test_fixtures.sh — red-phase acceptance tests for Story 1a-4
#
# Fixture generation: file creation, size accuracy, manifest integrity,
# incompressibility, gitignore.
#
# TDD RED PHASE: will FAIL/SKIP until fixture generator is implemented.
# Returns 0 on all pass/skip, 1 on any failure.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
BENCH_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
FIXTURES_DIR="$BENCH_DIR/fixtures"

PASS=0; FAIL=0; SKIP=0

pass() { PASS=$((PASS+1)); printf '  PASS [%s]\n' "$1"; }
fail() { FAIL=$((FAIL+1)); printf '  FAIL [%s]: %s\n' "$1" "${2:-}" >&2; }
skip() { SKIP=$((SKIP+1)); printf '  SKIP [%s]: %s\n' "$1" "${2:-}"; }

printf '\n=== Story 1a-4: fixture generation ===\n\n'

GENERATE_SH="$FIXTURES_DIR/generate.sh"

# ---------------------------------------------------------------------------
# 1. generator_produces_files (AC#1)
# ---------------------------------------------------------------------------
printf '%s\n' '--- generator_produces_files ---'
if [ ! -f "$GENERATE_SH" ]; then
    skip "generator_produces_files" "generate.sh not found at $GENERATE_SH"
    skip "size_accuracy_1mb" "generate.sh not found"
    skip "size_accuracy_10mb" "generate.sh not found"
    skip "size_accuracy_50mb" "generate.sh not found"
    skip "manifest_valid_json" "generate.sh not found"
    skip "manifest_three_entries" "generate.sh not found"
    skip "manifest_sha256_match" "generate.sh not found"
    skip "incompressible" "generate.sh not found"
    skip "gitignore_exists" "generate.sh not found"

    printf '\n=== Results: %d passed, %d failed, %d skipped ===\n' "$PASS" "$FAIL" "$SKIP"
    [ "$FAIL" -eq 0 ] && exit 0 || exit 1
fi

GEN_EXIT=0
(cd "$FIXTURES_DIR" && bash generate.sh 2>&1) || GEN_EXIT=$?
if [ "$GEN_EXIT" -ne 0 ]; then
    printf 'WARN: generate.sh exited %d (downstream test failures may have masked root cause)\n' "$GEN_EXIT" >&2
fi

all_exist=true
for f in fixture-1mb.bin fixture-10mb.bin fixture-50mb.bin; do
    if [ ! -f "$FIXTURES_DIR/$f" ]; then
        all_exist=false
        break
    fi
done

if $all_exist; then
    pass "generator_produces_files"
else
    fail "generator_produces_files" "one or more fixture-*.bin files missing after generate.sh"
fi

# ---------------------------------------------------------------------------
# 2-4. size_accuracy (AC#3)
# ---------------------------------------------------------------------------
check_size() {
    local name="$1" file="$2" expected="$3"
    printf '%s\n' "--- ${name} ---"
    if [ ! -f "$file" ]; then
        fail "$name" "$file not found"
        return
    fi
    actual="$(wc -c < "$file" | tr -d ' ')"
    if [ "$actual" -eq "$expected" ]; then
        pass "$name"
    else
        fail "$name" "expected $expected bytes, got $actual"
    fi
}

check_size "size_accuracy_1mb"  "$FIXTURES_DIR/fixture-1mb.bin"  1048576
check_size "size_accuracy_10mb" "$FIXTURES_DIR/fixture-10mb.bin" 10485760
check_size "size_accuracy_50mb" "$FIXTURES_DIR/fixture-50mb.bin" 52428800

# ---------------------------------------------------------------------------
# 5. manifest_valid_json (AC#2)
# ---------------------------------------------------------------------------
printf '%s\n' '--- manifest_valid_json ---'
MANIFEST="$FIXTURES_DIR/manifest.json"
if [ ! -f "$MANIFEST" ]; then
    fail "manifest_valid_json" "manifest.json not found"
else
    if python3 -c 'import json, sys; json.load(open(sys.argv[1]))' "$MANIFEST" 2>/dev/null; then
        pass "manifest_valid_json"
    else
        fail "manifest_valid_json" "manifest.json is not valid JSON"
    fi
fi

# ---------------------------------------------------------------------------
# 6. manifest_three_entries (AC#2)
# ---------------------------------------------------------------------------
printf '%s\n' '--- manifest_three_entries ---'
if [ ! -f "$MANIFEST" ]; then
    fail "manifest_three_entries" "manifest.json not found"
else
    entry_count="$(python3 -c '
import json, sys
m = json.load(open(sys.argv[1]))
print(len(m.get("fixtures", [])))
' "$MANIFEST" 2>/dev/null || echo 0)"
    if [ "$entry_count" -eq 3 ]; then
        pass "manifest_three_entries"
    else
        fail "manifest_three_entries" "expected 3 fixture entries, got $entry_count"
    fi
fi

# ---------------------------------------------------------------------------
# 7. manifest_sha256_match (AC#2)
# ---------------------------------------------------------------------------
printf '%s\n' '--- manifest_sha256_match ---'
if [ ! -f "$MANIFEST" ]; then
    fail "manifest_sha256_match" "manifest.json not found"
else
    match_result="$(python3 -c '
import json, hashlib, sys, os
manifest_path = sys.argv[1]
fixtures_dir = os.path.dirname(manifest_path)
m = json.load(open(manifest_path))
checked = matched = 0
for entry in m.get("fixtures", []):
    path = entry.get("path", "")
    expected_sha = entry.get("sha256", "")
    filepath = os.path.join(fixtures_dir, path)
    if not os.path.isfile(filepath):
        continue
    actual_sha = hashlib.sha256(open(filepath, "rb").read()).hexdigest()
    checked += 1
    if actual_sha == expected_sha:
        matched += 1
if checked == 0:
    print("no_files")
elif matched == checked:
    print("all_match")
else:
    print("mismatch:%d/%d" % (matched, checked))
' "$MANIFEST" 2>/dev/null || echo "error")"

    case "$match_result" in
        all_match)   pass "manifest_sha256_match" ;;
        no_files)    fail "manifest_sha256_match" "no fixture files found to verify" ;;
        mismatch:*)  fail "manifest_sha256_match" "SHA-256 mismatch ($match_result)" ;;
        *)           fail "manifest_sha256_match" "failed to parse manifest ($match_result)" ;;
    esac
fi

# ---------------------------------------------------------------------------
# 8. incompressible (AC#4)
# ---------------------------------------------------------------------------
printf '%s\n' '--- incompressible ---'
F1M="$FIXTURES_DIR/fixture-1mb.bin"
if [ ! -f "$F1M" ]; then
    fail "incompressible" "fixture-1mb.bin not found"
else
    original_size="$(wc -c < "$F1M" | tr -d ' ')"
    compressed_size="$(gzip -c "$F1M" | wc -c | tr -d ' ')"
    threshold=$((original_size * 101 / 100))
    if [ "$compressed_size" -le "$threshold" ]; then
        pass "incompressible"
    else
        fail "incompressible" \
            "gzip ratio too high: $compressed_size / $original_size (threshold $threshold)"
    fi
fi

# ---------------------------------------------------------------------------
# 9. gitignore_exists (AC#5)
# ---------------------------------------------------------------------------
printf '%s\n' '--- gitignore_exists ---'
GITIGNORE="$FIXTURES_DIR/.gitignore"
if [ ! -f "$GITIGNORE" ]; then
    fail "gitignore_exists" ".gitignore not found in fixtures dir"
else
    has_bin=false
    has_manifest=false
    grep -qF '*.bin' "$GITIGNORE" && has_bin=true
    grep -qF 'manifest.json' "$GITIGNORE" && has_manifest=true
    if $has_bin && $has_manifest; then
        pass "gitignore_exists"
    else
        fail "gitignore_exists" \
            "missing entries (*.bin=$has_bin, manifest.json=$has_manifest)"
    fi
fi

# ---------------------------------------------------------------------------
printf '\n=== Results: %d passed, %d failed, %d skipped ===\n' "$PASS" "$FAIL" "$SKIP"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
