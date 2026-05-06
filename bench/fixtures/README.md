# Bench fixtures

`generate.sh` produces three binary fixture files used by the bench tooling:

| File | Size |
|------|------|
| `fixture-1mb.bin` | 1 048 576 bytes |
| `fixture-10mb.bin` | 10 485 760 bytes |
| `fixture-50mb.bin` | 52 428 800 bytes |

## Usage

From the `teleproto3` repo root:

```bash
cd bench/fixtures
bash generate.sh
```

## Design notes

**Non-deterministic by design.** Each run draws fresh bytes from `/dev/urandom`.
Re-running produces different files (and a fresh `manifest.json`). This is intentional:

- *Anti-dedup* — a server-side caching layer (if one ever appeared) would not get cache hits across runs.
- *TSPU/DPI fingerprinting* — inspection devices may fingerprint repeated byte patterns; random payloads avoid accidental signatures (relevant for 1a-7 TSPU validation run).

**Incompressible.** `/dev/urandom` output has maximum entropy; neither TLS compression nor WS `permessage-deflate` can reduce payload size. This keeps throughput measurements honest — no skew from compression ratios.

**Three sizes, not one.** 1 MiB reveals handshake/TTFB overhead. 10 MiB measures steady-state throughput across multiple WS frames. 50 MiB exercises AES-CTR pipeline and ACK behaviour under sustained load.

## Consumers

- `1a-3` — Python bench client sends fixtures over Type3 WS connection
- `1a-5` — bench driver runs the full 3×3×N matrix against all three sizes
- `1a-6` — smoke harness uses 1 MiB only for a quick sub-1-minute CI run
- `1a-7` — TSPU validation run uses all three sizes from a Russian VPS

## Cross-machine usage

When the same fixture set must be present on two machines (e.g. 1a-7 TSPU
validation: bench client on operator laptop, server on Russian VPS), the
non-deterministic-per-run property means **regenerating on each side
produces different bytes**. The expected pattern is:

1. Generate fixtures **once** on the side that drives the run (typically
   the bench client).
2. `scp` / `rsync` the resulting `fixture-*.bin` plus `manifest.json` to
   the peer, preserving both files together.
3. Both sides verify the same SHA-256 from `manifest.json` against the
   bytes on disk before any benchmark cell starts.

The bench echo (1a-2) reflects bytes back, so only the **client** needs
the fixtures on disk; the server reads from the wire and never opens
`manifest.json`. The cross-machine note is therefore mostly relevant for
operator-driven workflows that re-verify integrity on the destination
host before kicking off the run.

## What is committed

Only `generate.sh`, `.gitignore`, and this `README.md` are tracked by git.
The generated `*.bin` files and `manifest.json` are excluded via `.gitignore`.
