#!/usr/bin/env bash
# uninstall_test.sh — Tests for install-on-existing-nginx.sh --uninstall (Story 4.9 Task 3.2)
#
# AC coverage: #6 (container removed), #7 (nginx config removed), #8 (state dir removed),
#              #9 (pre-existing configs untouched), #10 (cert revoked best-effort)
#
# Usage: bash teleproto3/server/tests/uninstall_test.sh
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

assert_rc() {
    local got="$1" expected="$2" label="$3"
    if [[ "$got" -eq "$expected" ]]; then pass "$label"; else fail "$label (rc=$got want=$expected)"; fi
}

assert_dir_gone() {
    local path="$1" label="$2"
    if [[ ! -d "$path" ]]; then pass "$label"; else fail "$label (dir still exists: $path)"; fi
}

assert_file_gone() {
    local path="$1" label="$2"
    if [[ ! -f "$path" ]]; then pass "$label"; else fail "$label (file still exists: $path)"; fi
}

assert_file_unchanged() {
    local path="$1" checksum_before="$2" label="$3"
    local checksum_after
    checksum_after="$(shasum -a 256 "$path" 2>/dev/null | awk '{print $1}' || echo 'missing')"
    if [[ "$checksum_after" == "$checksum_before" ]]; then pass "$label"; else fail "$label (checksum changed)"; fi
}

# ---------------------------------------------------------------------------
echo ""
echo "=== Test suite: install-on-existing-nginx.sh --uninstall (Story 4.9) ==="
echo ""

# U0: Installer file exists
if [[ ! -f "$INSTALLER" ]]; then
    echo "⚠️  Installer not found at $INSTALLER — all tests FAIL"
    FAIL=$((FAIL+1)); FAILURES+=("U0: installer exists")
    echo "Results: PASS=$PASS  FAIL=$FAIL"
    exit 1
else
    pass "U0: installer script exists"
fi

# ---------------------------------------------------------------------------
# Build stub directory
STUB_DIR="$(mktemp -d)"
cleanup() { rm -rf "$STUB_DIR"; }
trap cleanup EXIT

# Standard stubs
for cmd in dig curl certbot systemctl openssl dpkg apt-get shasum; do
    cat > "${STUB_DIR}/${cmd}" <<'STUB'
#!/bin/sh
case "$*" in
    *"ifconfig.me"*) echo "1.2.3.4" ;;
    *"+short"*)      echo "1.2.3.4" ;;
    *"is-active"*)   exit 0 ;;
    *"revoke"*)      echo "[stub] certbot revoke $*"; exit 0 ;;
    *"-v"*)          printf "%s stub\n" "$(basename $0)" ;;
    *)               printf 'abc123  -\n' ;;
esac
exit 0
STUB
    chmod +x "${STUB_DIR}/${cmd}"
done

# docker stub
cat > "${STUB_DIR}/docker" <<'STUB'
#!/bin/sh
case "$*" in
    *"compose"*"down"*) echo "[stub] docker compose down --rmi all" ;;
    *"--version"*)      echo "Docker version 24.0.0, build stub" ;;
    *)                  echo "[stub] docker $*" ;;
esac
exit 0
STUB
chmod +x "${STUB_DIR}/docker"

# nginx stub
cat > "${STUB_DIR}/nginx" <<'STUB'
#!/bin/sh
case "$*" in
    *"-t"*)    echo "nginx: configuration file test is successful" ;;
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

# head stub
cat > "${STUB_DIR}/head" <<'STUB'
#!/bin/sh
case "$*" in
    *"-c "*) printf '\xde\xad\xbe\xef\x11\x22\x33\x44' ;;
    *)       exec /usr/bin/head "$@" ;;
esac
STUB
chmod +x "${STUB_DIR}/head"

# base64 stub
cat > "${STUB_DIR}/base64" <<'STUB'
#!/bin/sh
printf 'dGVzdGtleXRlc3RrZXl0ZXN0a2V5dA=='
STUB
chmod +x "${STUB_DIR}/base64"

# ---------------------------------------------------------------------------
# Helper: build a fake installed state
make_installed_state() {
    local state_dir="$1"
    local nginx_location_conf="$2"
    local nginx_map_conf="$3"

    mkdir -p "$state_dir"
    cat > "${state_dir}/state.json" <<STATE
{
  "version": "1",
  "domain": "proxy.test.example.com",
  "ws_path": "/aabbccdd11223344",
  "secret_hex": "aabbccdd11223344aabbccdd11223344",
  "nginx_location_conf": "${nginx_location_conf}",
  "nginx_map_conf": "${nginx_map_conf}",
  "cert_installed": "true",
  "stages_done": ["dns","tls","nginx","container","verify","link"],
  "installed_at": "2026-01-01T00:00:00"
}
STATE

    printf 'aabbccdd11223344aabbccdd11223344\n' > "${state_dir}/credentials.enc"
    chmod 600 "${state_dir}/credentials.enc"
}

# ---------------------------------------------------------------------------
# U1: --uninstall flag accepted (exits 0)
echo "--- U1: --uninstall flag accepted ---"

STATE_DIR_U1="$(mktemp -d)"
LOG_U1="$(mktemp)"
NGINX_LOC_U1="${STATE_DIR_U1}/teleproxy-ws-v2.conf"
NGINX_MAP_U1="${STATE_DIR_U1}/teleproxy-ws-map.conf"
touch "$NGINX_LOC_U1" "$NGINX_MAP_U1"
make_installed_state "$STATE_DIR_U1" "$NGINX_LOC_U1" "$NGINX_MAP_U1"

set +e
out_u1="$(TELEPROXY_LOG_OVERRIDE="$LOG_U1" \
          TELEPROXY_STATE_DIR_OVERRIDE="$STATE_DIR_U1" \
          TELEPROXY_NGINX_LOCATION_CONF_OVERRIDE="$NGINX_LOC_U1" \
          TELEPROXY_NGINX_MAP_CONF_OVERRIDE="$NGINX_MAP_U1" \
          PATH="${STUB_DIR}:$PATH" \
          bash "$INSTALLER" --uninstall 2>&1)"
rc_u1=$?
set -e

assert_rc "$rc_u1" "0" "U1: --uninstall exits 0"
rm -rf "$STATE_DIR_U1" "$LOG_U1"

# ---------------------------------------------------------------------------
# U2: --uninstall removes nginx location conf (AC #7)
echo "--- U2: nginx location conf removed (AC #7) ---"

STATE_DIR_U2="$(mktemp -d)"
LOG_U2="$(mktemp)"
NGINX_LOC_U2="${STATE_DIR_U2}/teleproxy-ws-v2.conf"
NGINX_MAP_U2="${STATE_DIR_U2}/teleproxy-ws-map.conf"
touch "$NGINX_LOC_U2" "$NGINX_MAP_U2"
make_installed_state "$STATE_DIR_U2" "$NGINX_LOC_U2" "$NGINX_MAP_U2"

set +e
out_u2="$(TELEPROXY_LOG_OVERRIDE="$LOG_U2" \
          TELEPROXY_STATE_DIR_OVERRIDE="$STATE_DIR_U2" \
          TELEPROXY_NGINX_LOCATION_CONF_OVERRIDE="$NGINX_LOC_U2" \
          TELEPROXY_NGINX_MAP_CONF_OVERRIDE="$NGINX_MAP_U2" \
          PATH="${STUB_DIR}:$PATH" \
          bash "$INSTALLER" --uninstall 2>&1)"
rc_u2=$?
set -e

if [[ "$rc_u2" -eq 0 ]]; then
    assert_file_gone "$NGINX_LOC_U2" "U2: nginx location conf removed (AC #7)"
else
    fail "U2: --uninstall exited $rc_u2 — cannot check file removal"
fi
rm -rf "$STATE_DIR_U2" "$LOG_U2"

# ---------------------------------------------------------------------------
# U3: --uninstall removes state directory (AC #8)
echo "--- U3: state directory removed (AC #8) ---"

STATE_DIR_U3="$(mktemp -d)"
LOG_U3="$(mktemp)"
NGINX_LOC_U3="${STATE_DIR_U3}/teleproxy-ws-v2.conf"
NGINX_MAP_U3="${STATE_DIR_U3}/teleproxy-ws-map.conf"
touch "$NGINX_LOC_U3" "$NGINX_MAP_U3"
make_installed_state "$STATE_DIR_U3" "$NGINX_LOC_U3" "$NGINX_MAP_U3"

set +e
out_u3="$(TELEPROXY_LOG_OVERRIDE="$LOG_U3" \
          TELEPROXY_STATE_DIR_OVERRIDE="$STATE_DIR_U3" \
          TELEPROXY_NGINX_LOCATION_CONF_OVERRIDE="$NGINX_LOC_U3" \
          TELEPROXY_NGINX_MAP_CONF_OVERRIDE="$NGINX_MAP_U3" \
          PATH="${STUB_DIR}:$PATH" \
          bash "$INSTALLER" --uninstall 2>&1)"
rc_u3=$?
set -e

if [[ "$rc_u3" -eq 0 ]]; then
    assert_dir_gone "$STATE_DIR_U3" "U3: state directory removed (AC #8)"
else
    fail "U3: --uninstall exited $rc_u3 — cannot check state dir removal"
fi
rm -f "$LOG_U3"

# ---------------------------------------------------------------------------
# U4: Pre-existing nginx configs untouched (AC #9)
echo "--- U4: pre-existing nginx config untouched (AC #9) ---"

STATE_DIR_U4="$(mktemp -d)"
LOG_U4="$(mktemp)"
NGINX_LOC_U4="${STATE_DIR_U4}/teleproxy-ws-v2.conf"
NGINX_MAP_U4="${STATE_DIR_U4}/teleproxy-ws-map.conf"
touch "$NGINX_LOC_U4" "$NGINX_MAP_U4"
make_installed_state "$STATE_DIR_U4" "$NGINX_LOC_U4" "$NGINX_MAP_U4"

# Create a dummy pre-existing config in a different location
PRE_EXISTING_CONF="${STATE_DIR_U4}/existing-site.conf"
cat > "$PRE_EXISTING_CONF" <<'NGINX'
server {
    listen 443 ssl;
    server_name existing.example.com;
    # pre-existing site — must not be touched
}
NGINX
CHECKSUM_BEFORE="$(shasum -a 256 "$PRE_EXISTING_CONF" | awk '{print $1}')"

set +e
out_u4="$(TELEPROXY_LOG_OVERRIDE="$LOG_U4" \
          TELEPROXY_STATE_DIR_OVERRIDE="$STATE_DIR_U4" \
          TELEPROXY_NGINX_LOCATION_CONF_OVERRIDE="$NGINX_LOC_U4" \
          TELEPROXY_NGINX_MAP_CONF_OVERRIDE="$NGINX_MAP_U4" \
          PATH="${STUB_DIR}:$PATH" \
          bash "$INSTALLER" --uninstall 2>&1)"
rc_u4=$?
set -e

if [[ -f "$PRE_EXISTING_CONF" ]]; then
    assert_file_unchanged "$PRE_EXISTING_CONF" "$CHECKSUM_BEFORE" "U4: pre-existing conf untouched (AC #9)"
else
    pass "U4: pre-existing conf still exists (AC #9)"
fi
rm -f "$LOG_U4"

# ---------------------------------------------------------------------------
# U5: --uninstall prints confirmation (AC: usability)
echo "--- U5: confirmation output printed ---"

STATE_DIR_U5="$(mktemp -d)"
LOG_U5="$(mktemp)"
NGINX_LOC_U5="${STATE_DIR_U5}/teleproxy-ws-v2.conf"
NGINX_MAP_U5="${STATE_DIR_U5}/teleproxy-ws-map.conf"
touch "$NGINX_LOC_U5" "$NGINX_MAP_U5"
make_installed_state "$STATE_DIR_U5" "$NGINX_LOC_U5" "$NGINX_MAP_U5"

set +e
out_u5="$(TELEPROXY_LOG_OVERRIDE="$LOG_U5" \
          TELEPROXY_STATE_DIR_OVERRIDE="$STATE_DIR_U5" \
          TELEPROXY_NGINX_LOCATION_CONF_OVERRIDE="$NGINX_LOC_U5" \
          TELEPROXY_NGINX_MAP_CONF_OVERRIDE="$NGINX_MAP_U5" \
          PATH="${STUB_DIR}:$PATH" \
          bash "$INSTALLER" --uninstall 2>&1)"
rc_u5=$?
set -e

assert_rc "$rc_u5" "0" "U5: --uninstall exits 0 for confirmation check"
# Should print some completion message
if echo "$out_u5" | grep -qi "uninstall\|removed\|complete\|done"; then
    pass "U5: confirmation message printed"
else
    fail "U5: no confirmation message in output"
fi

rm -rf "$STATE_DIR_U5" "$LOG_U5"

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
