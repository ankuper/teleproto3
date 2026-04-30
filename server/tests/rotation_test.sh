#!/usr/bin/env bash
# rotation_test.sh — Tests for install-on-existing-nginx.sh --rotate (Story 4.9 Task 3.1)
#
# AC coverage: #1 (new path generated), #2 (nginx updated), #3 (new link printed)
# Runs without root / live network using command stubs.
#
# Usage: bash teleproto3/server/tests/rotation_test.sh
# Exit code: 0 = all pass, 1 = any failure
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
INSTALLER="${REPO_ROOT}/teleproto3/server/deploy/install-on-existing-nginx.sh"

PASS=0
FAIL=0
FAILURES=()

pass() { PASS=$((PASS+1)); echo "  ✅  PASS: $1"; }
fail() { FAIL=$((FAIL+1)); FAILURES+=("$1"); echo "  ❌  FAIL: $1"; }

assert_contains() {
    local haystack="$1" needle="$2" label="$3"
    if echo "$haystack" | grep -qF "$needle"; then pass "$label"; else fail "$label (missing '$needle')"; fi
}

assert_not_contains() {
    local haystack="$1" needle="$2" label="$3"
    if ! echo "$haystack" | grep -qF "$needle"; then pass "$label"; else fail "$label (unexpectedly found '$needle')"; fi
}

assert_rc() {
    local got="$1" expected="$2" label="$3"
    if [[ "$got" -eq "$expected" ]]; then pass "$label"; else fail "$label (rc=$got want=$expected)"; fi
}

# ---------------------------------------------------------------------------
echo ""
echo "=== Test suite: install-on-existing-nginx.sh --rotate (Story 4.9) ==="
echo ""

# R0: Installer file exists
if [[ ! -f "$INSTALLER" ]]; then
    echo "⚠️  Installer not found at $INSTALLER — all tests FAIL"
    FAIL=$((FAIL+1)); FAILURES+=("R0: installer exists")
else
    pass "R0: installer script exists"
fi

if [[ ! -f "$INSTALLER" ]]; then
    echo "Results: PASS=$PASS  FAIL=$FAIL"
    exit 1
fi

# ---------------------------------------------------------------------------
# Build stub directory (shared across all tests)
STUB_DIR="$(mktemp -d)"
TMP_LOG="$(mktemp)"
NGINX_CONF_DIR="$(mktemp -d)"
OLD_WS_PATH="/aabbccdd11223344"

# Cleanup on exit
cleanup() { rm -rf "$STUB_DIR" "$TMP_LOG" "$NGINX_CONF_DIR"; }
trap cleanup EXIT

# Standard stubs
for cmd in dig curl certbot systemctl openssl dpkg apt-get; do
    cat > "${STUB_DIR}/${cmd}" <<'STUB'
#!/bin/sh
case "$*" in
    *"ifconfig.me"*) echo "1.2.3.4" ;;
    *"+short"*)      echo "1.2.3.4" ;;
    *"is-active"*)   exit 0 ;;
    *"-v"*)  printf "%s\n" "$(basename $0) stub" ;;
    *)       echo "[stub] $0 $*" ;;
esac
exit 0
STUB
    chmod +x "${STUB_DIR}/${cmd}"
done

# docker stub — supports compose, ps, inspect
cat > "${STUB_DIR}/docker" <<'STUB'
#!/bin/sh
case "$*" in
    *"compose"*"up"*)  echo "[stub] docker compose up" ;;
    *"compose"*"down"*) echo "[stub] docker compose down" ;;
    *"--version"*)     echo "Docker version 24.0.0, build stub" ;;
    *"inspect"*)       echo '[{"State":{"Health":{"Status":"healthy"}}}]' ;;
    *"ps"*)            echo "teleproxy" ;;
    *)                 echo "[stub] docker $*" ;;
esac
exit 0
STUB
chmod +x "${STUB_DIR}/docker"

# nginx stub — supports -t and -s reload
cat > "${STUB_DIR}/nginx" <<'STUB'
#!/bin/sh
case "$*" in
    *"-t"*)    echo "nginx: configuration file /etc/nginx/nginx.conf test is successful" ;;
    *"reload"*) echo "[stub] nginx reload" ;;
    *"-s"*)    echo "[stub] nginx -s $*" ;;
    *)         echo "[stub] nginx $*" ;;
esac
exit 0
STUB
chmod +x "${STUB_DIR}/nginx"

# od stub — produces 16-hex-char output (replaces xxd, installer uses od -An -tx1)
cat > "${STUB_DIR}/od" <<'STUB'
#!/bin/sh
printf 'deadbeef11223344\n'
STUB
chmod +x "${STUB_DIR}/od"

# head stub — -c for random bytes, otherwise passthrough
cat > "${STUB_DIR}/head" <<'STUB'
#!/bin/sh
case "$*" in
    *"-c "*)
        printf '\xde\xad\xbe\xef\x11\x22\x33\x44' ;;
    *)
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

# shasum stub (for tests that need it)
cat > "${STUB_DIR}/shasum" <<'STUB'
#!/bin/sh
printf 'abc123  -\n'
STUB
chmod +x "${STUB_DIR}/shasum"

# tls-setup.sh stub (called by installer tls stage)
DEPLOY_DIR="${REPO_ROOT}/teleproto3/server/deploy"
STUB_TLS="${STUB_DIR}/tls-setup.sh"
cat > "$STUB_TLS" <<'STUB'
#!/bin/sh
echo "[stub] tls-setup.sh $*"
exit 0
STUB
chmod +x "$STUB_TLS"

# ---------------------------------------------------------------------------
# R1: --rotate flag accepted (no error)
echo "--- R1: --rotate flag accepted ---"

STATE_DIR_R1="$(mktemp -d)"
LOG_R1="$(mktemp)"
NGINX_CONF_R1="${STATE_DIR_R1}/teleproxy-ws-v2.conf"

# Seed state with an existing installation
cat > "${STATE_DIR_R1}/state.json" <<STATE
{
  "version": "1",
  "domain": "proxy.test.example.com",
  "ws_path": "${OLD_WS_PATH}",
  "secret_hex": "aabbccdd11223344aabbccdd11223344",
  "nginx_location_conf": "${NGINX_CONF_R1}",
  "stages_done": ["dns","tls","nginx","container","verify","link"],
  "installed_at": "2026-01-01T00:00:00"
}
STATE

# Seed nginx location conf with old path
cat > "${NGINX_CONF_R1}" <<NGINX
location ${OLD_WS_PATH} {
    proxy_pass http://127.0.0.1:3130;
}
NGINX

set +e
out_r1="$(TELEPROXY_LOG_OVERRIDE="$LOG_R1" \
         TELEPROXY_STATE_DIR_OVERRIDE="$STATE_DIR_R1" \
         TELEPROXY_NGINX_LOCATION_CONF_OVERRIDE="$NGINX_CONF_R1" \
         PATH="${STUB_DIR}:$PATH" \
         bash "$INSTALLER" --rotate 2>&1)"
rc_r1=$?
set -e

assert_rc "$rc_r1" "0" "R1: --rotate exits 0"
rm -rf "$STATE_DIR_R1" "$LOG_R1"

# ---------------------------------------------------------------------------
# R2: --rotate generates a new WS path different from the old one
echo "--- R2: new WS path generated (AC #1) ---"

STATE_DIR_R2="$(mktemp -d)"
LOG_R2="$(mktemp)"
NGINX_CONF_R2="${STATE_DIR_R2}/teleproxy-ws-v2.conf"

cat > "${STATE_DIR_R2}/state.json" <<STATE
{
  "version": "1",
  "domain": "proxy.test.example.com",
  "ws_path": "${OLD_WS_PATH}",
  "secret_hex": "aabbccdd11223344aabbccdd11223344",
  "nginx_location_conf": "${NGINX_CONF_R2}",
  "stages_done": ["dns","tls","nginx","container","verify","link"],
  "installed_at": "2026-01-01T00:00:00"
}
STATE

cat > "${NGINX_CONF_R2}" <<NGINX
location ${OLD_WS_PATH} {
    proxy_pass http://127.0.0.1:3130;
}
NGINX

set +e
out_r2="$(TELEPROXY_LOG_OVERRIDE="$LOG_R2" \
          TELEPROXY_STATE_DIR_OVERRIDE="$STATE_DIR_R2" \
          TELEPROXY_NGINX_LOCATION_CONF_OVERRIDE="$NGINX_CONF_R2" \
          PATH="${STUB_DIR}:$PATH" \
          bash "$INSTALLER" --rotate 2>&1)"
rc_r2=$?
set -e

# New path should appear in output and differ from OLD_WS_PATH
if [[ "$rc_r2" -eq 0 ]]; then
    # The new tg:// link should NOT contain the old domain+path encoded in the secret.
    # Old domain+path hex = hex('proxy.test.example.com' + old_ws_path)
    # New domain+path hex = hex('proxy.test.example.com' + new_ws_path from stub = '/deadbeef...')
    old_domain_path_hex="$(printf '%s%s' 'proxy.test.example.com' "${OLD_WS_PATH}" | xxd -p | tr -d '\n')"
    assert_contains "$out_r2" "deadbeef" "R2: new WS path (stub hex) appears in output"
    if echo "$out_r2" | grep "tg://proxy" | grep -qF "$old_domain_path_hex"; then
        fail "R2: tg:// link secret still encodes old domain+path (AC #1)"
    else
        pass "R2: tg:// link encodes new domain+path, not old (AC #1)"
    fi
else
    fail "R2: --rotate exited $rc_r2 — cannot check path output"
fi

rm -rf "$STATE_DIR_R2" "$LOG_R2"

# ---------------------------------------------------------------------------
# R3: --rotate prints new tg://proxy link (AC #3)
echo "--- R3: new tg:// link printed (AC #3) ---"

STATE_DIR_R3="$(mktemp -d)"
LOG_R3="$(mktemp)"
NGINX_CONF_R3="${STATE_DIR_R3}/teleproxy-ws-v2.conf"

cat > "${STATE_DIR_R3}/state.json" <<STATE
{
  "version": "1",
  "domain": "proxy.test.example.com",
  "ws_path": "${OLD_WS_PATH}",
  "secret_hex": "aabbccdd11223344aabbccdd11223344",
  "nginx_location_conf": "${NGINX_CONF_R3}",
  "stages_done": ["dns","tls","nginx","container","verify","link"],
  "installed_at": "2026-01-01T00:00:00"
}
STATE

cat > "${NGINX_CONF_R3}" <<NGINX
location ${OLD_WS_PATH} {
    proxy_pass http://127.0.0.1:3130;
}
NGINX

set +e
out_r3="$(TELEPROXY_LOG_OVERRIDE="$LOG_R3" \
          TELEPROXY_STATE_DIR_OVERRIDE="$STATE_DIR_R3" \
          TELEPROXY_NGINX_LOCATION_CONF_OVERRIDE="$NGINX_CONF_R3" \
          PATH="${STUB_DIR}:$PATH" \
          bash "$INSTALLER" --rotate 2>&1)"
rc_r3=$?
set -e

assert_rc "$rc_r3" "0" "R3: --rotate exits 0 for link check"
assert_contains "$out_r3" "tg://proxy" "R3: new tg://proxy link printed (AC #3)"
assert_contains "$out_r3" "proxy.test.example.com" "R3: link contains domain"

rm -rf "$STATE_DIR_R3" "$LOG_R3"

# ---------------------------------------------------------------------------
# R4: --rotate updates nginx conf with new path (AC #2)
echo "--- R4: nginx conf updated with new path (AC #2) ---"

STATE_DIR_R4="$(mktemp -d)"
LOG_R4="$(mktemp)"
NGINX_CONF_R4="${STATE_DIR_R4}/teleproxy-ws-v2.conf"

cat > "${STATE_DIR_R4}/state.json" <<STATE
{
  "version": "1",
  "domain": "proxy.test.example.com",
  "ws_path": "${OLD_WS_PATH}",
  "secret_hex": "aabbccdd11223344aabbccdd11223344",
  "nginx_location_conf": "${NGINX_CONF_R4}",
  "stages_done": ["dns","tls","nginx","container","verify","link"],
  "installed_at": "2026-01-01T00:00:00"
}
STATE

cat > "${NGINX_CONF_R4}" <<NGINX
location ${OLD_WS_PATH} {
    proxy_pass http://127.0.0.1:3130;
}
NGINX

set +e
out_r4="$(TELEPROXY_LOG_OVERRIDE="$LOG_R4" \
          TELEPROXY_STATE_DIR_OVERRIDE="$STATE_DIR_R4" \
          TELEPROXY_NGINX_LOCATION_CONF_OVERRIDE="$NGINX_CONF_R4" \
          PATH="${STUB_DIR}:$PATH" \
          bash "$INSTALLER" --rotate 2>&1)"
rc_r4=$?
set -e

if [[ "$rc_r4" -eq 0 ]] && [[ -f "$NGINX_CONF_R4" ]]; then
    nginx_content="$(cat "$NGINX_CONF_R4")"
    if ! echo "$nginx_content" | grep -qF "$OLD_WS_PATH"; then
        pass "R4: old path removed from nginx conf (AC #2)"
    else
        fail "R4: old path still present in nginx conf — not updated (AC #2)"
    fi
    assert_contains "$nginx_content" "deadbeef" "R4: new path written to nginx conf"
else
    fail "R4: --rotate failed or nginx conf missing (rc=$rc_r4)"
fi

rm -rf "$STATE_DIR_R4" "$LOG_R4"

# ---------------------------------------------------------------------------
# R5: --rotate updates state.json with new ws_path (AC #1)
echo "--- R5: state.json updated with new ws_path (AC #1) ---"

STATE_DIR_R5="$(mktemp -d)"
LOG_R5="$(mktemp)"
NGINX_CONF_R5="${STATE_DIR_R5}/teleproxy-ws-v2.conf"

cat > "${STATE_DIR_R5}/state.json" <<STATE
{
  "version": "1",
  "domain": "proxy.test.example.com",
  "ws_path": "${OLD_WS_PATH}",
  "secret_hex": "aabbccdd11223344aabbccdd11223344",
  "nginx_location_conf": "${NGINX_CONF_R5}",
  "stages_done": ["dns","tls","nginx","container","verify","link"],
  "installed_at": "2026-01-01T00:00:00"
}
STATE

cat > "${NGINX_CONF_R5}" <<NGINX
location ${OLD_WS_PATH} {
    proxy_pass http://127.0.0.1:3130;
}
NGINX

set +e
out_r5="$(TELEPROXY_LOG_OVERRIDE="$LOG_R5" \
          TELEPROXY_STATE_DIR_OVERRIDE="$STATE_DIR_R5" \
          TELEPROXY_NGINX_LOCATION_CONF_OVERRIDE="$NGINX_CONF_R5" \
          PATH="${STUB_DIR}:$PATH" \
          bash "$INSTALLER" --rotate 2>&1)"
rc_r5=$?
set -e

if [[ "$rc_r5" -eq 0 ]] && [[ -f "${STATE_DIR_R5}/state.json" ]]; then
    state_content="$(cat "${STATE_DIR_R5}/state.json")"
    if ! echo "$state_content" | grep -qF "$OLD_WS_PATH"; then
        pass "R5: old ws_path removed from state.json"
    else
        fail "R5: old ws_path still in state.json — not updated"
    fi
else
    fail "R5: --rotate failed or state.json missing (rc=$rc_r5)"
fi

rm -rf "$STATE_DIR_R5" "$LOG_R5"

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
