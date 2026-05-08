# TSPU Validation Machine Profile

## Hardware / OS

| Field | Value |
|-------|-------|
| Hostname | `lnx` (anonymised) |
| OS | Ubuntu 25.10 (Questing Quokka) |
| Kernel | Linux 6.17.0-19-generic x86_64 |
| Architecture | x86_64 |
| Connectivity | Ethernet LAN → residential router → ISP |

## ISP / Network

| Field | Value |
|-------|-------|
| ISP | `<redacted>` — residential ISP, Russian Federation |
| Region | `<redacted>` — federal subject redacted per privacy policy |
| TSPU class | Residential ТСПУ (deep packet inspection at ISP egress) |
| Public IP | `<redacted>` |

## Bench Environment

| Component | Version |
|-----------|---------|
| Python | 3.13.7 |
| cryptography | 43.0.0 |
| iperf3 | not installed (ratio column N/A; gate validity-based) |
| bench repo | `teleproto3-bench` cloned from GitHub, commit `67b0eea` |
| fixtures | generated on-host via `fixtures/generate.sh` (`/dev/urandom`) |
| `.credentials` | configured with `BENCH_DOMAIN`, `BENCH_PATH`, `BENCH_SECRET` |

## TSPU Verification

The machine is behind a residential ISP in the Russian Federation that applies ТСПУ (ТСПУ — технические средства противодействия угрозам) filtering at ISP egress. Outbound traffic traverses the ISP's DPI stack before reaching the open internet.

Pre-run verification: outbound connections to known Telegram IP ranges were confirmed throttled/blocked by the ISP (standard TSPU behaviour), confirming the machine is subject to active DPI inspection during the bench run.

## Run Date

2026-05-08
