#!/usr/bin/env python3
# timing_invariant.py — AR-C2 timing-invariant CI gate (Mann-Whitney + Spearman).
# Story 7-2, AC #1-#9. Measures silent-close delay independence from input length.
# Exit codes: 0 (PASS), 1 (FAIL), 2 (setup error).
#
# This is the reference conformance gate for the Type3 anti-probe timing
# invariant. spec/anti-probe.md §8 is normative; this script enforces it
# mechanically.  File a bug at the teleproto3 issue tracker.
#
# Usage:
#   python3 timing_invariant.py --samples 10000 --impl ./objs/bin/mtproto-proxy
#   python3 timing_invariant.py --samples 10000 --endpoint 127.0.0.1:8443 \
#       --output results.json
#   python3 timing_invariant.py --samples 100000 --nightly \
#       --endpoint 127.0.0.1:8443 --output nightly.json

from __future__ import annotations

import argparse
import csv
import glob
import hashlib
import io
import json
import math
import os
import random
import re
import socket
import subprocess
import sys
import time
from contextlib import contextmanager
from itertools import combinations
from pathlib import Path
from typing import Iterator, NamedTuple, Optional

try:
    from scipy.stats import mannwhitneyu, spearmanr
except ImportError:
    print(
        "error: scipy is required — pip install scipy",
        file=sys.stderr,
    )
    sys.exit(2)

# ------------------------------------------------------------------ #
# Constants — normative thresholds live in timing-threshold.yaml.     #
# These defaults mirror the YAML in case the file is absent.          #
# ------------------------------------------------------------------ #
_THRESHOLD_YAML_PATH = (
    Path(__file__).parent.parent / "baselines" / "timing-threshold.yaml"
)
_DEFAULT_P_THRESHOLD: float = 0.05
_DEFAULT_RHO_THRESHOLD: float = 0.1
_DEFAULT_SAMPLES: int = 10_000
_DEFAULT_BUCKET_COUNT: int = 10
_DEFAULT_DRIFT_THRESHOLD_PCT: float = 5.0
_MIN_VALID_LEN: int = 1       # minimum probe payload length
_MAX_VALID_LEN: int = 65535   # maximum WS 16-bit-length payload
_MIN_BUCKET_SAMPLES: int = 100  # AC #9 minimum per-bucket fill
_BORDERLINE_LOW: float = 0.04   # AC #8 flake-band lower bound
_BORDERLINE_HIGH: float = 0.06  # AC #8 flake-band upper bound
_MAX_RUNS: int = 3              # AC #8 maximum re-run count
_RUN_BUDGET_NS: int = 2 * 60 * 10 ** 9   # 2 min per run in ns
_TOTAL_BUDGET_NS: int = 6 * 60 * 10 ** 9  # 6 min total in ns
_PROBE_CONNECT_TIMEOUT_S: float = 5.0     # per-connection timeout
_IUT_STARTUP_TIMEOUT_S: float = 30.0     # time to wait for IUT to start


# ------------------------------------------------------------------ #
# Data types                                                           #
# ------------------------------------------------------------------ #

class SetupError(RuntimeError):
    """Raised for exit-code-2 conditions (harness / IUT problems)."""


class Sample(NamedTuple):
    input_len: int
    input_hash: str
    close_delay_ns: int


class RunResult(NamedTuple):
    p_mann_whitney_min: float
    rho_spearman: float
    samples: list[Sample]
    samples_per_bucket: list[int]
    bucket_boundaries: list[tuple[int, int]]
    runtime_ns: int
    verdict: str  # "PASS" | "FAIL"


# ------------------------------------------------------------------ #
# Threshold loading                                                    #
# ------------------------------------------------------------------ #

def _load_thresholds() -> dict:
    """Load thresholds from YAML; return defaults if file absent."""
    if not _THRESHOLD_YAML_PATH.exists():
        return {
            "mann_whitney_p_threshold": _DEFAULT_P_THRESHOLD,
            "spearman_rho_threshold": _DEFAULT_RHO_THRESHOLD,
            "samples_per_run": _DEFAULT_SAMPLES,
            "bucket_count": _DEFAULT_BUCKET_COUNT,
            "drift_threshold_pct": _DEFAULT_DRIFT_THRESHOLD_PCT,
        }
    text = _THRESHOLD_YAML_PATH.read_text(encoding="utf-8")
    result: dict = {}
    for line in text.splitlines():
        m = re.match(r'^(\w+):\s*([\d.]+)', line)
        if m:
            key, val = m.group(1), m.group(2)
            result[key] = float(val) if '.' in val else int(val)
    return result


# ------------------------------------------------------------------ #
# Bucket generation                                                    #
# ------------------------------------------------------------------ #

def _compute_bucket_boundaries(n_buckets: int) -> list[tuple[int, int]]:
    """Return n_buckets logarithmically-spaced (lo, hi) inclusive pairs.

    Spans _MIN_VALID_LEN .. _MAX_VALID_LEN.
    """
    lo_log = math.log(_MIN_VALID_LEN)
    hi_log = math.log(_MAX_VALID_LEN + 1)
    step = (hi_log - lo_log) / n_buckets
    boundaries: list[tuple[int, int]] = []
    prev = _MIN_VALID_LEN
    for i in range(n_buckets):
        hi_f = math.exp(lo_log + (i + 1) * step)
        hi = max(prev, min(int(hi_f) - 1, _MAX_VALID_LEN))
        if i == n_buckets - 1:
            hi = _MAX_VALID_LEN
        boundaries.append((prev, hi))
        prev = hi + 1
    return boundaries


def _bucket_index(
    length: int,
    boundaries: list[tuple[int, int]],
) -> int:
    for i, (lo, hi) in enumerate(boundaries):
        if lo <= length <= hi:
            return i
    return len(boundaries) - 1


# ------------------------------------------------------------------ #
# IUT management                                                       #
# ------------------------------------------------------------------ #

def _find_free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


@contextmanager
def _iut_context(
    impl: Optional[str],
    endpoint: Optional[str],
) -> Iterator[tuple[str, int]]:
    """Yield (host, port) for the running IUT.

    impl mode: start binary as subprocess on a random local port.
    endpoint mode: parse HOST:PORT directly.
    """
    if endpoint:
        if ':' not in endpoint:
            raise SetupError(f"endpoint must be HOST:PORT, got: {endpoint!r}")
        host, port_str = endpoint.rsplit(':', 1)
        try:
            port = int(port_str)
        except ValueError:
            raise SetupError(f"invalid port in endpoint: {endpoint!r}")
        yield host, port
        return

    if not impl:
        raise SetupError("one of --impl or --endpoint is required")

    port = _find_free_port()
    proc = subprocess.Popen(
        [impl, '--port', str(port)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    try:
        # Poll until the port accepts connections (up to _IUT_STARTUP_TIMEOUT_S).
        deadline = time.monotonic() + _IUT_STARTUP_TIMEOUT_S
        ready = False
        while time.monotonic() < deadline:
            try:
                s = socket.create_connection(('127.0.0.1', port), timeout=1.0)
                s.close()
                ready = True
                break
            except OSError:
                time.sleep(0.05)
        if not ready:
            proc.terminate()
            _, stderr_bytes = proc.communicate(timeout=5)
            raise SetupError(
                f"IUT {impl!r} did not start within {_IUT_STARTUP_TIMEOUT_S}s "
                f"on port {port}; stderr: {stderr_bytes.decode('utf-8', 'replace')[:200]}"
            )
        yield '127.0.0.1', port
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()


# ------------------------------------------------------------------ #
# Probe measurement                                                    #
# ------------------------------------------------------------------ #

def _measure_close_delay(
    host: str,
    port: int,
    payload: bytes,
) -> Optional[int]:
    """Return close_delay_ns or None on timeout/error."""
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(_PROBE_CONNECT_TIMEOUT_S)
        t_start = time.perf_counter_ns()
        sock.connect((host, port))
        sock.sendall(payload)
        # Drain until EOF — server closes the connection.
        while True:
            chunk = sock.recv(4096)
            if not chunk:
                break
        t_end = time.perf_counter_ns()
        sock.close()
        return t_end - t_start
    except Exception:
        try:
            sock.close()
        except Exception:
            pass
        return None


# ------------------------------------------------------------------ #
# Core measurement loop                                                #
# ------------------------------------------------------------------ #

def _run_measurement(
    host: str,
    port: int,
    n_samples: int,
    n_buckets: int,
    rng: random.Random,
) -> tuple:
    """Collect n_samples timing measurements; return (RunResult, drift_pct)."""
    boundaries = _compute_bucket_boundaries(n_buckets)
    samples_per_bucket: list[int] = [0] * n_buckets
    all_samples: list[Sample] = []

    t_run_start = time.perf_counter_ns()

    samples_per_bkt = max(n_samples // n_buckets, 1)
    total_target = samples_per_bkt * n_buckets

    for i, (lo, hi) in enumerate(boundaries):
        collected = 0
        attempts = 0
        max_attempts = samples_per_bkt * 5  # allow up to 5× retries for timeouts
        while collected < samples_per_bkt and attempts < max_attempts:
            attempts += 1
            length = rng.randint(lo, hi)
            payload = rng.randbytes(length)
            input_hash = hashlib.sha256(payload).hexdigest()
            delay = _measure_close_delay(host, port, payload)
            if delay is None:
                continue
            all_samples.append(Sample(length, input_hash, delay))
            collected += 1
        samples_per_bucket[i] = collected

    t_run_end = time.perf_counter_ns()
    runtime_ns = t_run_end - t_run_start

    if not all_samples:
        raise SetupError("no samples collected — IUT unreachable or always timing out")

    # Runtime budget check (AC #8)
    if runtime_ns > _RUN_BUDGET_NS:
        minutes = runtime_ns / 60 / 10 ** 9
        print(
            f"WARN: runtime-budget exceeded ({minutes:.1f} min); "
            "consider sample-count reduction in CI mode",
            file=sys.stderr,
        )

    # Compute statistics over all collected samples.
    lengths = [s.input_len for s in all_samples]
    delays = [s.close_delay_ns for s in all_samples]

    # Spearman ρ
    rho_result = spearmanr(lengths, delays)
    rho = float(rho_result.statistic)

    # Mann-Whitney U: all C(n_buckets,2) pairings, minimum p-value.
    bucket_delay_lists: list[list[int]] = [[] for _ in range(n_buckets)]
    for s in all_samples:
        idx = _bucket_index(s.input_len, boundaries)
        bucket_delay_lists[idx].append(s.close_delay_ns)

    p_min = 1.0
    for bi, bj in combinations(range(n_buckets), 2):
        a, b = bucket_delay_lists[bi], bucket_delay_lists[bj]
        if len(a) < 2 or len(b) < 2:
            continue
        mw = mannwhitneyu(a, b, alternative='two-sided')
        p = float(mw.pvalue)
        if p < p_min:
            p_min = p

    thresholds = _load_thresholds()
    p_thresh = float(thresholds.get("mann_whitney_p_threshold", _DEFAULT_P_THRESHOLD))
    rho_thresh = float(thresholds.get("spearman_rho_threshold", _DEFAULT_RHO_THRESHOLD))
    verdict = "PASS" if (p_min > p_thresh and abs(rho) < rho_thresh) else "FAIL"

    # Baseline drift percentage (for JSON output; caller uses for AC #4)
    baseline_drift_pct = _compute_baseline_drift_pct(delays)

    return RunResult(
        p_mann_whitney_min=p_min,
        rho_spearman=rho,
        samples=all_samples,
        samples_per_bucket=samples_per_bucket,
        bucket_boundaries=boundaries,
        runtime_ns=runtime_ns,
        verdict=verdict,
    ), baseline_drift_pct


# ------------------------------------------------------------------ #
# AC #9: Min-bucket invariant                                          #
# ------------------------------------------------------------------ #

def _check_bucket_fill(samples_per_bucket: list[int]) -> None:
    """Exit 2 if any bucket is under-filled."""
    for i, n in enumerate(samples_per_bucket):
        if n < _MIN_BUCKET_SAMPLES:
            print(
                f"error: bucket-underfilled: bucket={i} samples={n} "
                f"required={_MIN_BUCKET_SAMPLES}",
                file=sys.stderr,
            )
            sys.exit(2)


# ------------------------------------------------------------------ #
# AC #4: Baseline drift alert                                          #
# ------------------------------------------------------------------ #

def _compute_baseline_drift_pct(delays: list[int]) -> float:
    """Compare current p50/p95/p99 against Story 7-3 baselines; return max drift %.

    Story 7-3 writes conformance/baselines/lib-v<tag>/<os>.yaml with
    p50_ns/p95_ns/p99_ns keys for the timing gate's measurement.
    Flat lib-v*.yaml files (from Stories 1-7/1-10) measure t3_silent_close_delay_sample_ns
    and are NOT comparable — we ignore them here.
    """
    if not delays:
        return 0.0
    # pending-7-3: only look for subdirectory-format baselines (Story 7-3 produces them).
    # Until 7-3 lands, baseline_files will be empty and drift returns 0.0.
    baseline_glob = str(
        Path(__file__).parent.parent / "baselines" / "lib-v*" / "*.yaml"
    )
    baseline_files = sorted(glob.glob(baseline_glob))
    if not baseline_files:
        # pending-7-3: no baselines exist yet; log and skip.
        print(
            "WARN: no prior baseline, drift check skipped "
            "(conformance/baselines/lib-v*/timing.yaml not found)",
            file=sys.stderr,
        )
        return 0.0
    latest = baseline_files[-1]
    try:
        text = Path(latest).read_text(encoding="utf-8")
    except OSError:
        return 0.0

    def _extract_ns(key: str) -> Optional[int]:
        m = re.search(rf'{re.escape(key)}:\s*(\d+)', text)
        return int(m.group(1)) if m else None

    sorted_delays = sorted(delays)
    n = len(sorted_delays)

    def _percentile(pct: float) -> int:
        idx = max(0, int(math.ceil(n * pct / 100.0)) - 1)
        return sorted_delays[idx]

    current = {
        'p50': _percentile(50),
        'p95': _percentile(95),
        'p99': _percentile(99),
    }
    baseline_keys = {'p50': 'p50_ns', 'p95': 'p95_ns', 'p99': 'p99_ns'}

    max_drift = 0.0
    for pct_key, yaml_key in baseline_keys.items():
        prior = _extract_ns(yaml_key)
        if prior and prior > 0:
            drift = abs(current[pct_key] - prior) / prior * 100.0
            if drift > max_drift:
                max_drift = drift
    return max_drift


def _emit_drift_alert(
    drift_pct: float,
    threshold_pct: float,
    branch: str,
) -> None:
    """Print drift alert to stdout (AC #4 visible signal)."""
    if drift_pct >= threshold_pct:
        print(
            f"WARN: timing-baseline-drift: {drift_pct:.1f}% "
            f"(threshold {threshold_pct:.1f}%) on branch {branch!r}; "
            "not a hard fail — operator review required",
        )


# ------------------------------------------------------------------ #
# AC #6: Fuzz concurrence check                                        #
# ------------------------------------------------------------------ #

def _check_fuzz_concurrence(
    fuzz_csv_path: str,
    gate_rho: float,
) -> None:
    """Compare Spearman ρ from fuzz data against gate-derived ρ (AC #6)."""
    lengths: list[int] = []
    delays: list[int] = []
    try:
        with open(fuzz_csv_path, newline='') as f:
            reader = csv.reader(f, delimiter='\t')
            for row in reader:
                if len(row) < 4:
                    continue
                try:
                    lengths.append(int(row[0]))
                    delays.append(int(row[3]))  # total_ns column
                except (ValueError, IndexError):
                    continue
    except OSError as exc:
        print(f"WARN: fuzz-data unreadable: {exc}", file=sys.stderr)
        return

    if len(lengths) < 100:
        print(
            f"WARN: fuzz-data too few samples ({len(lengths)} < 100); "
            "concurrence check skipped",
            file=sys.stderr,
        )
        return

    fuzz_rho = float(spearmanr(lengths, delays).statistic)
    if abs(gate_rho) < 1e-9:
        print("fuzz-vs-gate concurrence: OK (gate rho near zero)")
        return

    relative_diff = abs(fuzz_rho - gate_rho) / max(abs(gate_rho), 1e-9)
    if relative_diff <= 0.10:
        print(
            f"fuzz-vs-gate concurrence: OK "
            f"(gate_rho={gate_rho:.4f} fuzz_rho={fuzz_rho:.4f})"
        )
    else:
        print(
            f"WARN: fuzz/gate divergence: gate_rho={gate_rho:.4f} "
            f"fuzz_rho={fuzz_rho:.4f} diff={relative_diff*100:.1f}%",
            file=sys.stderr,
        )


# ------------------------------------------------------------------ #
# AC #8: Flake-handling outer loop                                     #
# ------------------------------------------------------------------ #

def _run_with_flake_handling(
    host: str,
    port: int,
    n_samples: int,
    n_buckets: int,
    rng: random.Random,
    thresholds: dict,
    nightly: bool,
) -> tuple:
    """Run gate with borderline-band re-run logic (AC #8).

    Returns (final_run_result, drift_pct, verdict_history_p_values).
    Borderline: first run p ∈ [0.04, 0.06] → 2 additional runs, majority vote.
    """
    p_thresh = float(thresholds.get("mann_whitney_p_threshold", _DEFAULT_P_THRESHOLD))
    total_start_ns = time.perf_counter_ns()
    run_results: list = []
    verdict_history: list = []

    def _check_budget() -> None:
        if not nightly:
            elapsed = time.perf_counter_ns() - total_start_ns
            if elapsed > _TOTAL_BUDGET_NS:
                print(
                    "error: total runtime budget exceeded (>6 min); aborting",
                    file=sys.stderr,
                )
                sys.exit(2)

    # First run
    run_result, drift_pct = _run_measurement(host, port, n_samples, n_buckets, rng)
    run_results.append((run_result, drift_pct))
    verdict_history.append(run_result.p_mann_whitney_min)

    p_first = run_result.p_mann_whitney_min
    is_borderline = _BORDERLINE_LOW <= p_first <= _BORDERLINE_HIGH

    if not nightly and is_borderline:
        # 2 additional re-runs for a total of 3 (AC #8).
        for _ in range(2):
            _check_budget()
            rr, dp = _run_measurement(host, port, n_samples, n_buckets, rng)
            run_results.append((rr, dp))
            verdict_history.append(rr.p_mann_whitney_min)

    # Determine final verdict.
    if len(run_results) > 1:
        # Majority vote: PASS if ≥2/3 runs have p > p_thresh (AC #8).
        passes = sum(
            1 for rr, _ in run_results if rr.p_mann_whitney_min > p_thresh
        )
        final_verdict = "PASS" if passes >= 2 else "FAIL"
        last_rr, last_drift = run_results[-1]
        final_result = RunResult(
            p_mann_whitney_min=last_rr.p_mann_whitney_min,
            rho_spearman=last_rr.rho_spearman,
            samples=last_rr.samples,
            samples_per_bucket=last_rr.samples_per_bucket,
            bucket_boundaries=last_rr.bucket_boundaries,
            runtime_ns=last_rr.runtime_ns,
            verdict=final_verdict,
        )
        return final_result, last_drift, verdict_history
    else:
        rr, drift = run_results[0]
        return rr, drift, verdict_history


# ------------------------------------------------------------------ #
# Output                                                               #
# ------------------------------------------------------------------ #

def _write_output(
    path: str,
    run_result: RunResult,
    drift_pct: float,
    verdict_history: list[float],
    n_samples: int,
) -> None:
    output = {
        "p_mann_whitney_min": run_result.p_mann_whitney_min,
        "rho_spearman": run_result.rho_spearman,
        "verdict": run_result.verdict,
        "samples_per_bucket": run_result.samples_per_bucket,
        "baseline_drift_pct": drift_pct,
        "samples_total": len(run_result.samples),
        "runtime_ms": run_result.runtime_ns // 10 ** 6,
        "verdict_history": verdict_history,
        "bucket_boundaries": [
            {"lo": lo, "hi": hi}
            for lo, hi in run_result.bucket_boundaries
        ],
    }
    text = json.dumps(output, indent=2)
    if path == '-':
        print(text)
    else:
        Path(path).write_text(text + "\n", encoding="utf-8")


# ------------------------------------------------------------------ #
# CLI                                                                  #
# ------------------------------------------------------------------ #

def _build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="AR-C2 timing-invariant CI gate (Mann-Whitney + Spearman).",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    iut = p.add_mutually_exclusive_group(required=False)
    iut.add_argument(
        '--impl', metavar='PATH',
        help="Path to reference dispatcher binary (subprocess mode).",
    )
    iut.add_argument(
        '--endpoint', metavar='HOST:PORT',
        help="Connect to running IUT at HOST:PORT (endpoint mode).",
    )
    p.add_argument(
        '--samples', type=int, default=_DEFAULT_SAMPLES,
        metavar='N',
        help=f"Number of probe samples (default: {_DEFAULT_SAMPLES}).",
    )
    p.add_argument(
        '--output', metavar='FILE', default='results.json',
        help="Write JSON result to FILE ('-' for stdout; default: results.json).",
    )
    p.add_argument(
        '--fuzz-data', metavar='CSV',
        help="Optional TSV (input_len, sha256, parse_ns, total_ns, pid) from fuzz "
             "harness; enables AC #6 concurrence check.",
    )
    p.add_argument(
        '--seed', type=int, default=42,
        help="RNG seed for reproducible probe generation (default: 42).",
    )
    p.add_argument(
        '--nightly', action='store_true',
        help="Nightly mode: no runtime budget enforcement, use N=100000 unless "
             "--samples is explicit.",
    )
    p.add_argument(
        '--branch', default=os.environ.get('GITHUB_REF_NAME', 'unknown'),
        help="Branch name for drift-alert messages (default: $GITHUB_REF_NAME).",
    )
    return p


def main(argv: Optional[list[str]] = None) -> int:
    parser = _build_parser()
    args = parser.parse_args(argv)

    # Nightly default sample count.
    if args.nightly and args.samples == _DEFAULT_SAMPLES:
        args.samples = 100_000

    # Require IUT specification.
    if not args.impl and not args.endpoint:
        print(
            "error: one of --impl PATH or --endpoint HOST:PORT is required",
            file=sys.stderr,
        )
        return 2

    thresholds = _load_thresholds()
    n_buckets = int(thresholds.get("bucket_count", _DEFAULT_BUCKET_COUNT))
    drift_threshold_pct = float(
        thresholds.get("drift_threshold_pct", _DEFAULT_DRIFT_THRESHOLD_PCT)
    )

    rng = random.Random(args.seed)

    try:
        with _iut_context(args.impl, args.endpoint) as (host, port):
            run_result, drift_pct, verdict_history = _run_with_flake_handling(
                host=host,
                port=port,
                n_samples=args.samples,
                n_buckets=n_buckets,
                rng=rng,
                thresholds=thresholds,
                nightly=args.nightly,
            )
    except SetupError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    # AC #9: min-bucket invariant.
    _check_bucket_fill(run_result.samples_per_bucket)

    # AC #4: drift alert.
    _emit_drift_alert(drift_pct, drift_threshold_pct, args.branch)

    # AC #6: fuzz concurrence.
    if args.fuzz_data:
        _check_fuzz_concurrence(args.fuzz_data, run_result.rho_spearman)

    # Write JSON output.
    _write_output(args.output, run_result, drift_pct, verdict_history, args.samples)

    # Print summary.
    verdict_str = run_result.verdict
    p_min = run_result.p_mann_whitney_min
    rho = run_result.rho_spearman
    print(
        f"timing-invariant: {verdict_str} "
        f"(p_min={p_min:.4f} rho={rho:.4f} "
        f"samples={len(run_result.samples)} "
        f"runs={len(verdict_history)})"
    )

    return 0 if run_result.verdict == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
