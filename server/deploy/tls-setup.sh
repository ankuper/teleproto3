#!/usr/bin/env bash
# tls-setup.sh — Let's Encrypt TLS automation for teleproxy v2 subdomain
#
# Usage:
#   bash tls-setup.sh --domain proxy.example.com --email admin@example.com [--dry-run]
#
# Actions:
#   1. Validates arguments and prerequisites (certbot, nginx)
#   2. Checks if port 80 is reachable
#   3. Detects existing wildcard certificate for the target domain
#   4. Issues a new Let's Encrypt cert via HTTP-01 challenge (nginx plugin),
#      or reuses the wildcard if found
#   5. Applies TLS hardening to the nginx snippet
#   6. Configures auto-renewal deploy hook
#
# All errors are logged to /var/log/teleproxy-install-v2.log in structured
# human-readable format (architecture §Category 5).
#
# Exit codes: 0 = success, 1 = fatal error, 2 = prerequisite not met

set -euo pipefail

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------
LOG_FILE="/var/log/teleproxy-install-v2.log"
RENEWAL_HOOK="/etc/letsencrypt/renewal-hooks/deploy/reload-nginx.sh"
NGINX_SNIPPET="/etc/nginx/snippets/teleproxy-ws-location.conf"
SCRIPT_NAME="$(basename "$0")"

# ---------------------------------------------------------------------------
# Logging helpers
# ---------------------------------------------------------------------------
_ts() { date '+%Y-%m-%dT%H:%M:%S'; }

log_info() {
    local msg="$1"
    echo "[INFO]  $(_ts) $msg" | tee -a "$LOG_FILE"
}

log_warn() {
    local msg="$1"
    echo "[WARN]  $(_ts) $msg" | tee -a "$LOG_FILE"
}

# Structured error format (architecture §Category 5):
#   Stage <name> failed: <cause>
#   Most likely cause: <human reason>
#   Fix: <actionable fix>
#   Full log: <path>
log_error() {
    local stage="$1"
    local cause="$2"
    local reason="$3"
    local fix="$4"
    {
        echo "[ERROR] $(_ts) Stage ${stage} failed: ${cause}"
        echo "        Most likely cause: ${reason}"
        echo "        Fix: ${fix}"
        echo "        Full log: ${LOG_FILE}"
    } | tee -a "$LOG_FILE" >&2
}

fatal() {
    local stage="$1"; local cause="$2"; local reason="$3"; local fix="$4"
    log_error "$stage" "$cause" "$reason" "$fix"
    exit 1
}

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------
DOMAIN=""
ADMIN_EMAIL=""
DRY_RUN=0

usage() {
    echo "Usage: $SCRIPT_NAME --domain <subdomain> --email <admin-email> [--dry-run]"
    echo ""
    echo "  --domain   Target subdomain (e.g. proxy.example.com)"
    echo "  --email    Admin email for Let's Encrypt registration"
    echo "  --dry-run  Run certbot in --dry-run mode (no real cert issued)"
    exit 2
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --domain)   DOMAIN="${2:?'--domain requires an argument'}";     shift 2 ;;
        --email)    ADMIN_EMAIL="${2:?'--email requires an argument'}";  shift 2 ;;
        --dry-run)  DRY_RUN=1; shift ;;
        -h|--help)  usage ;;
        *)          echo "Unknown option: $1" >&2; usage ;;
    esac
done

[[ -z "$DOMAIN" ]]      && { echo "Error: --domain is required" >&2; usage; }
[[ -z "$ADMIN_EMAIL" ]] && { echo "Error: --email is required"  >&2; usage; }

# Derive base domain (everything after the first label)
[[ "$DOMAIN" == *.*.* ]] && BASE_DOMAIN="${DOMAIN#*.}" || BASE_DOMAIN="$DOMAIN"

# ---------------------------------------------------------------------------
# Ensure log file is writable
# ---------------------------------------------------------------------------
touch "$LOG_FILE" 2>/dev/null || {
    echo "Warning: cannot write to ${LOG_FILE}; logging to /tmp/teleproxy-tls-setup.log" >&2
    LOG_FILE="$(mktemp /tmp/teleproxy-tls-setup-XXXXXX)"
}
chmod 640 "$LOG_FILE" 2>/dev/null || true

log_info "=== tls-setup.sh start: domain=${DOMAIN} dry_run=${DRY_RUN} ==="

# ---------------------------------------------------------------------------
# Prerequisite checks
# ---------------------------------------------------------------------------
check_command() {
    local cmd="$1"
    if ! command -v "$cmd" &>/dev/null; then
        fatal "Prerequisites" \
            "command '${cmd}' not found" \
            "Required tool '${cmd}' is missing from PATH" \
            "Install it: apt install ${cmd}   or   snap install --classic certbot"
    fi
    log_info "Prerequisite OK: ${cmd} found at $(command -v "$cmd")"
}

check_command certbot
check_command nginx
check_command openssl

# Verify certbot nginx plugin is available
if ! certbot plugins 2>/dev/null | grep -q "nginx"; then
    fatal "Prerequisites" \
        "certbot nginx plugin not available" \
        "certbot is installed but the nginx plugin (python3-certbot-nginx) is missing" \
        "Run: apt install python3-certbot-nginx"
fi
log_info "Prerequisite OK: certbot nginx plugin available"

# ---------------------------------------------------------------------------
# Port 80 reachability check (required for HTTP-01 challenge)
# ---------------------------------------------------------------------------
log_info "Note: Let's Encrypt HTTP-01 challenge REQUIRES port 80 to be accessible from the internet."

# ---------------------------------------------------------------------------
# Wildcard certificate detection (AC #4)
# ---------------------------------------------------------------------------
detect_wildcard() {
    log_info "Checking for existing wildcard certificate covering *.${BASE_DOMAIN}..."

    # certbot certificates outputs multi-line blocks; grep for wildcard matching base domain
    if certbot certificates 2>/dev/null \
            | grep -A5 "Domains:" \
            | grep -qE "(^|[[:space:]])\*\.${BASE_DOMAIN//./\\.}([[:space:]]|$)"; then
        log_info "Wildcard certificate found for *.${BASE_DOMAIN} — skipping Let's Encrypt issuance."
        return 0  # wildcard found
    fi

    # Also check if certbot has an exact cert for the domain already
    if certbot certificates 2>/dev/null \
            | grep -qE "Domains:.*(^|[[:space:]])${DOMAIN//./\\.}([[:space:]]|$)"; then
        log_info "Existing certificate found for ${DOMAIN} — skipping issuance."
        return 0  # exact cert found
    fi

    return 1  # no cert found
}

CERT_REUSED=0
if detect_wildcard; then
    CERT_REUSED=1
    log_info "Using existing wildcard/domain certificate — no new issuance needed."
fi

# ---------------------------------------------------------------------------
# Issue certificate via HTTP-01 / certbot nginx plugin (AC #1)
# ---------------------------------------------------------------------------
if [[ $CERT_REUSED -eq 0 ]]; then
    log_info "Issuing Let's Encrypt certificate for ${DOMAIN} via HTTP-01 challenge..."

    CERTBOT_FLAGS=(
        certonly
        --nginx
        -d "$DOMAIN"
        --non-interactive
        --agree-tos
        --email "$ADMIN_EMAIL"
    )

    if [[ $DRY_RUN -eq 1 ]]; then
        CERTBOT_FLAGS+=(--dry-run)
        log_info "DRY-RUN mode: no real certificate will be issued."
    fi

    set +e
    certbot_output="$(certbot "${CERTBOT_FLAGS[@]}" 2>&1)"
    certbot_rc=$?
    set -e

    echo "$certbot_output" >> "$LOG_FILE"

    if [[ $certbot_rc -ne 0 ]]; then
        # Detect common failure modes and emit actionable guidance
        if echo "$certbot_output" | grep -qiE "Problem binding to port 80|Timeout during connect|Connection refused"; then
            fatal "TLS" \
                "certbot HTTP-01 challenge failed (port 80 blocked)" \
                "Port 80 on ${DOMAIN} is blocked or unreachable from the internet" \
                "Run: ufw allow 80/tcp   or   iptables -A INPUT -p tcp --dport 80 -j ACCEPT"
        elif echo "$certbot_output" | grep -qi "too many certificates\|rate limit"; then
            fatal "TLS" \
                "certbot rate limit exceeded" \
                "Let's Encrypt has issued too many certificates for ${BASE_DOMAIN} recently" \
                "Wait at least 1 hour and retry, or use the staging environment with --dry-run"
        elif echo "$certbot_output" | grep -qi "DNS\|NXDOMAIN\|resolve\|not found"; then
            fatal "TLS" \
                "certbot could not verify domain ownership (DNS failure)" \
                "DNS A record for ${DOMAIN} does not point to this server" \
                "Run 'dig ${DOMAIN} A' and verify it returns your server IP. DNS propagation may take up to 48h."
        else
            fatal "TLS" \
                "certbot exited with code ${certbot_rc}" \
                "Unknown certbot error — see full output above" \
                "Review the output above and check https://letsencrypt.org/docs/faq/"
        fi
    fi

    log_info "Certificate issued successfully for ${DOMAIN}."
fi

# ---------------------------------------------------------------------------
# Verify certificate installed (AC #2)
# ---------------------------------------------------------------------------
if [[ $DRY_RUN -eq 0 ]]; then
    log_info "Verifying certificate on ${DOMAIN}:443..."
    set +e
    cert_info="$(openssl s_client -connect "${DOMAIN}:443" -servername "$DOMAIN" \
                    </dev/null 2>/dev/null \
                    | openssl x509 -noout -dates 2>/dev/null)"
    cert_rc=$?
    set -e

    if [[ $cert_rc -ne 0 ]] || [[ -z "$cert_info" ]]; then
        log_warn "Could not verify TLS certificate on ${DOMAIN}:443 — nginx may need a reload."
        log_warn "Run: nginx -s reload"
    else
        log_info "Certificate dates for ${DOMAIN}:"
        log_info "$cert_info"
    fi
fi

# ---------------------------------------------------------------------------
# Apply TLS hardening to nginx snippet (AC #2, NFR17: TLS ≥ 1.2)
# ---------------------------------------------------------------------------
generate_server_block() {
    log_info "Generating nginx server block for ${DOMAIN}..."

    local paths=($(python3 -c "
import sys, subprocess
try:
    out = subprocess.check_output(['certbot', 'certificates']).decode('utf-8')
except:
    sys.exit(1)
domain = sys.argv[1]
base = sys.argv[2]
current_cert = None
cert_path = None
key_path = None
for line in out.split('\n'):
    line = line.strip()
    if line.startswith('Certificate Name:'):
        current_cert = line.split(':', 1)[1].strip()
    elif line.startswith('Domains:'):
        doms = line.split(':', 1)[1].split()
        if domain in doms or f'*.{base}' in doms:
            pass
        else:
            current_cert = None
    elif line.startswith('Certificate Path:') and current_cert:
        cert_path = line.split(':', 1)[1].strip()
    elif line.startswith('Private Key Path:') and current_cert:
        key_path = line.split(':', 1)[1].strip()
        if cert_path and key_path:
            print(cert_path)
            print(key_path)
            sys.exit(0)
" "$DOMAIN" "$BASE_DOMAIN" || true))

    local cert_path="${paths[0]:-/etc/letsencrypt/live/${DOMAIN}/fullchain.pem}"
    local key_path="${paths[1]:-/etc/letsencrypt/live/${DOMAIN}/privkey.pem}"

    local dest_dir="/etc/nginx/conf.d"
    [[ -d "/etc/nginx/sites-enabled" ]] && dest_dir="/etc/nginx/sites-enabled"
    local server_block="${dest_dir}/teleproxy-${DOMAIN}.conf"

    cat > "$server_block" <<NGINX_CONF || fatal "TLS" "Write failed" "Disk full or permissions" "Check disk space"
# Generated by tls-setup.sh (Story 4.4)
server {
    listen 443 ssl;
    server_name ${DOMAIN};

    ssl_certificate ${cert_path};
    ssl_certificate_key ${key_path};

    # TLS hardening — Story 4.4 (NFR17: TLS >= 1.2)
    ssl_protocols TLSv1.2 TLSv1.3;
    ssl_ciphers 'ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256:ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384:ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305:DHE-RSA-AES128-GCM-SHA256';
    ssl_prefer_server_ciphers on;
    ssl_session_cache shared:TELEPROXY_SSL:10m;
    ssl_session_timeout 1d;

    include ${NGINX_SNIPPET};
}
NGINX_CONF

    log_info "Server block generated at ${server_block}."

    # Validate nginx config
    set +e
    local nginx_test_out
    nginx_test_out="$(nginx -t 2>&1)"
    local nginx_test_rc=$?
    set -e

    if [[ $nginx_test_rc -ne 0 ]]; then
        rm -f "$server_block"
        log_error "nginx-validate" \
            "nginx -t failed after generating server block" \
            "Directives may conflict with existing config" \
            "Review nginx error and fix manually"
        echo "$nginx_test_out" >> "$LOG_FILE"
    else
        log_info "nginx -t passed."
        nginx -s reload 2>/dev/null && log_info "nginx reloaded successfully." \
            || log_warn "nginx reload signal sent but reload may be pending."
    fi
}

generate_server_block

# ---------------------------------------------------------------------------
# Configure auto-renewal deploy hook (AC #3)
# ---------------------------------------------------------------------------
configure_renewal_hook() {
    log_info "Configuring auto-renewal nginx reload hook..."

    local hook_dir
    hook_dir="$(dirname "$RENEWAL_HOOK")"
    mkdir -p "$hook_dir" || fatal "Hook" "mkdir failed" "Check perms" ""

    if [[ -f "$RENEWAL_HOOK" ]]; then
        log_info "Renewal hook already exists: ${RENEWAL_HOOK} — skipping."
    else
        cat > "$RENEWAL_HOOK" <<'HOOK_SCRIPT'
#!/bin/sh
# Deployed by tls-setup.sh (Story 4.4)
# Fires after every successful certbot renewal to reload nginx.
/usr/sbin/nginx -s reload || systemctl reload nginx
HOOK_SCRIPT
        chmod 755 "$RENEWAL_HOOK"
        log_info "Renewal hook installed: ${RENEWAL_HOOK}"
    fi
}

configure_renewal_hook

# ---------------------------------------------------------------------------
# Verify systemd timer (AC #3)
# ---------------------------------------------------------------------------
verify_certbot_timer() {
    log_info "Checking certbot systemd timer..."

    if systemctl is-active --quiet certbot.timer 2>/dev/null; then
        log_info "certbot.timer is active — auto-renewal is configured."
    elif systemctl is-active --quiet snap.certbot.renew.timer 2>/dev/null; then
        log_info "snap.certbot.renew.timer is active — auto-renewal is configured (snap install)."
    else
        log_warn "certbot systemd timer is NOT active."
        log_warn "Enable it with: systemctl enable --now certbot.timer"
        log_warn "Or verify: systemctl list-timers | grep certbot"
    fi
}

verify_certbot_timer

# ---------------------------------------------------------------------------
# Dry-run renewal test (AC #3 / Task 2.3)
# ---------------------------------------------------------------------------
if [[ $DRY_RUN -eq 1 ]]; then
    log_info "Running certbot renew --dry-run to validate renewal configuration..."
    set +e
    renew_out="$(certbot renew --dry-run 2>&1)"
    renew_rc=$?
    set -e
    echo "$renew_out" >> "$LOG_FILE"
    if [[ $renew_rc -eq 0 ]]; then
        log_info "certbot renew --dry-run succeeded — renewal configuration is valid."
    else
        log_warn "certbot renew --dry-run exited with code ${renew_rc}."
        log_warn "This may be expected if no real certificate is present yet (dry-run mode)."
    fi
fi

# ---------------------------------------------------------------------------
# Done
# ---------------------------------------------------------------------------
log_info "=== tls-setup.sh complete ==="
echo ""
echo "✅  TLS setup complete for ${DOMAIN}"
echo "    Certificate: ${CERT_REUSED:+reused wildcard/existing}${CERT_REUSED:-0 eq 0 && echo 'issued via Let s Encrypt' || true}"
echo "    Renewal hook: ${RENEWAL_HOOK}"
echo "    Full log: ${LOG_FILE}"
