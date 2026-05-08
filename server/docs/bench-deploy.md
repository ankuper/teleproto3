# Bench-only sandbox deploy

> Story 1a-2, AC #7. **Dev-self-use only.** Never deploy to a host that
> serves production traffic. Bench traffic exposes a `command_type=0x04`
> dispatch path that is mutually exclusive with the production
> `command_type=0x01` MTProto path on a single instance.

## Why a separate instance

Production binaries are built **without** `TELEPROTO3_BENCH`, so the
bench dispatch case is absent at link time and `0x04` traffic is rejected
by the existing default path (silent close). To exercise the bench
handler, you need a **separate** binary built with
`TELEPROTO3_BENCH=1` running on a different listener.

The deployed prod prototype at `arctic-breeze.my.id:443/ws/7f34ba` is the
production instance and **must remain bench-symbol-free**. The
`bench-release-audit` CI workflow (`.github/workflows/bench-release-audit.yml`)
asserts that with every PR.

## Topology

```
┌──────────────────────────────────────────────────────────────────┐
│  arctic-breeze.my.id (single host)                               │
│                                                                  │
│  nginx :443 ──► server_name=arctic-breeze.my.id  ─► proxy :3129  │ ← prod, no bench
│       │                                                           │
│       └──────► server_name=bench-arctic-breeze.my.id ─► :3140    │ ← bench
│                                                                  │
│  proxy :3129    teleproxy (TELEPROTO3_BENCH=0)                   │
│  proxy :3140    teleproxy (TELEPROTO3_BENCH=1, --enable-bench-handler)
└──────────────────────────────────────────────────────────────────┘
```

Different listeners, different `server_name`. The bench `server_name` need
not be publicly resolvable — point local `/etc/hosts` (or the bench
client's `--host` flag) at the prod IP and the request reaches the bench
listener via SNI routing.

## Build the bench binary

```bash
cd teleproto3/lib && make -j$(nproc)
cd ../server     && make -j$(nproc) TELEPROTO3_BENCH=1 \
                       EXTRA_CFLAGS='-DTELEPROTO3_BENCH'
```

Output: `objs/bin/teleproxy`. Quick sanity:

```bash
nm objs/bin/teleproxy | grep bench_handler   # should print at least one symbol
./objs/bin/teleproxy --help | grep bench     # --enable-bench-handler shown
```

If you also build a release binary (no `TELEPROTO3_BENCH`), `nm | grep bench_handler`
must return zero — `tests/test_bench_ci_release.sh` enforces this.

## Run the bench instance

```bash
./objs/bin/teleproxy \
  --enable-bench-handler \                 # runtime gate (Story 1a-2 AC #5)
  -H 3140 \                                # bench listener port
  -S "$BENCH_SECRET" \                     # SEPARATE from prod secret
  --domain bench-arctic-breeze.my.id \
  --config bench.toml
```

**Use distinct test credentials**, never the prod `aes-pwd` / Type3 secret.
The deployed-prod-prototype contract (sprint-status.yaml: "Sandbox MUST
use separate test credentials, NOT prod credentials") applies.

## nginx server_name routing

Add a second `server { listen 443 ssl; server_name bench-arctic-breeze.my.id; ... }`
block forwarding to `127.0.0.1:3140`. Nothing else changes; the existing
prod block (server_name=`arctic-breeze.my.id`) keeps routing to `:3129`.

## Verify the prod listener still rejects `0x04`

This is part of AC #7 — capture proof:

```bash
# From the bench client host, try to bench the prod listener:
python3 teleproto3/bench/bench_client.py \
   --host arctic-breeze.my.id --secret "$PROD_SECRET" --mode echo --bytes 1M

# Expected: connection terminated cleanly. Server log shows neither
# "BENCH" nor a bench_handler stack trace, because the symbol is absent.
```

Capture `tcpdump -nnvvi any host arctic-breeze.my.id and port 443`
during the run and store the snippet in `_bmad-output/implementation-artifacts/`
for traceability.

## Decommission

```bash
# Stop and remove the bench instance:
systemctl stop teleproxy-bench
rm /etc/teleproxy/bench.toml
nginx -s reload    # remove the bench server_name block first
```

The prod instance is unaffected. The bench host can be reclaimed for
other dev work — there is no persistent bench state between runs (state
lives in process memory only).

## Measurement bias: pre-AES-CTR path

The bench handler operates at the **AR-S2 layer (post-WS-unmask, pre-obf2/AES-CTR)**.
This is a documented scope deviation from the original Dev Notes, which asked the handler
to exercise the full production hot path through `cpu_tcp_aes_crypto_*`.

**What this means for throughput numbers:**

- SINK / ECHO / SOURCE bytes flow through TLS termination (nginx) + WS framing
  (MTProxy) + bench handler, but they do **not** pass through the obfuscated-2
  header parsing or the AES-256-CTR encrypt/decrypt that production MTProto traffic
  traverses.
- Measured throughput will be **higher** than what a real MTProto session achieves
  over the same path, because AES-CTR overhead (typically 5–15 % CPU on modern
  hardware) is absent.
- For the primary goal of comparing Type3 vs non-Type3 across network conditions
  (the 1a-7 TSPU validation run), this bias is constant and cancels out in the
  ratio. It only matters if you want absolute throughput parity with production.

**If you need post-AES-CTR measurements:** a follow-up patch can hook the handler
into the `cpu_tcp_aes_crypto_ctr128_decrypt_input` path. The Python bench client
(Story 1a-3) is symmetric on the choice — switching layers is a server-side change
only. Defer unless the TSPU run shows the delta is material.

## Out of scope

- Public discovery: bench `server_name` is intentionally not in DNS.
- Authentication: loopback or VPN-restricted. The bench secret is
  rotated per session.
- Rate limiting: dev-only single-user tool. Prod's rate limits are
  unaffected because they run on the prod instance, not this one.
