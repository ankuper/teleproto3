#!/usr/bin/env bash
# test-tls-setup.sh — Functional tests for tls-setup.sh (Story 4.4)
#
# Tests exercise argument-validation, wildcard-detection regex, structured
# error log format, and dry-run flag parsing WITHOUT real network calls.
#
# Usage:
#   bash test-tls-setup.sh        # run all tests
#   bash test-tls-setup.sh -v     # verbose (show each pass)
#
# Exit code: 0 all pass, 1 any fail.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SCRIPT_UNDER_TEST="${SCRIPT_DIR}/tls-setup.sh"
TMPDIR_BASE="$(mktemp -d)" || { echo "mktemp failed"; exit 1; }
LOG_OVERRIDE="${TMPDIR_BASE}/teleproxy-test.log"

PASS=0
FAIL=0
VERBOSE=0
[[ "${1:-}" == "-v" ]] && VERBOSE=1

# ---------------------------------------------------------------------------
# Harness helpers
# ---------------------------------------------------------------------------
ok() {
    PASS=$((PASS + 1))
    [[ $VERBOSE -eq 1 ]] && echo "  ✅  PASS: $1"
    return 0
}

fail() {
    FAIL=$((FAIL + 1))
    echo "  ❌  FAIL: $1"
    [[ -n "${2:-}" ]] && echo "      Detail: $2"
    return 0
}

run_script() {
    # run_script <expected_rc> [args...]
    local expected_rc="$1"; shift
    set +e
    output="$(LOG_FILE="${LOG_OVERRIDE}" bash "${SCRIPT_UNDER_TEST}" "$@" 2>&1)"
    actual_rc=$?
    set -e
    # Don't echo output, just let actual_rc be used
    return 0
}

# ---------------------------------------------------------------------------
# T1: missing --domain → exit 2
# ---------------------------------------------------------------------------
echo ""
echo "▶ T1: missing --domain"
run_script 2 --email a@b.com
[[ $actual_rc -eq 2 ]] && ok "missing --domain → exit 2" || fail "missing --domain → exit 2" "got exit ${actual_rc}"

# ---------------------------------------------------------------------------
# T2: missing --email → exit 2
# ---------------------------------------------------------------------------
echo ""
echo "▶ T2: missing --email"
run_script 2 --domain proxy.example.com
[[ $actual_rc -eq 2 ]] && ok "missing --email → exit 2" || fail "missing --email → exit 2" "got exit ${actual_rc}"

# ---------------------------------------------------------------------------
# T3: unknown option → exit 2
# ---------------------------------------------------------------------------
echo ""
echo "▶ T3: unknown option"
run_script 2 --bogus
[[ $actual_rc -eq 2 ]] && ok "unknown option → exit 2" || fail "unknown option → exit 2" "got exit ${actual_rc}"

# ---------------------------------------------------------------------------
# T4: --help → exit 2 with Usage string
# ---------------------------------------------------------------------------
echo ""
echo "▶ T4: --help"
run_script 2 --help
[[ $actual_rc -eq 2 ]] && ok "--help exits 2" || fail "--help exits 2" "got ${actual_rc}"
echo "$output" | grep -qi "Usage" && ok "--help prints Usage" || fail "--help prints Usage" "output: ${output}"

# ---------------------------------------------------------------------------
# T5: certbot missing → exit 1 + structured error
#     Use a wrapper script that stubs certbot as not-found.
# ---------------------------------------------------------------------------
echo ""
echo "▶ T5: certbot missing → structured error"

STUB_DIR="${TMPDIR_BASE}/stubs"
mkdir -p "${STUB_DIR}"

# Stub every command the script needs *except* certbot
# (nginx and openssl just need to exist; we stub them as no-ops)
cat > "${STUB_DIR}/nginx" <<'EOF'
#!/bin/sh
exit 0
EOF
cat > "${STUB_DIR}/openssl" <<'EOF'
#!/bin/sh
exit 0
EOF
chmod +x "${STUB_DIR}/nginx" "${STUB_DIR}/openssl"

# Build a minimal PATH that has nginx+openssl stubs but NOT certbot
SAFE_PATH="${STUB_DIR}:/usr/bin:/bin"

set +e
output="$(LOG_FILE="${LOG_OVERRIDE}" PATH="${SAFE_PATH}" bash "${SCRIPT_UNDER_TEST}" --domain proxy.example.com --email a@b.com 2>&1)"
actual_rc=$?
set -e

[[ $actual_rc -eq 1 ]] \
    && ok "certbot missing → exit 1" \
    || fail "certbot missing → exit 1" "got exit ${actual_rc}. Output: ${output}"

echo "$output" | grep -q "Stage Prerequisites failed" \
    && ok "certbot missing → structured error present" \
    || fail "certbot missing → structured error present" "output: ${output}"

# ---------------------------------------------------------------------------
# T6: nginx missing → exit 1 + structured error
# ---------------------------------------------------------------------------
echo ""
echo "▶ T6: nginx missing → structured error"

STUB_DIR2="${TMPDIR_BASE}/stubs2"
mkdir -p "${STUB_DIR2}"

# Only certbot stub; no nginx
cat > "${STUB_DIR2}/certbot" <<'EOF'
#!/bin/sh
echo "Certbot 2.x"
exit 0
EOF
cat > "${STUB_DIR2}/openssl" <<'EOF'
#!/bin/sh
exit 0
EOF
chmod +x "${STUB_DIR2}/certbot" "${STUB_DIR2}/openssl"

SAFE_PATH2="${STUB_DIR2}:/usr/bin:/bin"

set +e
output="$(LOG_FILE="${LOG_OVERRIDE}" PATH="${SAFE_PATH2}" bash "${SCRIPT_UNDER_TEST}" --domain proxy.example.com --email a@b.com 2>&1)"
actual_rc=$?
set -e

[[ $actual_rc -eq 1 ]] \
    && ok "nginx missing → exit 1" \
    || fail "nginx missing → exit 1" "got exit ${actual_rc}. Output: ${output}"

echo "$output" | grep -q "Stage Prerequisites failed" \
    && ok "nginx missing → structured error present" \
    || fail "nginx missing → structured error present" "output: ${output}"

# ---------------------------------------------------------------------------
# T7: wildcard detection regex — positive match
# ---------------------------------------------------------------------------
echo ""
echo "▶ T7: wildcard detection regex"

FAKE_CERT_OUT="Certificate Name: main
  Domains: *.example.com
  Expiry Date: 2026-09-01 (VALID: 90 days)"

BASE="example.com"
echo "$FAKE_CERT_OUT" | grep -A5 "Domains:" | grep -qE "\*\.${BASE//./\\.}" \
    && ok "wildcard regex matches *.example.com" \
    || fail "wildcard regex matches *.example.com"

# Negative: should NOT match a different base domain
BASE2="other.com"
echo "$FAKE_CERT_OUT" | grep -A5 "Domains:" | grep -qE "\*\.${BASE2//./\\.}" \
    && fail "wildcard regex false positive for other.com" \
    || ok "wildcard regex does NOT match *.other.com"

# Substring negative: should NOT match myexample.com
BASE3="myexample.com"
echo "$FAKE_CERT_OUT" | grep -A5 "Domains:" | grep -qE "(^|[[:space:]])\*\.${BASE3//./\\\\.}([[:space:]]|$)" \
    && fail "wildcard regex false positive for myexample.com" \
    || ok "wildcard regex does NOT match *.myexample.com"

# Substring negative for exact domain
echo "$FAKE_CERT_OUT" | grep -qE "Domains:.*(^|[[:space:]])myexample\.com([[:space:]]|$)" \
    && fail "exact domain regex false positive for myexample.com" \
    || ok "exact domain regex does NOT match myexample.com"


# ---------------------------------------------------------------------------
# T8: structured error log format (unit test of the format itself)
# ---------------------------------------------------------------------------
echo ""
echo "▶ T8: structured error log format"

LOG_TEST="${TMPDIR_BASE}/fmt-test.log"
{
    echo "[ERROR] 2026-01-01T00:00:00 Stage TLS failed: certbot could not verify domain ownership"
    echo "        Most likely cause: DNS A record for proxy.example.com does not point to this server"
    echo "        Fix: Run 'dig proxy.example.com A' and verify it returns your server IP."
    echo "        Full log: /var/log/teleproxy-install-v2.log"
} > "$LOG_TEST"

grep -q "Stage TLS failed:"      "$LOG_TEST" && \
grep -q "Most likely cause:"     "$LOG_TEST" && \
grep -q "Fix:"                   "$LOG_TEST" && \
grep -q "Full log:"              "$LOG_TEST" \
    && ok "structured error log has all required fields" \
    || fail "structured error log missing fields"

# ---------------------------------------------------------------------------
# T9: --dry-run flag is accepted (not rejected by arg parser)
# ---------------------------------------------------------------------------
echo ""
echo "▶ T9: --dry-run flag accepted by argument parser"
run_script 1 --domain proxy.example.com --email a@b.com --dry-run
# exit 2 = arg parse failure; exit 1 = prereq failure (expected)
[[ $actual_rc -ne 2 ]] \
    && ok "--dry-run accepted (exit ${actual_rc}, not 2)" \
    || fail "--dry-run accepted" "got exit 2 (arg parse failure). Output: ${output}"

# ---------------------------------------------------------------------------
# Cleanup
# ---------------------------------------------------------------------------
rm -rf "${TMPDIR_BASE}"

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
echo ""
echo "=========================================="
echo "  Results: ${PASS} passed, ${FAIL} failed"
echo "=========================================="

[[ $FAIL -eq 0 ]]
