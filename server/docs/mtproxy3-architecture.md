# mtProxy3 architecture (server perspective)

Server-side architecture of the Type3 transport. How the fork sits
between nginx and the Telegram DCs, and how the Type3 dispatch hook
integrates with upstream MTProxy's connection lifecycle.

> Note: a pre-monorepo version lives at `../../../docs/mtproxy3-architecture.md`
> with more operational detail. That document is authoritative until
> this one is filled in during the server-v0.1.0 epic.

## 1. Where Type3 plugs in

Upstream MTProxy's TCP RPC connection lifecycle has a `CRYPTO_INIT`
stage where the encryption layer is set up per-connection. The fork
adds a hook at that stage:

```
accept(tcp) → WS upgrade → CRYPTO_INIT ── hook ──▶ type3_dispatch_on_crypto_init()
                                                      │
                                                      ├─ ACCEPT        → Type3 session (lib call)
                                                      ├─ PASSTHROUGH   → existing MTProxy transports
                                                      └─ DROP_SILENT   → close without response, bump bad_header_drops
```

`type3_dispatch_on_crypto_init()` in
[`../net/net-type3-dispatch.c`](../net/net-type3-dispatch.c) calls into
`../../lib/` via `t3.h`. All wire-format decisions live in the library.

## 2. Frame layering

```
[ TCP ][ TLS ][ WebSocket frame ][ Type3 Session Header ][ Obfuscated-2 ][ MTProto ]
         │       │                                        │
         │       │                                        AES-256-CTR (lib/)
         │       RFC 6455 framing (net-websocket.c, unchanged upstream)
         nginx-terminated
```

- Client frames are masked; server frames are not (RFC 6455).
- WS header is plaintext; AES-256-CTR lives inside the frame payload.

## 3. File map (fork-local additions)

| File | Role |
| ---- | ---- |
| `net/net-type3-dispatch.{c,h}` | CRYPTO_INIT hook → calls `lib/` via `t3.h` |
| `net/net-type3-stats.{c,h}` | admin-local counters (bad_header_drops, silent_closes) |
| `tests/dispatch/test_tcpdump_assertion.sh` | AC-PROTO-002 wire-silence proof |
| `tests/dispatch/test_log_silence.sh` | AC-PROTO-003 log-silence proof |
| `tests/integration/coexistence_v1_v2_test.sh` | v1/v2 coexistence smoke |

Upstream-managed (do not edit):
- `net/net-websocket.{c,h}` — RFC 6455 framing
- `net/net-tcp-connections.c` — post-crypto encrypt/decrypt
- `net/net-tcp-rpc-ext-server.c` — connection init (the hook is *called* from here post-pull, not authored here)
- `common/`, `crypto/`, `jobs/`, `mtproto/` — upstream subtrees

## 4. Anti-probe invariants (normative)

Per [`../../spec/anti-probe.md`](../../spec/anti-probe.md):
- Malformed Type3 headers MUST result in silent close (no protocol-level error).
- Timing of close MUST fall within a fixed envelope (no "slow fail" that fingerprints).
- Server logs MUST NOT record remote identifiers for silently-closed probes.

CI enforcement: AC-PROTO-002, AC-PROTO-003 (shell tests above).

## 5. Observability

Admin-local only (Cat 11). No remote telemetry. Counters readable via
the existing MTProxy admin interface; see
[`runbook/tls-renewal.md`](runbook/tls-renewal.md) for how the ops
loop surfaces them.
