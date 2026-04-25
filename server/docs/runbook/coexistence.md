# Runbook: v1 + v2 coexistence

Steady-state operation of a server instance serving both legacy v1
clients and v2 (teleproto3) clients from the same host and port.

## Configuration

_TBD(server-v0.1.0):_ canonical config snippet. Shape:

```
# /etc/mtproto-proxy/secrets.conf
secret_v1 = 0x<16-byte hex>@domain.nip.io
secret_v2 = 0xff<16-byte hex><UTF-8 domain>
```

Both secrets are loaded at startup. The Type3 dispatch hook
disambiguates per-connection based on the Session Header
(`type3_dispatch_on_crypto_init()`).

## Invariants under coexistence

- v1 handshakes MUST continue to work byte-for-byte unchanged from
  pre-v2 behaviour. Any deviation is a regression.
- v2 dispatch MUST NOT examine v1 frames; the dispatch is decided
  before any v2-specific parsing runs.
- A single connection is either v1 or v2 for its lifetime; there is
  no mid-stream upgrade.

## Monitoring

- `connections_accepted` per-version buckets.
- `bad_header_drops` MUST NOT tick upward purely because of v1
  traffic. If it does, the dispatch is misclassifying v1 as
  malformed v2 — bug.

## Exit criteria

Coexistence is scaffolding; see [`migration-from-v1.md`](migration-from-v1.md)
for the plan to retire v1. When the v1 secret is removed, this runbook
is retired.
