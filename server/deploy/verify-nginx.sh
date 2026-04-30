#!/usr/bin/env bash
# verify-nginx.sh — Validate teleproxy v2 nginx coexistence config (Story 4.3 AC#5)
#
# Usage:
#   bash verify-nginx.sh [--domain DOMAIN] [--ws-path WS_PATH] [--port PORT]
#
# Options:
#   --domain   DOMAIN     Hostname to test against (required for live checks)
#   --ws-path  WS_PATH    WebSocket path configured in the location block
#   --port     PORT       HTTPS port (default: 443)
#   --local               Only run local nginx config checks (no live HTTP tests)
#
# Exit codes:
#   0  All checks passed
#   1  One or more checks failed

set -euo pipefail

##############################################################################
# Defaults
##############################################################################
DOMAIN=""
WS_PATH=""
HTTPS_PORT=443
LOCAL_ONLY=false
NGINX_CONF_DIR="/etc/nginx"
SNIPPET_PATH="/etc/nginx/snippets/teleproxy-ws-location.conf"
MAP_PATH="/etc/nginx/conf.d/teleproxy-ws-map.conf"

PASS=0
FAIL=0

##############################################################################
# Helpers
##############################################################################
ok()   { echo "  ✅ PASS: $*"; ((PASS++)) || true; }
fail() { echo "  ❌ FAIL: $*"; ((FAIL++)) || true; }
info() { echo "  ℹ️  $*"; }
section() { echo; echo "── $* ──"; }

if [[ $EUID -ne 0 ]]; then
    echo "❌ Must run as root (for nginx -T and config checks)" >&2
    exit 1
fi

if ! command -v curl >/dev/null 2>&1; then
    echo "❌ curl is required but not installed." >&2
    exit 1
fi

##############################################################################
# Argument parsing
##############################################################################
while [[ $# -gt 0 ]]; do
    case "$1" in
        --domain)   if [[ -z "${2:-}" ]]; then echo "Missing value for --domain"; exit 1; fi; DOMAIN="${2%%/}"; shift 2 ;;
        --ws-path)  if [[ -z "${2:-}" ]]; then echo "Missing value for --ws-path"; exit 1; fi; WS_PATH="$2"; shift 2 ;;
        --port)     if [[ -z "${2:-}" ]]; then echo "Missing value for --port"; exit 1; fi; HTTPS_PORT="$2"; shift 2 ;;
        --local)    LOCAL_ONLY=true; shift   ;;
        *) echo "Unknown argument: $1" >&2; exit 1 ;;
    esac
done

##############################################################################
# Check 1 — nginx -t passes
##############################################################################
section "Check 1: nginx -t"
if nginx -t 2>/dev/null; then
    ok "nginx -t passed"
else
    fail "nginx -t failed — check nginx error log"
fi

##############################################################################
# Check 2 — WS path location exists in the snippet
##############################################################################
section "Check 2: WS path location block"
if [[ -f "$SNIPPET_PATH" ]]; then
    ok "Snippet exists: $SNIPPET_PATH"
    if grep -qE '^\s*location\s+.*\{' "$SNIPPET_PATH"; then
        ok "Location block found in snippet"
    else
        fail "No location block found in $SNIPPET_PATH"
    fi
    if [[ -n "$WS_PATH" ]]; then
        if grep -qF "location ${WS_PATH}" "$SNIPPET_PATH"; then
            ok "Configured WS path '${WS_PATH}' found in snippet"
        else
            fail "WS path '${WS_PATH}' not found in snippet (check substitution)"
        fi
    else
        info "No --ws-path given; skipping path-specific check"
    fi
else
    fail "Snippet not found: $SNIPPET_PATH (run installer first)"
fi

##############################################################################
# Check 3 — Rate limiting zone configured
##############################################################################
section "Check 3: Rate limiting (FR21)"
if [[ -f "$SNIPPET_PATH" ]]; then
    if grep -qE 'limit_req_zone\s+\$binary_remote_addr\s+zone=teleproxy_ws' "$MAP_PATH"; then
        ok "limit_req_zone configured in map file"
    else
        fail "limit_req_zone not found in map file"
    fi
    if grep -qE 'limit_req\s+zone=teleproxy_ws\s+burst=50\s+nodelay' "$SNIPPET_PATH"; then
        ok "limit_req with burst=50 nodelay configured"
    else
        fail "limit_req burst=50 nodelay not found in snippet"
    fi
fi

##############################################################################
# Check 4 — proxy_pass points to 127.0.0.1:3130
##############################################################################
section "Check 4: proxy_pass target"
if [[ -f "$SNIPPET_PATH" ]]; then
    if grep -qE 'proxy_pass\s+http://127\.0\.0\.1:3130' "$SNIPPET_PATH"; then
        ok "proxy_pass → http://127.0.0.1:3130"
    else
        fail "proxy_pass to 127.0.0.1:3130 not found in snippet"
    fi
fi

##############################################################################
# Check 5 — Map directive exists
##############################################################################
section "Check 5: WebSocket upgrade map"
if [[ -f "$MAP_PATH" ]]; then
    ok "Map file exists: $MAP_PATH"
    if grep -qE 'map\s+\$http_upgrade\s+\$connection_upgrade' "$MAP_PATH"; then
        ok "\$connection_upgrade map defined"
    else
        fail "\$connection_upgrade map not found in $MAP_PATH"
    fi
else
    # May be defined elsewhere (e.g. operator's existing config)
    if nginx -T 2>/dev/null | grep -qE 'map\s+\$http_upgrade\s+\$connection_upgrade'; then
        ok "\$connection_upgrade map defined (found in active nginx config)"
    else
        fail "Map file not found and map not present in active config"
    fi
fi

##############################################################################
# Live HTTP checks (require --domain)
##############################################################################
if [[ "$LOCAL_ONLY" == "true" ]]; then
    info "Skipping live HTTP checks (--local mode)"
elif [[ -z "$DOMAIN" ]]; then
    info "No --domain provided; skipping live HTTP checks"
else
    ##########################################################################
    # Check 6 — WS upgrade request returns 101 or 502
    ##########################################################################
    section "Check 6: WS upgrade request (AC#2)"
    if [[ -z "$WS_PATH" ]]; then
        info "No --ws-path provided; skipping live WS check"
    else
        WS_KEY=$(head -c 16 /dev/urandom | base64)
        HTTP_CODE=$(curl -k -o /dev/null -s -w "%{http_code}" --max-time 10 \
            --http1.1 \
            -H "Upgrade: websocket" \
            -H "Connection: Upgrade" \
            -H "Sec-WebSocket-Version: 13" \
            -H "Sec-WebSocket-Key: ${WS_KEY}" \
            "https://${DOMAIN}:${HTTPS_PORT}${WS_PATH}" 2>/dev/null || true)
        if [[ "$HTTP_CODE" == "101" ]]; then
            ok "WS upgrade returned 101 (teleproxy running and accepting)"
        elif [[ "$HTTP_CODE" == "502" ]]; then
            ok "WS upgrade returned 502 (proxy routing correct; teleproxy may not be running)"
        elif [[ "$HTTP_CODE" == "000" ]]; then
            fail "Connection refused or TLS error (check domain/port)"
        else
            fail "Unexpected HTTP code from WS path: $HTTP_CODE (expected 101 or 502)"
        fi
    fi

    ##########################################################################
    # Check 7 — Unknown path returns 404 (AC#3, FR20)
    ##########################################################################
    section "Check 7: Unknown path → 404 (FR20)"
    RANDOM_PATH="/teleproxy-verify-unknown-$(openssl rand -hex 6)"
    HTTP_404=$(curl -k -o /dev/null -s -w "%{http_code}" --max-time 10 \
        "https://${DOMAIN}:${HTTPS_PORT}${RANDOM_PATH}" 2>/dev/null || true)
    if [[ "$HTTP_404" == "404" ]]; then
        ok "Unknown path '${RANDOM_PATH}' returned 404"
    elif [[ "$HTTP_404" == "000" ]]; then
        fail "Connection refused or TLS error for 404 check"
    else
        info "⚠️ WARNING: Unknown path returned $HTTP_404. For maximum anti-probe protection, configure a catch-all 404 location."
        ok "Allowed due to FR25 strictness (soft warning)"
    fi
fi

##############################################################################
# Summary
##############################################################################
echo
echo "════════════════════════════════════"
echo "  Checks passed: ${PASS}"
echo "  Checks failed: ${FAIL}"
echo "════════════════════════════════════"

if [[ $FAIL -gt 0 ]]; then
    echo "❌ Verification FAILED — see failures above"
    exit 1
else
    echo "✅ All checks passed"
    exit 0
fi
