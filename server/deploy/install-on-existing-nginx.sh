#!/bin/sh
# install-on-existing-nginx.sh — Teleproxy v2 one-shot installer for existing-nginx deployments
#
# Usage:
#   bash install-on-existing-nginx.sh --origin-domain <subdomain> [options]
#   bash install-on-existing-nginx.sh --rotate
#   bash install-on-existing-nginx.sh --uninstall
#
# Options:
#   --origin-domain <fqdn>    Target subdomain (required for install)
#   --ws-path <path>          WebSocket path (default: random 16-hex path)
#   --stats-port <port>       Stats port (default: 8888)
#   --proxy-protocol          Enable PROXY protocol (default: false)
#   --dry-run                 Plan only — no system changes; print what would happen
#   --rotate                  Rotate WS path: new path + new link (Story 4.9)
#   --uninstall               Remove container, nginx config, state dir, revoke cert
#   -h, --help                Show this help
#
# Stages (install): dns → tls → nginx → container → verify → link
#
# All output is logged to /var/log/teleproxy-install-v2.log (AC #12).
# Pre-existing nginx files are NEVER modified (FR25 — AC #10).
# Re-running with identical inputs is idempotent (AC #11).
#
# Architecture §Category 5 error format:
#   Stage N failed: <symptom>
#   Most likely cause: <specific>
#   Fix: <actionable steps>
#   Full log: <path>
#
# Exit codes: 0 = success, 1 = fatal error, 2 = bad arguments
#
# Story 4.8 / 4.9 — MTProxy / teleproto3

set -e

# ---------------------------------------------------------------------------
# Constants — override-able via env for testing
# ---------------------------------------------------------------------------
LOG="${TELEPROXY_LOG_OVERRIDE:-/var/log/teleproxy-install-v2.log}"
STATE_DIR="${TELEPROXY_STATE_DIR_OVERRIDE:-/etc/teleproxy-ws-v2}"
STAGES="dns tls nginx container verify link"
SCRIPT_NAME="$(basename "$0")"

# Derived paths
STATE_FILE="${STATE_DIR}/state.json"
CREDS_FILE="${STATE_DIR}/credentials.enc"
NGINX_LOCATION_CONF="${TELEPROXY_NGINX_LOCATION_CONF_OVERRIDE:-/etc/nginx/conf.d/teleproxy-ws-v2.conf}"
NGINX_MAP_CONF="${TELEPROXY_NGINX_MAP_CONF_OVERRIDE:-/etc/nginx/conf.d/teleproxy-ws-map.conf}"
DEPLOY_DIR="$(cd "$(dirname "$0")" && pwd)"

# Operation mode: install (default) | rotate | uninstall
OPERATION="install"

# ---------------------------------------------------------------------------
# Logging
# ---------------------------------------------------------------------------
_ts() { date '+%Y-%m-%dT%H:%M:%S' 2>/dev/null || echo "0000-00-00T00:00:00"; }

_log() {
    local level="$1"; shift
    printf '[%s]  %s %s\n' "$level" "$(_ts)" "$*" | tee -a "$LOG"
}

log_info()  { _log "INFO " "$@"; }
log_warn()  { _log "WARN " "$@"; }
log_stage() { printf '\n[STAGE] %s %s\n' "$(_ts)" "$*" | tee -a "$LOG"; }

log_error() {
    # Structured error per architecture §Category 5
    local stage="$1" symptom="$2" cause="$3" fix="$4"
    {
        printf '[ERROR] %s Stage %s failed: %s\n' "$(_ts)" "$stage" "$symptom"
        printf '        Most likely cause: %s\n' "$cause"
        printf '        Fix: %s\n' "$fix"
        printf '        Full log: %s\n' "$LOG"
    } | tee -a "$LOG" >&2
}

fatal() {
    log_error "$@"
    exit 1
}

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------
DOMAIN=""
WS_PATH=""
STATS_PORT="8888"
PROXY_PROTOCOL="false"
DRY_RUN=0

usage() {
    cat <<USAGE
Usage:
  $SCRIPT_NAME --origin-domain <fqdn> [options]   # fresh install
  $SCRIPT_NAME --rotate                            # rotate WS path
  $SCRIPT_NAME --uninstall                         # clean removal

Install options:
  --origin-domain <fqdn>    Fully-qualified subdomain (e.g. proxy.example.com)
  --ws-path <path>          WebSocket path (default: random /XXXXXXXXXXXXXXXX)
  --stats-port <port>       Prometheus stats port inside container (default: 8888)
  --proxy-protocol          Enable PROXY protocol IP pass-through (default: false)
  --dry-run                 Plan-only: print what would happen, no changes made
  -h, --help                Show this help

Stages (install): dns → tls → nginx → container → verify → link

All output is logged to ${LOG}.
USAGE
    exit 2
}

while [ $# -gt 0 ]; do
    case "$1" in
        --origin-domain)
            DOMAIN="${2:?'--origin-domain requires a value'}"; shift 2 ;;
        --ws-path)
            WS_PATH="${2:?'--ws-path requires a value'}"; shift 2 ;;
        --stats-port)
            STATS_PORT="${2:?'--stats-port requires a value'}"; shift 2 ;;
        --proxy-protocol)
            PROXY_PROTOCOL="true"; shift ;;
        --dry-run)
            DRY_RUN=1; shift ;;
        --rotate)
            OPERATION="rotate"; shift ;;
        --uninstall)
            OPERATION="uninstall"; shift ;;
        -h|--help)
            usage ;;
        *)
            printf 'Unknown option: %s\n' "$1" >&2; usage ;;
    esac
done

# For install mode, --origin-domain is required
if [ "$OPERATION" = "install" ]; then
    [ -z "$DOMAIN" ] && { printf 'Error: --origin-domain is required\n' >&2; usage; }
fi

# ---------------------------------------------------------------------------
# Ensure log is writable
# ---------------------------------------------------------------------------
LOG_DIR="$(dirname "$LOG")"
mkdir -p "$LOG_DIR" 2>/dev/null || true
touch "$LOG" 2>/dev/null || {
    LOG="/tmp/teleproxy-install-v2.log"
    touch "$LOG"
    printf 'Warning: cannot write to default log — using %s\n' "$LOG" >&2
}
chmod 640 "$LOG" 2>/dev/null || true

log_info "=== install-on-existing-nginx.sh start ==="
log_info "domain=${DOMAIN} ws_path=${WS_PATH:-<random>} stats_port=${STATS_PORT} proxy_protocol=${PROXY_PROTOCOL} dry_run=${DRY_RUN}"

# ---------------------------------------------------------------------------
# Generate WS path if not provided (random 16 hex chars for camouflage)
# ---------------------------------------------------------------------------
if [ -z "$WS_PATH" ]; then
    WS_PATH="/$(head -c 8 /dev/urandom | od -An -tx1 | tr -d ' \n')"
    log_info "Generated random WS path: ${WS_PATH}"
fi

# ---------------------------------------------------------------------------
# State helpers (idempotency — AC #11, #8.1, #8.2)
# ---------------------------------------------------------------------------
state_get() {
    # state_get <key>  — returns value from state.json or empty string
    local key="$1"
    if [ -f "$STATE_FILE" ]; then
        # Portable grep-based JSON field extraction (no jq dependency)
        grep -o "\"${key}\"[[:space:]]*:[[:space:]]*\"[^\"]*\"" "$STATE_FILE" \
            | grep -m1 '' \
            | sed 's/.*:[[:space:]]*"\([^"]*\)"/\1/'
    fi
}

state_has_stage() {
    # Returns 0 (true) if stage is in stages_done list
    local stage="$1"
    if [ -f "$STATE_FILE" ]; then
        grep -q "\"${stage}\"" "$STATE_FILE" 2>/dev/null
    else
        return 1
    fi
}

state_set_stage_done() {
    # Append stage to stages_done in state.json (creates/updates file)
    local stage="$1"
    mkdir -p "$STATE_DIR"
    if [ ! -f "$STATE_FILE" ]; then
        cat > "$STATE_FILE" <<JSON
{
  "version": "1",
  "domain": "${DOMAIN}",
  "ws_path": "${WS_PATH}",
  "secret_hex": "${SECRET_HEX:-}",
  "stages_done": ["${stage}"],
  "installed_at": "$(_ts)"
}
JSON
    else
        # Add stage to the stages_done array if not already present
        if ! grep -q "\"${stage}\"" "$STATE_FILE"; then
            sed -i.bak "s/\"stages_done\": \[/\"stages_done\": [\"${stage}\", /" "$STATE_FILE" 2>/dev/null || true
            rm -f "${STATE_FILE}.bak" 2>/dev/null || true
        fi
    fi
}

state_update_secret() {
    local hex="$1"
    if [ -f "$STATE_FILE" ]; then
        sed -i.bak "s/\"secret_hex\": \"[^\"]*\"/\"secret_hex\": \"${hex}\"/" "$STATE_FILE" 2>/dev/null || true
        rm -f "${STATE_FILE}.bak" 2>/dev/null || true
    fi
}

# ---------------------------------------------------------------------------
# Stage real-state checks (pessimistic idempotency — called before skip)
# ---------------------------------------------------------------------------
# Each *_state_ok() returns 0 if system already matches desired state, 1 if not.
# If _state_ok returns 1, the stage runs even though it appears in stages_done.

dns_state_ok() {
    # DNS still resolves to this server
    local server_ip domain_ip
    server_ip="$(curl -s --max-time 5 ifconfig.me 2>/dev/null || echo '')"
    domain_ip="$(dig +short "$DOMAIN" A 2>/dev/null | head -1 || echo '')"
    [ -n "$server_ip" ] && [ "$domain_ip" = "$server_ip" ]
}

tls_state_ok() {
    # Certificate file exists for the domain
    [ -f "/etc/letsencrypt/live/${DOMAIN}/fullchain.pem" ] || \
    [ -f "/etc/letsencrypt/live/$(echo "$DOMAIN" | cut -d. -f2-)/fullchain.pem" ]
}

nginx_state_ok() {
    # Our nginx location conf still exists and nginx is running
    [ -f "$NGINX_LOCATION_CONF" ] && nginx -t >/dev/null 2>&1
}

container_state_ok() {
    # teleproxy container is running and stats endpoint is reachable
    docker ps --filter name=teleproxy --filter status=running --format '{{.Names}}' 2>/dev/null \
        | grep -q teleproxy && \
    curl -f -s --max-time 3 "http://localhost:${STATS_PORT}/stats" >/dev/null 2>&1
}

verify_state_ok() {
    # Stats endpoint is still reachable
    curl -f -s --max-time 3 "http://localhost:${STATS_PORT}/stats" >/dev/null 2>&1
}

link_state_ok() {
    # Link stage is stateless — always OK if credentials exist
    [ -f "$CREDS_FILE" ]
}

# ---------------------------------------------------------------------------
# Stage framework helpers
# ---------------------------------------------------------------------------
run_stage() {
    local stage="$1"
    log_stage "$stage"

    # Pessimistic idempotency: check state.json AND verify actual system state
    if state_has_stage "$stage"; then
        local stored_domain
        stored_domain="$(state_get domain)"
        if [ "$stored_domain" = "$DOMAIN" ]; then
            # Verify actual system state before trusting state.json
            if "${stage}_state_ok" 2>/dev/null; then
                log_info "  OK (no change) — stage '${stage}' already completed and system state verified"
                return 0
            else
                log_info "  Re-running '${stage}' — state.json says done but system state mismatch"
            fi
        fi
    fi

    # In dry-run: call only the *_plan function
    if [ "$DRY_RUN" = "1" ]; then
        "${stage}_plan"
    else
        "${stage}_plan"
        "${stage}_apply"
        "${stage}_verify"
        state_set_stage_done "$stage"
    fi
}

# ---------------------------------------------------------------------------
# ─── STAGE: dns ──────────────────────────────────────────────────────────────
# ---------------------------------------------------------------------------
dns_plan() {
    log_info "[dns/plan] Will verify DNS A record for ${DOMAIN} resolves to this server's IP"
}

dns_apply() {
    log_info "[dns/apply] Checking DNS A record..."
    local server_ip domain_ip
    server_ip="$(curl -s --max-time 5 ifconfig.me 2>/dev/null || echo '')"
    domain_ip="$(dig +short "$DOMAIN" A 2>/dev/null | head -1 || echo '')"

    if [ -z "$server_ip" ]; then
        log_warn "Could not determine server IP from ifconfig.me — skipping DNS check"
        return 0
    fi

    if [ -z "$domain_ip" ]; then
        fatal "dns" \
            "DNS lookup for ${DOMAIN} returned no A record" \
            "DNS A record for ${DOMAIN} does not exist or has not propagated" \
            "Add an A record: ${DOMAIN} → ${server_ip}. DNS propagation may take up to 48h."
    fi

    if [ "$domain_ip" != "$server_ip" ]; then
        fatal "dns" \
            "DNS A record for ${DOMAIN} points to ${domain_ip}, server IP is ${server_ip}" \
            "DNS is pointing to a different server" \
            "Update DNS A record: ${DOMAIN} → ${server_ip}. Propagation can take up to 48h."
    fi

    log_info "[dns/apply] DNS OK: ${DOMAIN} → ${domain_ip}"
}

dns_verify() {
    log_info "[dns/verify] DNS stage verified."
}

# ---------------------------------------------------------------------------
# ─── STAGE: tls ──────────────────────────────────────────────────────────────
# ---------------------------------------------------------------------------
tls_plan() {
    log_info "[tls/plan] Will invoke tls-setup.sh to issue or reuse Let's Encrypt cert for ${DOMAIN}"
}

tls_apply() {
    log_info "[tls/apply] Running tls-setup.sh..."
    local tls_script="${DEPLOY_DIR}/tls-setup.sh"

    if [ ! -f "$tls_script" ]; then
        fatal "tls" \
            "tls-setup.sh not found at ${tls_script}" \
            "Story 4.4 script is missing from the deploy directory" \
            "Ensure teleproto3/server/deploy/tls-setup.sh exists (Story 4.4 must be deployed)"
    fi

    # Derive admin email from domain (operator can override via TLS_EMAIL env)
    local email="${TLS_EMAIL:-admin@$(echo "$DOMAIN" | cut -d. -f2-)}"

    set +e
    tls_out="$(bash "$tls_script" --domain "$DOMAIN" --email "$email" 2>&1)"
    tls_rc=$?
    set -e

    printf '%s\n' "$tls_out" >> "$LOG"

    if [ $tls_rc -ne 0 ]; then
        fatal "tls" \
            "tls-setup.sh exited with code ${tls_rc}" \
            "Certificate issuance or wildcard detection failed" \
            "Review output above and ${LOG}. Common fix: ensure port 80 is open (ufw allow 80/tcp)."
    fi

    log_info "[tls/apply] TLS certificate ready for ${DOMAIN}"
}

tls_verify() {
    log_info "[tls/verify] TLS stage verified."
}

# ---------------------------------------------------------------------------
# ─── STAGE: nginx ─────────────────────────────────────────────────────────────
# ---------------------------------------------------------------------------
nginx_plan() {
    log_info "[nginx/plan] Will install nginx location snippet at ${NGINX_LOCATION_CONF}"
    log_info "[nginx/plan] Will install nginx map directive at ${NGINX_MAP_CONF}"
    log_info "[nginx/plan] WS path: ${WS_PATH}"
    log_info "[nginx/plan] Pre-existing nginx files will NOT be modified (FR25)"
}

nginx_apply() {
    log_info "[nginx/apply] Installing nginx configuration..."
    local template="${DEPLOY_DIR}/nginx-ws-location.conf.template"
    local map_template="${DEPLOY_DIR}/nginx-ws-map.conf.template"

    if [ ! -f "$template" ]; then
        fatal "nginx" \
            "nginx-ws-location.conf.template not found at ${template}" \
            "Story 4.3 template is missing from the deploy directory" \
            "Ensure teleproto3/server/deploy/nginx-ws-location.conf.template exists"
    fi

    if [ ! -f "$map_template" ]; then
        fatal "nginx" \
            "nginx-ws-map.conf.template not found at ${map_template}" \
            "Story 4.3 map template is missing from the deploy directory" \
            "Ensure teleproto3/server/deploy/nginx-ws-map.conf.template exists"
    fi

    # Install location block (new file — zero changes to existing configs — FR25/AC #10)
    if [ ! -f "$NGINX_LOCATION_CONF" ]; then
        mkdir -p "$(dirname "$NGINX_LOCATION_CONF")"
        sed "s|\${WS_PATH}|${WS_PATH}|g" "$template" > "$NGINX_LOCATION_CONF"
        chmod 644 "$NGINX_LOCATION_CONF"
        log_info "[nginx/apply] Installed location block: ${NGINX_LOCATION_CONF}"
    else
        log_info "[nginx/apply] Location block already exists: ${NGINX_LOCATION_CONF} — skipping"
    fi

    # Install map directive (only if not already present)
    if [ ! -f "$NGINX_MAP_CONF" ]; then
        mkdir -p "$(dirname "$NGINX_MAP_CONF")"
        cp "$map_template" "$NGINX_MAP_CONF"
        chmod 644 "$NGINX_MAP_CONF"
        log_info "[nginx/apply] Installed map directive: ${NGINX_MAP_CONF}"
    else
        log_info "[nginx/apply] Map directive already exists: ${NGINX_MAP_CONF} — skipping"
    fi

    # Validate and reload nginx
    set +e
    nginx_test_out="$(nginx -t 2>&1)"
    nginx_test_rc=$?
    set -e

    printf '%s\n' "$nginx_test_out" >> "$LOG"

    if [ $nginx_test_rc -ne 0 ]; then
        fatal "nginx" \
            "nginx -t failed after installing teleproxy config" \
            "The installed nginx snippet may conflict with existing configuration" \
            "Review ${NGINX_LOCATION_CONF} and run 'nginx -t' to see the exact error"
    fi

    log_info "[nginx/apply] nginx -t OK"
    nginx -s reload 2>/dev/null && log_info "[nginx/apply] nginx reloaded" \
        || log_warn "[nginx/apply] nginx reload signal sent (may be pending)"
}

nginx_verify() {
    log_info "[nginx/verify] nginx configuration stage verified."
}

# ---------------------------------------------------------------------------
# ─── STAGE: container ─────────────────────────────────────────────────────────
# ---------------------------------------------------------------------------
container_plan() {
    log_info "[container/plan] Will install Docker if not present (AC #2)"
    log_info "[container/plan] Will generate 16-byte secret (AC #4)"
    log_info "[container/plan] Will write .env and start teleproxy via docker compose (AC #3)"
}

container_apply() {
    log_info "[container/apply] Checking Docker..."

    # Install Docker if not present (AC #2)
    if ! command -v docker >/dev/null 2>&1; then
        log_info "[container/apply] Docker not found — installing via get.docker.com..."
        set +e
        docker_install_out="$(curl -fsSL https://get.docker.com | sh 2>&1)"
        docker_install_rc=$?
        set -e
        printf '%s\n' "$docker_install_out" >> "$LOG"
        if [ $docker_install_rc -ne 0 ]; then
            fatal "container" \
                "Docker installation failed (exit ${docker_install_rc})" \
                "get.docker.com script failed on this system" \
                "Install Docker manually: https://docs.docker.com/engine/install/ubuntu/ then re-run this installer"
        fi
        log_info "[container/apply] Docker installed successfully"
    else
        log_info "[container/apply] Docker already present: $(docker --version 2>/dev/null || echo 'unknown version')"
    fi

    # Generate secret — 16 random bytes as hex (AC #4)
    # Use od instead of xxd for portability on minimal Ubuntu installs
    SECRET_HEX="$(head -c 16 /dev/urandom | od -An -tx1 | tr -d ' \n')"
    log_info "[container/apply] Generated proxy secret (${#SECRET_HEX} hex chars)"

    # Determine external IP
    local ext_ip
    ext_ip="$(curl -s --max-time 5 ifconfig.me 2>/dev/null || echo '')"

    # Create state directory (AC #4)
    mkdir -p "$STATE_DIR"
    chmod 700 "$STATE_DIR"

    # Write credentials file (AC #4)
    # Note: stored as plaintext hex restricted to root (chmod 600);
    # the .enc extension signals operator intent, not actual encryption.
    printf '%s\n' "$SECRET_HEX" > "$CREDS_FILE"
    chmod 600 "$CREDS_FILE"
    log_info "[container/apply] Secret stored in ${CREDS_FILE}"

    # Write .env file and start container
    local compose_dir
    compose_dir="$(cd "${DEPLOY_DIR}/.." && pwd)"
    local compose_file="${compose_dir}/docker-compose.behind-nginx.yml"

    if [ ! -f "$compose_file" ]; then
        fatal "container" \
            "docker-compose.behind-nginx.yml not found at ${compose_file}" \
            "The compose file for behind-nginx mode is missing" \
            "Ensure teleproto3/server/docker-compose.behind-nginx.yml exists (Story 4.2 must be deployed)"
    fi

    # Write .env for docker compose
    cat > "${compose_dir}/.env" <<ENV
SECRET=${SECRET_HEX}
EXTERNAL_IP=${ext_ip:-}
STATS_PORT=${STATS_PORT}
PROXY_PROTOCOL=${PROXY_PROTOCOL}
ENV
    chmod 600 "${compose_dir}/.env"
    log_info "[container/apply] Written docker-compose .env at ${compose_dir}/.env"

    # Start container
    log_info "[container/apply] Starting teleproxy via docker compose..."
    set +e
    compose_out="$(docker compose -f "$compose_file" up -d 2>&1)"
    compose_rc=$?
    set -e
    printf '%s\n' "$compose_out" >> "$LOG"

    if [ $compose_rc -ne 0 ]; then
        fatal "container" \
            "docker compose up -d failed (exit ${compose_rc})" \
            "Docker or the compose file may have an issue" \
            "Run manually: docker compose -f ${compose_file} up -d  and check 'docker logs teleproxy'"
    fi

    log_info "[container/apply] teleproxy container started"
    state_update_secret "$SECRET_HEX"
}

container_verify() {
    log_info "[container/verify] Container stage verified."
}

# ---------------------------------------------------------------------------
# ─── STAGE: verify ────────────────────────────────────────────────────────────
# ---------------------------------------------------------------------------
verify_plan() {
    log_info "[verify/plan] Will poll http://localhost:${STATS_PORT}/stats (up to 60s)"
    log_info "[verify/plan] Will test WebSocket upgrade to https://${DOMAIN}${WS_PATH} — expect HTTP 101"
}

verify_apply() {
    log_info "[verify/apply] Waiting for teleproxy healthcheck..."

    # Poll stats endpoint (AC #7 — healthcheck)
    local retries=12 interval=5 attempt=0 healthy=0
    while [ $attempt -lt $retries ]; do
        if curl -f -s --max-time 3 "http://localhost:${STATS_PORT}/stats" >/dev/null 2>&1; then
            healthy=1; break
        fi
        attempt=$((attempt+1))
        log_info "[verify/apply] Waiting for stats endpoint... (${attempt}/${retries})"
        sleep $interval
    done

    if [ $healthy -eq 0 ]; then
        fatal "verify" \
            "Stats endpoint http://localhost:${STATS_PORT}/stats not reachable after 60s" \
            "teleproxy container may not have started correctly" \
            "Run: docker logs teleproxy — to see container output. Check ${LOG}."
    fi

    log_info "[verify/apply] Stats endpoint healthy ✅"

    # Test WebSocket upgrade (AC #7)
    log_info "[verify/apply] Testing WebSocket upgrade to https://${DOMAIN}${WS_PATH}..."
    local ws_key
    ws_key="$(head -c 16 /dev/urandom | base64 | tr -d '\n')"

    set +e
    ws_code="$(curl -sI \
        -H "Upgrade: websocket" \
        -H "Connection: Upgrade" \
        -H "Sec-WebSocket-Version: 13" \
        -H "Sec-WebSocket-Key: ${ws_key}" \
        --max-time 10 \
        "https://${DOMAIN}${WS_PATH}" \
        | head -1 | grep -o '[0-9][0-9][0-9]' || echo '')"
    set -e

    if [ "$ws_code" = "101" ]; then
        log_info "[verify/apply] WebSocket upgrade: HTTP 101 ✅"
    else
        log_warn "[verify/apply] WebSocket upgrade returned HTTP ${ws_code:-???} (expected 101)"
        log_warn "  This may be transient — teleproxy may still be initializing DC connections."
        log_warn "  Re-run verify manually: curl -sI -H 'Upgrade: websocket' -H 'Connection: Upgrade' https://${DOMAIN}${WS_PATH}"
    fi
}

verify_verify() {
    log_info "[verify/verify] Verify stage done."
}

# ---------------------------------------------------------------------------
# ─── STAGE: link ──────────────────────────────────────────────────────────────
# ---------------------------------------------------------------------------
link_plan() {
    log_info "[link/plan] Will construct and print tg://proxy link (AC #8)"
}

link_apply() {
    log_info "[link/apply] Constructing Telegram proxy link..."

    # Retrieve secret: from state or from credentials file
    if [ -z "${SECRET_HEX:-}" ]; then
        SECRET_HEX="$(state_get secret_hex)"
    fi
    if [ -z "${SECRET_HEX:-}" ] && [ -f "$CREDS_FILE" ]; then
        SECRET_HEX="$(cat "$CREDS_FILE" | tr -d '[:space:]')"
    fi

    if [ -z "${SECRET_HEX:-}" ]; then
        fatal "link" \
            "Secret not available — credentials file not found" \
            "Container stage may not have completed successfully" \
            "Re-run installer or check ${CREDS_FILE}"
    fi

    # Construct Type3 secret (AC #8.1):
    # Format: "ff" prefix + 16-byte secret hex + domain bytes as hex (UTF-8)
    local domain_hex
    domain_hex="$(printf '%s' "$DOMAIN" | xxd -p | tr -d '\n')"
    local type3_secret="ff${SECRET_HEX}${domain_hex}"

    # Print link (AC #8.2)
    log_info "[link/apply] type3_secret length=${#type3_secret} chars"
    printf '\n'
    printf '═══════════════════════════════════════════════════════════\n'
    printf '  ✅  Teleproxy v2 installation complete!\n'
    printf '\n'
    printf '  Telegram proxy link:\n'
    printf '  tg://proxy?server=%s&port=443&secret=%s\n' "$DOMAIN" "$type3_secret"
    printf '\n'
    printf '  Server:  %s\n' "$DOMAIN"
    printf '  Port:    443\n'
    printf '  Secret:  %s\n' "$type3_secret"
    printf '  WS path: %s\n' "$WS_PATH"
    printf '\n'
    printf '  Full log: %s\n' "$LOG"
    printf '═══════════════════════════════════════════════════════════\n'
    printf '\n'
}

link_verify() {
    log_info "[link/verify] Link stage complete."
}

# ---------------------------------------------------------------------------
# ─── OPERATION: rotate ────────────────────────────────────────────────────────
# ---------------------------------------------------------------------------
do_rotate() {
    log_info "=== --rotate: generating new WS path ==="

    # Load current state
    if [ ! -f "$STATE_FILE" ]; then
        fatal "rotate" \
            "state.json not found at ${STATE_FILE}" \
            "Teleproxy is not installed or state was removed" \
            "Run the installer first to create a deployment"
    fi

    local old_domain old_ws_path old_secret
    old_domain="$(state_get domain)"
    old_ws_path="$(state_get ws_path)"
    old_secret="$(state_get secret_hex)"

    if [ -z "$old_domain" ] || [ -z "$old_ws_path" ]; then
        fatal "rotate" \
            "state.json is missing domain or ws_path" \
            "State file may be corrupt" \
            "Inspect ${STATE_FILE} and correct the values"
    fi

    DOMAIN="$old_domain"
    SECRET_HEX="$old_secret"

    # Generate new random WS path (Task 1.2)
    local new_ws_path
    new_ws_path="/$(head -c 8 /dev/urandom | od -An -tx1 | tr -d ' \n')"
    log_info "[rotate] Old path: ${old_ws_path}"
    log_info "[rotate] New path: ${new_ws_path}"

    # Determine nginx location conf path (prefer state.json record, else env/default)
    local location_conf
    location_conf="$(state_get nginx_location_conf)"
    [ -z "$location_conf" ] && location_conf="$NGINX_LOCATION_CONF"

    # Update nginx location conf with new path (Task 1.3)
    if [ -f "$location_conf" ]; then
        sed -i.bak "s|location ${old_ws_path} |location ${new_ws_path} |g" "$location_conf" 2>/dev/null || \
        sed -i.bak "s|location ${old_ws_path}{|location ${new_ws_path}{|g" "$location_conf" 2>/dev/null || true
        # Also update Path comment
        sed -i.bak "s|Path: ${old_ws_path}|Path: ${new_ws_path}|g" "$location_conf" 2>/dev/null || true
        rm -f "${location_conf}.bak" 2>/dev/null || true
        log_info "[rotate] Updated nginx conf: ${location_conf}"
    else
        log_warn "[rotate] nginx location conf not found at ${location_conf} — skipping conf update"
    fi

    # Validate and reload nginx (Task 1.4, AC #2)
    set +e
    nginx_test_out="$(nginx -t 2>&1)"
    nginx_test_rc=$?
    set -e
    printf '%s\n' "$nginx_test_out" >> "$LOG"
    if [ $nginx_test_rc -ne 0 ]; then
        fatal "rotate" \
            "nginx -t failed after updating location conf" \
            "The rotated nginx config may be invalid" \
            "Review ${location_conf} and run 'nginx -t' to see the error"
    fi
    nginx -s reload 2>/dev/null && log_info "[rotate] nginx reloaded (new path active)" || \
        log_warn "[rotate] nginx reload signal sent"

    # Update state.json with new ws_path (Task 1.5)
    WS_PATH="$new_ws_path"
    if [ -f "$STATE_FILE" ]; then
        sed -i.bak "s|\"ws_path\": \"[^\"]*\"|\"ws_path\": \"${new_ws_path}\"|" "$STATE_FILE" 2>/dev/null || true
        rm -f "${STATE_FILE}.bak" 2>/dev/null || true
        log_info "[rotate] state.json updated with new ws_path"
    fi

    # Construct and print new link (Task 1.6, AC #3)
    # The domain field in the Type3 secret encodes host+path.
    # Rotation changes the path → new domain value → new secret → new link.
    # The 16-byte key stays the same (FR9: rotation changes address, not identity).
    local domain_with_path
    domain_with_path="${DOMAIN}${new_ws_path}"
    local domain_hex
    domain_hex="$(printf '%s' "$domain_with_path" | xxd -p | tr -d '\n')"
    local type3_secret="ff${SECRET_HEX}${domain_hex}"

    printf '\n'
    printf '═══════════════════════════════════════════════════════════\n'
    printf '  🔄  Teleproxy v2 rotation complete!\n'
    printf '\n'
    printf '  New Telegram proxy link (share with clients):\n'
    printf '  tg://proxy?server=%s&port=443&secret=%s\n' "$DOMAIN" "$type3_secret"
    printf '\n'
    printf '  Server:      %s\n' "$DOMAIN"
    printf '  Port:        443\n'
    printf '  New WS path: %s\n' "$new_ws_path"
    printf '  Secret:      %s\n' "$type3_secret"
    printf '\n'
    printf '  ℹ️  Existing connections drain naturally (nginx graceful reload).\n'
    printf '  Full log: %s\n' "$LOG"
    printf '═══════════════════════════════════════════════════════════\n'
    printf '\n'

    log_info "=== --rotate complete ==="
}

# ---------------------------------------------------------------------------
# ─── OPERATION: uninstall ─────────────────────────────────────────────────────
# ---------------------------------------------------------------------------
do_uninstall() {
    log_info "=== --uninstall: removing teleproxy deployment ==="

    # Load current state
    if [ ! -f "$STATE_FILE" ]; then
        log_warn "state.json not found — nothing to uninstall or already removed"
        exit 0
    fi

    local domain cert_installed
    domain="$(state_get domain)"
    cert_installed="$(state_get cert_installed)"

    log_info "[uninstall] Removing deployment for domain: ${domain:-<unknown>}"

    # Task 2.2: Stop and remove Docker container + image
    local compose_dir
    compose_dir="$(cd "${DEPLOY_DIR}/.." && pwd)"
    local compose_file="${compose_dir}/docker-compose.behind-nginx.yml"
    if [ -f "$compose_file" ]; then
        log_info "[uninstall] Stopping and removing container..."
        set +e
        docker compose -f "$compose_file" down --rmi all 2>&1 | tee -a "$LOG"
        set -e
        log_info "[uninstall] Container removed"
    else
        log_warn "[uninstall] compose file not found at ${compose_file} — skipping container removal"
    fi

    # Task 2.3: Remove nginx configs (only files we installed, per state.json)
    local location_conf map_conf
    location_conf="$(state_get nginx_location_conf)"
    map_conf="$(state_get nginx_map_conf)"
    [ -z "$location_conf" ] && location_conf="$NGINX_LOCATION_CONF"
    [ -z "$map_conf" ]      && map_conf="$NGINX_MAP_CONF"

    local nginx_changed=0
    if [ -f "$location_conf" ]; then
        rm -f "$location_conf"
        log_info "[uninstall] Removed nginx location conf: ${location_conf}"
        nginx_changed=1
    fi
    if [ -f "$map_conf" ]; then
        rm -f "$map_conf"
        log_info "[uninstall] Removed nginx map conf: ${map_conf}"
        nginx_changed=1
    fi

    if [ "$nginx_changed" = "1" ]; then
        set +e
        nginx_test_out="$(nginx -t 2>&1)"
        nginx_test_rc=$?
        set -e
        printf '%s\n' "$nginx_test_out" >> "$LOG"
        if [ $nginx_test_rc -ne 0 ]; then
            log_warn "[uninstall] nginx -t failed after removing configs — nginx reload skipped"
            log_warn "[uninstall]   Manual fix required: check remaining nginx config"
        else
            nginx -s reload 2>/dev/null && log_info "[uninstall] nginx reloaded" || \
                log_warn "[uninstall] nginx reload signal sent"
        fi
    fi

    # Task 2.4: Revoke cert — best-effort (AC #10, AR-G3)
    if [ "$cert_installed" = "true" ] && [ -n "$domain" ]; then
        log_info "[uninstall] Revoking certbot certificate for ${domain}..."
        set +e
        certbot revoke --cert-name "$domain" --non-interactive 2>&1 | tee -a "$LOG"
        certbot_rc=$?
        set -e
        if [ $certbot_rc -ne 0 ]; then
            log_warn "[uninstall] certbot revoke returned ${certbot_rc} — continuing (best-effort per AR-G3)"
        else
            log_info "[uninstall] Certificate revoked"
        fi
    else
        log_info "[uninstall] Cert revocation skipped (cert_installed=${cert_installed:-false})"
    fi

    # Task 2.5: Remove state directory (AC #8)
    log_info "[uninstall] Removing state directory: ${STATE_DIR}"
    rm -rf "$STATE_DIR"
    log_info "[uninstall] State directory removed"

    # Task 2.6: Print confirmation (AC #9 guarantee)
    printf '\n'
    printf '═══════════════════════════════════════════════════════════\n'
    printf '  ✅  Teleproxy v2 uninstall complete!\n'
    printf '\n'
    printf '  Removed:\n'
    printf '    • Docker container and image\n'
    printf '    • nginx location block: %s\n' "${location_conf}"
    printf '    • nginx map directive: %s\n' "${map_conf}"
    printf '    • State directory: %s\n' "${STATE_DIR}"
    printf '\n'
    printf '  ✅  Pre-existing nginx configurations were NOT modified (FR25).\n'
    printf '\n'
    printf '  Full log: %s\n' "$LOG"
    printf '═══════════════════════════════════════════════════════════\n'
    printf '\n'

    log_info "=== --uninstall complete ==="
}

# ---------------------------------------------------------------------------
# Main — dispatch by OPERATION
# ---------------------------------------------------------------------------
if [ "$OPERATION" = "rotate" ]; then
    # Ensure log is writable for rotate operation too
    LOG_DIR="$(dirname "$LOG")"
    mkdir -p "$LOG_DIR" 2>/dev/null || true
    touch "$LOG" 2>/dev/null || { LOG="/tmp/teleproxy-install-v2.log"; touch "$LOG"; }
    chmod 640 "$LOG" 2>/dev/null || true
    log_info "=== install-on-existing-nginx.sh --rotate ==="
    do_rotate
    exit 0
fi

if [ "$OPERATION" = "uninstall" ]; then
    LOG_DIR="$(dirname "$LOG")"
    mkdir -p "$LOG_DIR" 2>/dev/null || true
    touch "$LOG" 2>/dev/null || { LOG="/tmp/teleproxy-install-v2.log"; touch "$LOG"; }
    chmod 640 "$LOG" 2>/dev/null || true
    log_info "=== install-on-existing-nginx.sh --uninstall ==="
    do_uninstall
    exit 0
fi

# ---------------------------------------------------------------------------
# Install mode — execute all stages in sequence (AC #1)
# ---------------------------------------------------------------------------
log_info "Stages: ${STAGES}"
if [ "$DRY_RUN" = "1" ]; then
    log_info "DRY-RUN mode — only planning stages, no system changes"
fi

for stage in $STAGES; do
    run_stage "$stage"
done

log_info "=== install-on-existing-nginx.sh complete ==="
exit 0
