---
doc_version: 0.1.0-draft
last_updated: 2026-05-03
status: draft
---

> **Normative.** This document defines required behaviour for conforming
> Type3 implementations. Code in `lib/` is illustrative; where this
> document and `lib/` differ, this document wins.

# Scenario: <!-- ban-list-doc: technical identifier -->PROXY-Protocol<!-- /ban-list-doc --> End-to-End Test

**Compliance level:** extended
**Owner:** Epic 2 story 2.10
**Status:** draft

## Classification

This scenario sits at compliance level `extended` because the
<!-- ban-list-doc: technical identifier -->PROXY-protocol<!-- /ban-list-doc --> v1/v2 path is
operator-deployment-shape-specific: it is operative only when a CDN or load
balancer is in front of the teleproxy server (per Epic 1 §4 and Epic 2 §11
ownership table). An IUT that does not operate behind a CDN MAY skip this
scenario and still claim `full` compliance, provided it declares the
limitation in its conformance report.

Canonical path: `teleproto3/conformance/scenarios/extended/proxy-protocol-e2e.md`
(single path, no symlinks, no mirrors — Epic 2 §11).

## Purpose

Validate that when teleproxy runs behind a load-balancer that injects a
<!-- ban-list-doc: technical identifier -->PROXY-protocol<!-- /ban-list-doc --> v1
(text) or v2 (binary) header, the server:

1. Parses the header and populates the connection's true source IP.
2. Feeds the true source IP to per-IP rate-limit logic and to Prometheus counters
   (`proxy_protocol_connections_total`).
3. Does **not** emit the client IP in default-verbosity server logs (NFR19).
4. Handles both v1 (text `PROXY TCP4 …`) and v2 (binary `\r\n\r\n\0\r\nQUIT\n`)
   header formats (FR46).
5. Falls back to peer-IP gracefully when no header is present (`--no-cdn` shape).

## Prerequisite state

| Symbol | Source | Status |
|--------|--------|--------|
| <!-- ban-list-doc: technical identifier -->PROXY-protocol<!-- /ban-list-doc --> v1/v2 parser (`proxy_protocol_parse`) | upstream commit `ebbc4f3` via `teleproto3/server/` subtree | **Present** in current subtree |
| `proxy_protocol_connections_total` C global | `net-tcp-rpc-ext-server.c` + `net-proxy-protocol.h` | **Present**; exported to `/metrics` by story 2.10 extension to `net-type3-stats.c` |
| `proxy_protocol_errors_total` C global | same | **Present**; same extension |
| Per-IP Prometheus top-N counter | upstream commit `f083b07` | **Not yet in subtree** — forward-citation: story 1.8/1.9 subtree-pull cron |
| Per-IP rate-limit Prometheus label | upstream commit `be46337` | **Not yet in subtree** — same forward-citation |

Per-IP labelled counters (`te_per_ip_top_n`, `te_rate_limit_bucket_count`) are
placeholders pending full subtree pull of `be46337` and `f083b07`. When those
commits land, amend §Expected-counter-values with the verified label names.

## §Reproduction-steps

Steps reproduce the full CF Worker → nginx → teleproxy chain.
CI sandbox uses port 13129 (not 3129, which is occupied by the v1 mtproxy on
the production prototype at `94.156.131.252`). Production-acceptance run uses
port 3129 with separately managed credentials.

1. **Generate sandbox credentials** (one-shot, deterministic seed):
   ```sh
   bash tdesktop/tests/fixtures/gen-sandbox-secrets.sh
   # Writes: tdesktop/tests/fixtures/sandbox-secret
   #         tdesktop/tests/fixtures/sandbox-type3-secret.hex
   ```

2. **Build teleproxy** from `teleproto3/server/` (Linux only):
   ```sh
   make -C teleproto3/server -j$(nproc)
   # Output: teleproto3/server/objs/bin/mtproto-proxy
   ```

3. **Start teleproxy** with <!-- ban-list-doc: technical identifier -->PROXY-protocol<!-- /ban-list-doc --> listener (see §Server-config-snippet).

4. **Run the E2E driver** (`tdesktop/tests/test-proxy-protocol-e2e.cpp`), which
   internally:
   - Spawns the server subprocess (steps 2–3 are pre-conditions).
   - Runs a minimal C++ framing shim injecting v1 or v2 header per matrix axis.
   - Drives a Type3 client connection through the shim → server chain.
   - Scrapes `/metrics` on `127.0.0.1:8889` and asserts counter values.
   - Checks server stderr for client IP presence (NFR19).

5. **Re-run with `--no-cdn`** (no `--proxy-protocol` flag; AC #3).

## §CF-Worker-config-snippet

```typescript
// Cloudflare Worker — forwards client WebSocket to origin nginx.
// Origin nginx wraps the upstream hop in PROXY-protocol v1/v2.
export default {
  async fetch(request: Request, env: Env): Promise<Response> {
    const upgradeHeader = request.headers.get('Upgrade');
    if (upgradeHeader !== 'websocket') {
      return new Response('Expected WebSocket', { status: 426 });
    }
    const url = new URL(request.url);
    url.host = env.ORIGIN_NGINX;  // e.g. "127.0.0.1:443" in sandbox
    return fetch(new Request(url, request));
  },
};
```

In production, `env.ORIGIN_NGINX` points at the origin nginx host. In the
CI sandbox the E2E driver uses a minimal C++ framing shim instead of a real
CF Worker + nginx pair, for portability across CI runners.

## §Nginx-config-snippet

```nginx
# Inbound from CF egress IPs; CF-Connecting-IP carries the real client IP.
set_real_ip_from 173.245.48.0/20;
set_real_ip_from 103.21.244.0/22;
set_real_ip_from 103.22.200.0/22;
set_real_ip_from 103.31.4.0/22;
real_ip_header CF-Connecting-IP;

upstream teleproxy_backend {
    server 127.0.0.1:13129;  # sandbox port; use 3129 for prod
}

server {
    listen 443 ssl http2;
    server_name _;

    location /ws {
        proxy_pass http://teleproxy_backend;
        proxy_http_version 1.1;
        proxy_set_header Upgrade $http_upgrade;
        proxy_set_header Connection "upgrade";
        # Inject PROXY-protocol header on the upstream TCP hop (nginx >= 1.13).
        proxy_protocol on;
    }
}
```

The CI E2E driver uses a minimal C++ framing shim instead of a real nginx
process, for portability across CI runners that may not have nginx installed.

## §Server-config-snippet

```sh
# teleproxy with PROXY-protocol listener (upstream ebbc4f3).
# Default verbosity — NFR19 compliance. No -v flags.
# Sandbox port 13129; prod port 3129.
./teleproto3/server/objs/bin/mtproto-proxy \
    -p 13129 \
    --proxy-protocol \
    --aes-pwd tdesktop/tests/fixtures/sandbox-secret \
    -S "$(cat tdesktop/tests/fixtures/sandbox-type3-secret.hex)" \
    -M 1
```

`--no-cdn` variant (AC #3 — graceful fallback without <!-- ban-list-doc: technical identifier -->PROXY-protocol<!-- /ban-list-doc -->):

```sh
./teleproto3/server/objs/bin/mtproto-proxy \
    -p 13129 \
    --aes-pwd tdesktop/tests/fixtures/sandbox-secret \
    -S "$(cat tdesktop/tests/fixtures/sandbox-type3-secret.hex)" \
    -M 1
# --proxy-protocol absent → peer-IP used as true_remote.
```

## §Expected-counter-values

Counter names are the ACTUAL names from the materialised subtree (verified
2026-05-03 from `teleproto3/server/net/net-proxy-protocol.h` and
`net-type3-stats.c`). Values assume **one successful handshake** from client IP
`198.51.100.42` (TEST-NET-3, RFC 5737 — safe for CI fixtures).

### With <!-- ban-list-doc: technical identifier -->PROXY-protocol<!-- /ban-list-doc --> header (v1 or v2 matrix axis)

```
# Scraped from http://127.0.0.1:8889/metrics after handshake.
proxy_protocol_connections_total 1
proxy_protocol_errors_total 0
teleproto3_connections_total{command_type="type3"} 1
```

> **Forward-citation — per-IP labelled counters:** `te_per_ip_top_n{ip="…"}`
> and `te_rate_limit_bucket_count{ip="…"}` are placeholders for upstream commits
> `f083b07` and `be46337`. When those commits land via the story 1.8/1.9
> subtree-pull cron, amend this section with the actual Prometheus label names
> and expected values. Do NOT substitute counter names from the illustrative
> placeholders in the story Dev Notes.

### `--no-cdn` variant (AC #3)

```
proxy_protocol_connections_total 0       # parser not invoked — no header
proxy_protocol_errors_total 0
teleproto3_connections_total{command_type="type3"} 1   # handshake succeeded
```

Rate-limit and top-N counters reflect peer-IP (loopback `127.0.0.1` in CI).

## §No-cdn-fallback

When teleproxy is invoked **without** `--proxy-protocol`, the server accepts
inbound connections without attempting to parse a
<!-- ban-list-doc: technical identifier -->PROXY-protocol<!-- /ban-list-doc -->
header. The connection's `remote_ip` / `remote_ipv6` remains the peer-IP.

This behaviour is required by FR46: _"Absence of a header MUST gracefully
fall back to peer-IP without rejecting the connection."_

The E2E test verifies this (AC #3) by re-running the full driver with
`--no-cdn` and asserting:
1. Handshake completes (driver exit code 0).
2. `proxy_protocol_connections_total` stays at 0.
3. `teleproto3_connections_total{command_type="type3"}` increments by 1.
4. Server stderr contains no occurrence of the injected PROXY-header source IP.

## §Forward-citation

- **Epic 3 (anti-probe coexistence):** AR-C2 silent-close timing consumes
  `connection->true_remote` for per-IP rate-limit decisions. Without
  <!-- ban-list-doc: technical identifier -->PROXY-protocol<!-- /ban-list-doc -->
  propagation, anti-probe sees only the loopback peer when nginx terminates
  upstream. This E2E scenario is the prerequisite proof.
  _Consumed by Epic 3._

- **Epic 4 (CDN integration / signed-update flows):** The CF Worker → nginx
  chain validated here is the same chain Epic 4 ships in production deployment
  runbooks. _Consumed by Epic 4._

## §Sandbox-vs-prod caveat

**CI iteration** uses the sandbox (port 13129, test credentials from
`gen-sandbox-secrets.sh`). **Production-acceptance** is a single deliberate run
against `94.156.131.252:3129` gated by manual `workflow_dispatch` with
`environments/prod-acceptance` reviewer click-through. Production credentials
MUST NOT be reused for CI sandbox runs (Sterling-guard, 2026-04-26
strategy round 3).

## IUT pre-conditions

- teleproxy binary built from subtree including upstream commit `ebbc4f3`.
- `--proxy-protocol` flag present in binary (verify: `./mtproto-proxy --help | grep proxy-protocol`).
- Stats endpoint active on `127.0.0.1:8889` (default; override with `--stats-port`).
- Test credentials generated via `gen-sandbox-secrets.sh` (no production credentials).

## Expected outcomes

| AC | Assertion | Pass condition |
|----|-----------|----------------|
| #1 | `proxy_protocol_connections_total` increments | equals 1 after 1 handshake |
| #2 | Client IP absent from default-verbosity log | `grep` returns exit code 1 (zero matches) |
| #3 | `--no-cdn` handshake completes | exit code 0; `proxy_protocol_connections_total` stays 0 |
| #4 | Scenario manifest readable and normative | this document |
| #5 | Both v1 and v2 matrix axes pass CI | CI matrix green on both legs |
