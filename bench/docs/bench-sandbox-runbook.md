# Bench Sandbox Runbook

Extends the 1a-2 sandbox deployment (bench-enabled server on port 3140 / separate
`bench-arctic-breeze.my.id` nginx server_name) with iperf3 and bench driver setup.

## Prerequisites

- Bench VPS already running with `TELEPROTO3_BENCH=1` and `--enable-bench-handler`
  (see `teleproto3/server/docs/bench-deploy.md` from Story 1a-2)
- `.credentials` file in `teleproto3/bench/` with bench-instance creds:

```bash
BENCH_DOMAIN="bench-arctic-breeze.my.id"   # NOT the prod proxy host
BENCH_PATH="/ws"
BENCH_SECRET="ff<32-hex-bytes><utf8-domain>"  # bench-specific secret
BENCH_PORT=443
IPERF3_PORT=5201
```

> **Never use prod credentials** (`arctic-breeze.my.id`) for bench runs.
> The bench instance must be separate to avoid interfering with prod traffic.

## iperf3 Server — Deploy on Bench VPS

iperf3 provides the baseline TCP throughput measurement on the same network path,
without the nginx+WS+obf2+AES-CTR overhead. Ratio `bench_p50 / iperf3_Mbps`
quantifies our stack overhead.

### 1. Install iperf3

```bash
# Debian/Ubuntu
apt-get install -y iperf3

# Verify
iperf3 --version
```

### 2. Run iperf3 server (background, port 5201)

```bash
# As a one-shot server (exits after one client connects):
iperf3 --server --port 5201 --one-off --daemon --logfile /var/log/iperf3.log

# Or as a persistent service:
iperf3 --server --port 5201 --daemon --logfile /var/log/iperf3.log
```

### 3. Firewall: allow iperf3 port

```bash
# ufw
ufw allow 5201/tcp comment 'iperf3 bench baseline'

# iptables fallback
iptables -A INPUT -p tcp --dport 5201 -j ACCEPT
```

### 4. Verify from MacBook

```bash
iperf3 -c bench-arctic-breeze.my.id -p 5201 -t 10
```

Expected: > 10 Mbps on a VPS with decent egress. Lower is a VPS cap, not our overhead.

### 5. Restart iperf3 for each bench session (--one-off mode)

If running in `--one-off` mode, restart before each `bench.sh` run:

```bash
# On bench VPS:
pkill iperf3 2>/dev/null; iperf3 --server --port 5201 --one-off --daemon
```

For multi-session use, run persistently without `--one-off`.

## Running the Full Bench Matrix

```bash
cd teleproto3/bench

# Dry-run first (verify matrix, no network traffic):
./bench.sh --smoke --dry-run

# Smoke run (~3 min, 9 runs):
./bench.sh --smoke

# Full matrix (~20 min, 99 runs) with artefact commit:
./bench.sh --commit
```

Artefacts are saved to `_bmad-output/measurements/teleproxy-bench-YYYY-MM-DD-HHMMSS/`.

## Interpreting the Report

The acceptance gate is **validity-driven** — ratio is informational only.

| Gate            | Meaning                                                               | Action                              |
|-----------------|-----------------------------------------------------------------------|-------------------------------------|
| `PASS`          | All cells VALID                                                       | Green-light Epic 2 release stories  |
| `WARN`          | At least one HIGH_VARIANCE cell, no INVALID cells                     | Ship with caveat or draft Epic 1b   |
| `FAIL`          | Any INVALID cell, OR zero-data run (all `n_valid==0`)                 | Mandatory Epic 1b before release    |
| `INDETERMINATE` | Smoke mode (`n_valid<10` by design) — gate not applicable             | Read process exit code for liveness |

Per-cell validity (full runs only; smoke skips this gate):

- `VALID`         — `n_valid ≥ 10` AND `error_class==ok` rate ≥ 90% AND (echo only) `sha256_match_rate == 1.0` AND `stddev/median < 0.5`.
- `HIGH_VARIANCE` — VALID candidate but `stddev/median ≥ 0.5`. Numbers caveat-flagged.
- `INVALID`      — `n_valid < 10` OR ANY echo-mode SHA-256 mismatch OR `error_class != ok` rate > 10%.

Percentile method: `statistics.quantiles(method='inclusive')` — comparable to Prometheus / Grafana / Datadog. No extrapolation past observed range.

## iperf3 Ratio (informational, not in gate)

The ratio `bench_p50 / iperf3_Mbps` is rendered for context only. iperf3 measures raw TCP throughput; bench measures the full stack (TLS + nginx + WS + AES-256-CTR + Type3). They are not like-for-like — a low ratio can mean either real stack overhead or VPS CPU/egress coupling, which is why ratio does not gate release decisions.

| iperf3   | bench p50 | Ratio | Diagnostic interpretation                                       |
|----------|-----------|-------|-----------------------------------------------------------------|
| 50 Mbps  | 45 Mbps   | 0.90  | Stack overhead ~10%; VPS has CPU+egress to spare                |
| 50 Mbps  | 35 Mbps   | 0.70  | Stack overhead ~30%; healthy for TLS+WS+AES on a typical VPS    |
| 50 Mbps  | 10 Mbps   | 0.20  | Investigate — likely AES not vectorised or syscall thrash       |
| 12 Mbps  | 10 Mbps   | 0.83  | ISP-capped VPS; bench is at the network ceiling                 |
| (absent) | 10 Mbps   | N/A   | iperf3 unavailable — gate unaffected, just lose this diagnostic |

## Troubleshooting

**iperf3 connection refused:** iperf3 server not running on bench VPS. See step 2 above.

**bench.sh reachability probe fails:** Bench server down or nginx config missing for
`bench-arctic-breeze.my.id`. Check `systemctl status teleproxy-bench`.

**FAIL with many `timeout` error_class rows:** TCP connection succeeds but bench
handler not enabled. Verify server was compiled with `TELEPROTO3_BENCH=1` and
started with `--enable-bench-handler`.

**HIGH_VARIANCE cells:** Re-run the full matrix at a quieter time. Variance > 50%
usually indicates ISP throttling or competing traffic on the bench VPS.
