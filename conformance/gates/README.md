# conformance/gates/

Mechanically-enforced anti-probe invariants for the Type3 protocol.
Each gate is a self-contained Python script that can be invoked locally
or from CI. Story 7-2 introduces this directory; later Epic 7 stories
add further gates.

## Gate-directory contract

Every gate consists of four artefacts:

| Artefact | Location | Purpose |
|----------|----------|---------|
| Gate script | `conformance/gates/<gate>.py` | Measurement + statistical assertion |
| Threshold YAML | `conformance/baselines/<gate>-threshold.yaml` | Normative thresholds |
| Workflow | `.github/workflows/<gate>.yml` | CI trigger + PR comment |
| README entry | This file (below) | Usage, JSON schema, exit codes |

Gate scripts follow these invariants:
- **Exit codes:** `0` = PASS, `1` = FAIL (threshold violated), `2` = setup error
  (IUT unreachable, insufficient samples, malformed input). Mirrors `verify.py`.
- **Determinism:** RNG seeded explicitly (`--seed`); re-runs reproduce verdicts.
- **JSON output:** written to `--output FILE` (default `results.json`).
- **AR-G5:** every PR touching gate artefacts passes `ci/identity-audit.sh`.

---

## Gate: `timing_invariant.py` (AR-C2, Story 7-2)

Enforces the AR-C2 anti-probe invariant: the server's silent-close delay
MUST be statistically independent of input length. A probe that sends
payloads of varying lengths MUST NOT recover timing-channel information.

**Spec reference:** `spec/anti-probe.md §8 Timing Invariants`
**Thresholds:** `conformance/baselines/timing-threshold.yaml`
**Workflow:** `.github/workflows/conformance-timing.yml`

### Usage

```sh
# Against a running server (endpoint mode):
python3 conformance/gates/timing_invariant.py \
    --samples 10000 \
    --endpoint 127.0.0.1:8443 \
    --output results.json

# Against a local binary (subprocess mode; binary must accept --port PORT):
python3 conformance/gates/timing_invariant.py \
    --samples 10000 \
    --impl ./objs/bin/mtproto-proxy \
    --output results.json

# Nightly mode (N=100000, no runtime budget):
python3 conformance/gates/timing_invariant.py \
    --nightly \
    --impl ./objs/bin/mtproto-proxy \
    --output nightly.json

# With fuzz-harness cross-check (AC #6):
python3 conformance/gates/timing_invariant.py \
    --endpoint 127.0.0.1:8443 \
    --fuzz-data lib/fuzz/side-channel-latest.log \
    --output results.json
```

### Arguments

| Flag | Default | Description |
|------|---------|-------------|
| `--impl PATH` | — | Start binary as subprocess server (mutually exclusive with `--endpoint`) |
| `--endpoint HOST:PORT` | — | Connect to running IUT (mutually exclusive with `--impl`) |
| `--samples N` | 10000 | Number of probe samples per run |
| `--output FILE` | `results.json` | JSON result destination (`-` for stdout) |
| `--fuzz-data CSV` | — | TSV from fuzz harness for AC #6 concurrence check |
| `--seed N` | 42 | RNG seed for reproducible probe generation |
| `--nightly` | off | No runtime budget; `--samples` defaults to 100000 |
| `--branch NAME` | `$GITHUB_REF_NAME` | Branch name for drift-alert messages |

### JSON output schema

```json
{
  "p_mann_whitney_min": 0.3142,
  "rho_spearman": 0.0023,
  "verdict": "PASS",
  "samples_per_bucket": [1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000],
  "baseline_drift_pct": 1.2,
  "samples_total": 10000,
  "runtime_ms": 45200,
  "verdict_history": [0.3142],
  "bucket_boundaries": [
    {"lo": 1, "hi": 3}, {"lo": 4, "hi": 11}, "…"
  ]
}
```

| Field | Type | Description |
|-------|------|-------------|
| `p_mann_whitney_min` | float | Minimum Mann-Whitney U p-value across all 45 bucket-pairs |
| `rho_spearman` | float | Spearman ρ between input length and close delay |
| `verdict` | string | `"PASS"` or `"FAIL"` |
| `samples_per_bucket` | int[] | Sample count per length bucket (10 elements) |
| `baseline_drift_pct` | float | Maximum p50/p95/p99 drift vs. prior tagged baseline (%) |
| `samples_total` | int | Total samples collected across all buckets |
| `runtime_ms` | int | Wall-clock runtime of the final measurement run (ms) |
| `verdict_history` | float[] | p_min values from each run (1 or 3 if borderline) |
| `bucket_boundaries` | object[] | `lo`/`hi` byte-range per bucket |

### Pass/fail criteria

```
PASS  iff  p_mann_whitney_min > 0.05  AND  |rho_spearman| < 0.1
FAIL  iff  either threshold violated
exit 2  iff  setup error (IUT unreachable, bucket underfilled, etc.)
```

Thresholds are normative and read from
`conformance/baselines/timing-threshold.yaml`.

### Flake handling (AC #8)

When p_min ∈ [0.04, 0.06] (borderline band), the gate re-runs up to 2
additional times (3 total). Verdict = PASS if ≥ 2/3 runs PASS (p > 0.05).
Total PR runtime budget: 6 minutes. WARN emitted if any run exceeds 2 min.
Nightly mode runs exactly once (no re-run, no budget).

### Min-bucket invariant (AC #9)

All 10 length buckets must contain ≥ 100 samples. If any bucket is
underfilled (typically because the input-length generator skips a range),
the gate exits 2 with:

```
error: bucket-underfilled: bucket=<i> samples=<n> required=100
```

Fix the input-generation strategy, not the threshold.

### Testing

```sh
cd teleproto3
pip install -r conformance/requirements.txt pytest
python3 -m pytest conformance/gates/tests/ -v
```
