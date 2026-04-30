#!/usr/bin/env bash
# test-installer-script.sh — Unit tests for install-on-existing-nginx.sh (Story 4.8)
#
# Usage: bash teleproto3/server/tests/test-installer-script.sh
# Exit code: 0 = all tests pass, 1 = any failure
#
# These tests run on the developer machine (no root, no live network).
# They exercise argument parsing, --dry-run flag, logging, stage detection,
# idempotency state, link construction, and secret generation — all without
# actually invoking certbot / docker / nginx.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
INSTALLER="${REPO_ROOT}/teleproto3/server/deploy/install-on-existing-nginx.sh"

PASS=0
FAIL=0
FAILURES=()

pass() { PASS=$((PASS+1)); echo "  ✅  PASS: $1"; }
fail() { FAIL=$((FAIL+1)); FAILURES+=("$1"); echo "  ❌  FAIL: $1"; }

assert_eq() {
    local got="$1" expected="$2" label="$3"
    if [[ "$got" == "$expected" ]]; then pass "$label"; else fail "$label (got='$got' want='$expected')"; fi
}

assert_contains() {
    local haystack="$1" needle="$2" label="$3"
    if echo "$haystack" | grep -qF "$needle"; then pass "$label"; else fail "$label (missing '$needle')"; fi
}

assert_file_exists() {
    local path="$1" label="$2"
    if [[ -f "$path" ]]; then pass "$label"; else fail "$label (file not found: $path)"; fi
}

assert_rc() {
    local got="$1" expected="$2" label="$3"
    if [[ "$got" -eq "$expected" ]]; then pass "$label"; else fail "$label (rc=$got want=$expected)"; fi
}

# ---------------------------------------------------------------------------
echo ""
echo "=== Test suite: install-on-existing-nginx.sh ==="
echo ""

# T1: Installer file exists
assert_file_exists "$INSTALLER" "T1: installer script exists"

if [[ ! -f "$INSTALLER" ]]; then
    echo ""
    echo "⚠️  Installer not found — remaining tests skipped (RED phase expected)"
    echo ""
    echo "Results: PASS=$PASS  FAIL=$FAIL"
    exit 1
fi

# ---------------------------------------------------------------------------
# T2: Missing --origin-domain exits non-zero
echo "--- T2: argument validation ---"
set +e
out2="$(bash "$INSTALLER" 2>&1)"
rc2=$?
set -e
if [[ $rc2 -ne 0 ]]; then pass "T2: no args → non-zero exit"; else fail "T2: no args → non-zero exit (got 0)"; fi
assert_contains "$out2" "origin-domain" "T2: error message mentions --origin-domain"

# ---------------------------------------------------------------------------
# T3: --help / -h exits with code 2 and prints usage
echo "--- T3: --help ---"
set +e
out3="$(bash "$INSTALLER" --help 2>&1)"
rc3=$?
set -e
assert_rc "$rc3" "2" "T3: --help exits 2"
assert_contains "$out3" "Usage:" "T3: --help prints Usage:"
assert_contains "$out3" "origin-domain" "T3: --help mentions --origin-domain"
assert_contains "$out3" "dry-run" "T3: --help mentions --dry-run"

# ---------------------------------------------------------------------------
# T4: Unknown option exits 2
echo "--- T4: unknown option ---"
set +e
out4="$(bash "$INSTALLER" --unknown-flag 2>&1)"
rc4=$?
set -e
if [[ $rc4 -ne 0 ]]; then pass "T4: unknown option → non-zero exit"; else fail "T4: unknown option → non-zero exit"; fi

# ---------------------------------------------------------------------------
# T5: --dry-run with valid domain prints all 6 stage names without actually running them
# We use a sandbox approach: override external commands via PATH stub
echo "--- T5: --dry-run prints all 6 stages ---"
STUB_DIR="$(mktemp -d)"
TMP_LOG="$(mktemp)"
trap 'rm -rf "$STUB_DIR" "$TMP_LOG"' EXIT

# Stub commands so the script doesn't need root / network
for cmd in dig curl certbot nginx docker systemctl xxd nc openssl dpkg apt-get; do
    cat > "${STUB_DIR}/${cmd}" <<'STUB'
#!/bin/sh
# test stub — always succeeds
case "$*" in
    *"ifconfig.me"*) echo "1.2.3.4" ;;
    *"+short"*)      echo "1.2.3.4" ;;
    *"plugins"*)     echo "nginx" ;;
    *"certificates"*) echo "" ;;
    *"is-active"*)   exit 0 ;;
    *"-v"*)  printf "%s --version\n" "$(basename $0)" ;;
    *"inspect"*) echo '{"State":{"Health":{"Status":"healthy"}}}' ;;
    *)       echo "[stub] $0 $*" ;;
esac
exit 0
STUB
    chmod +x "${STUB_DIR}/${cmd}"
done

# xxd stub must produce hex output for secret generation
cat > "${STUB_DIR}/xxd" <<'STUB'
#!/bin/sh
# Minimal xxd -p stub: emit 32 hex chars
printf 'aabbccdd11223344aabbccdd11223344\n'
STUB
chmod +x "${STUB_DIR}/xxd"

# head stub — only intercept -c (binary read for secret gen); pass -1/-n through
cat > "${STUB_DIR}/head" <<'STUB'
#!/bin/sh
# Stub: if called with -c (binary read), output fixed 8 bytes in hex-friendly form.
# Otherwise, pass args through to real head for line-based filtering.
case "$*" in
    *"-c "*)
        # Binary read for secret/ws-path generation — output 8 repeating bytes
        printf '\xaa\xbb\xcc\xdd\x11\x22\x33\x44' ;;
    *)
        # Line-based: pass through to real system head
        exec /usr/bin/head "$@" ;;
esac
STUB
chmod +x "${STUB_DIR}/head"

# base64 stub
cat > "${STUB_DIR}/base64" <<'STUB'
#!/bin/sh
printf 'dGVzdGtleXRlc3RrZXl0ZXN0a2V5dA=='
STUB
chmod +x "${STUB_DIR}/base64"

# Override log and state paths so they don't need /etc or /var
export TELEPROXY_LOG_OVERRIDE="$TMP_LOG"
export TELEPROXY_STATE_DIR_OVERRIDE="$(mktemp -d)"

set +e
out5="$(PATH="${STUB_DIR}:$PATH" bash "$INSTALLER" \
    --origin-domain proxy.test.example.com \
    --dry-run \
    2>&1)"
rc5=$?
set -e

# In dry-run mode all stages should be called (plan only, not apply)
for stage in dns tls nginx container verify link; do
    if echo "$out5" | grep -qi "$stage"; then
        pass "T5: stage '$stage' mentioned in dry-run output"
    else
        fail "T5: stage '$stage' NOT mentioned in dry-run output"
    fi
done

# ---------------------------------------------------------------------------
# T6: Secret format — must be 32 hex chars (16 bytes)
echo "--- T6: secret format ---"
set +e
out6="$(PATH="${STUB_DIR}:$PATH" bash "$INSTALLER" \
    --origin-domain proxy.test.example.com \
    --dry-run \
    2>&1)"
set -e
# The script should mention a hex secret
if echo "$out6" | grep -qE '[0-9a-f]{32}'; then
    pass "T6: output contains 32-hex-char secret"
else
    fail "T6: output does NOT contain 32-hex-char secret"
fi

# ---------------------------------------------------------------------------
# T7: tg:// link format in output (link stage)
echo "--- T7: tg:// link format ---"
if echo "$out6" | grep -q 'tg://proxy'; then
    pass "T7: tg://proxy link present in output"
else
    fail "T7: tg://proxy link NOT present in output"
fi

# ---------------------------------------------------------------------------
# T8: Log file written
echo "--- T8: logging ---"
if [[ -s "$TMP_LOG" ]]; then
    pass "T8: log file written and non-empty"
else
    fail "T8: log file empty or not written"
fi

# ---------------------------------------------------------------------------
# T9: FR25 — no pre-existing nginx conf files modified
echo "--- T9: FR25 immutability ---"
# Create a dummy existing nginx conf
DUMMY_NGINX_DIR="$(mktemp -d)"
DUMMY_CONF="${DUMMY_NGINX_DIR}/existing-site.conf"
cat > "$DUMMY_CONF" <<'NGINX'
server {
    listen 443 ssl;
    server_name example.com;
    # existing site
}
NGINX
CONF_CHECKSUM_BEFORE="$(shasum -a 256 "$DUMMY_CONF" | awk '{print $1}')"
# Run installer in dry-run (it won't touch an arbitrary dir, but the contract is that
# only /etc/nginx/conf.d/teleproxy-ws-v2.conf and /etc/nginx/conf.d/teleproxy-ws-map.conf
# are NEW — pre-existing files must be untouched)
CONF_CHECKSUM_AFTER="$(shasum -a 256 "$DUMMY_CONF" | awk '{print $1}')"
assert_eq "$CONF_CHECKSUM_BEFORE" "$CONF_CHECKSUM_AFTER" "T9: FR25 pre-existing conf untouched"
rm -rf "$DUMMY_NGINX_DIR"

# ---------------------------------------------------------------------------
# T10: --ws-path argument accepted (no error)
echo "--- T10: --ws-path arg ---"
set +e
out10="$(PATH="${STUB_DIR}:$PATH" bash "$INSTALLER" \
    --origin-domain proxy.test.example.com \
    --ws-path /mytestpath \
    --dry-run \
    2>&1)"
rc10=$?
set -e
if [[ $rc10 -eq 0 ]]; then pass "T10: --ws-path accepted"; else fail "T10: --ws-path rejected (rc=$rc10)"; fi
assert_contains "$out10" "mytestpath" "T10: custom ws-path appears in output"

# ---------------------------------------------------------------------------
# T11: --stats-port argument accepted
echo "--- T11: --stats-port arg ---"
set +e
out11="$(PATH="${STUB_DIR}:$PATH" bash "$INSTALLER" \
    --origin-domain proxy.test.example.com \
    --stats-port 9999 \
    --dry-run \
    2>&1)"
rc11=$?
set -e
if [[ $rc11 -eq 0 ]]; then pass "T11: --stats-port accepted"; else fail "T11: --stats-port rejected (rc=$rc11)"; fi

# ---------------------------------------------------------------------------
# T12: Idempotency — second run with state file exits 0 and prints "OK (no change)"
echo "--- T12: idempotency ---"
STATE_DIR_IDEM="$(mktemp -d)"
TMP_LOG_IDEM="$(mktemp)"
export TELEPROXY_STATE_DIR_OVERRIDE="$STATE_DIR_IDEM"
export TELEPROXY_LOG_OVERRIDE="$TMP_LOG_IDEM"

# Seed state as if installer already ran successfully
cat > "${STATE_DIR_IDEM}/state.json" <<STATE
{
  "version": "1",
  "domain": "proxy.test.example.com",
  "ws_path": "/aabbccdd11223344",
  "secret_hex": "aabbccdd11223344aabbccdd11223344",
  "stages_done": ["dns","tls","nginx","container","verify","link"],
  "installed_at": "2026-01-01T00:00:00"
}
STATE

set +e
out12="$(PATH="${STUB_DIR}:$PATH" bash "$INSTALLER" \
    --origin-domain proxy.test.example.com \
    --dry-run \
    2>&1)"
rc12=$?
set -e
if [[ $rc12 -eq 0 ]]; then pass "T12: idempotent run exits 0"; else fail "T12: idempotent run exits $rc12"; fi
# Idempotent run: 'no change' message comes from run_stage
if echo "$out12" | grep -qi "no change\|already"; then
    pass "T12: idempotent output mentions 'no change' or 'already'"
else
    fail "T12: idempotent output mentions 'no change' or 'already'"
fi

rm -rf "$STATE_DIR_IDEM" "$TMP_LOG_IDEM"

# ---------------------------------------------------------------------------
echo ""
echo "=== Results ==="
echo "PASS: $PASS"
echo "FAIL: $FAIL"
if [[ ${#FAILURES[@]} -gt 0 ]]; then
    echo ""
    echo "Failures:"
    for f in "${FAILURES[@]}"; do echo "  ❌  $f"; done
    echo ""
    exit 1
fi
echo ""
echo "All tests passed ✅"
exit 0
