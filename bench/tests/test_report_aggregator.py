"""
Tests for Story 1a-5: bench report aggregator (report.py).

Covers: warmup exclusion, percentile computation (p50/p95/p99), stddev/mean,
validity gate (VALID/HIGH_VARIANCE/INVALID), acceptance gate (PASS/WARN/FAIL),
and ratio computation (bench_p50 / iperf3_throughput).

Schema: 11-column (post-1a-3 D1 Round-1 review):
  ts_iso, mode, size_bytes, run_index, duration_ms, throughput_mbps,
  upload_mbps, download_mbps, sha256_match, fixture_sha256, error_class
"""

from __future__ import annotations

import csv
import io
import statistics

import pytest

# ---------------------------------------------------------------------------
# Helpers — realistic inline CSV data (11-column schema)
# ---------------------------------------------------------------------------

CSV_HEADER = (
    "ts_iso,mode,size_bytes,run_index,duration_ms,throughput_mbps,"
    "upload_mbps,download_mbps,sha256_match,fixture_sha256,error_class"
)


def _make_csv(rows: list[tuple]) -> str:
    """Build a CSV string from a list of 11-column row tuples.

    Each tuple: (ts_iso, mode, size_bytes, run_index, duration_ms,
                 throughput_mbps, upload_mbps, download_mbps,
                 sha256_match, fixture_sha256, error_class)
    """
    buf = io.StringIO()
    writer = csv.writer(buf)
    writer.writerow(CSV_HEADER.split(","))
    for row in rows:
        writer.writerow(row)
    return buf.getvalue()


# -- 11-run echo cell: 1 warmup (idx=0) + 10 valid (idx=1..10)
# Throughput values carefully chosen for verifiable percentile results.
# Sorted valid values (idx 1-10): 8.0, 9.0, 9.5, 10.0, 10.2, 10.5, 10.8, 11.0, 11.5, 12.0
ECHO_1MB_RUNS = [
    # warmup: throughput=7.5 (must NOT appear in valid-run stats)
    ("2026-05-06T14:00:00.000Z", "echo", 1048576, 0, 900.0, 7.5, "na", "na", "true", "na", "ok"),
    ("2026-05-06T14:00:10.000Z", "echo", 1048576, 1, 850.0, 10.0, "na", "na", "true", "na", "ok"),
    ("2026-05-06T14:00:20.000Z", "echo", 1048576, 2, 830.0, 10.5, "na", "na", "true", "na", "ok"),
    ("2026-05-06T14:00:30.000Z", "echo", 1048576, 3, 870.0, 9.5, "na", "na", "true", "na", "ok"),
    ("2026-05-06T14:00:40.000Z", "echo", 1048576, 4, 810.0, 11.0, "na", "na", "true", "na", "ok"),
    ("2026-05-06T14:00:50.000Z", "echo", 1048576, 5, 890.0, 8.0, "na", "na", "true", "na", "ok"),
    ("2026-05-06T14:01:00.000Z", "echo", 1048576, 6, 820.0, 10.8, "na", "na", "true", "na", "ok"),
    ("2026-05-06T14:01:10.000Z", "echo", 1048576, 7, 860.0, 9.0, "na", "na", "true", "na", "ok"),
    ("2026-05-06T14:01:20.000Z", "echo", 1048576, 8, 800.0, 11.5, "na", "na", "true", "na", "ok"),
    ("2026-05-06T14:01:30.000Z", "echo", 1048576, 9, 825.0, 10.2, "na", "na", "true", "na", "ok"),
    ("2026-05-06T14:01:40.000Z", "echo", 1048576, 10, 790.0, 12.0, "na", "na", "true", "na", "ok"),
]

# -- High-variance cell: stddev/median >= 0.5
# Valid throughputs (idx 1-10): [2.0, 3.0, 50.0, 2.5, 55.0, 3.5, 48.0, 4.0, 52.0, 60.0]
# Sorted: [2.0, 2.5, 3.0, 3.5, 4.0, 48.0, 50.0, 52.0, 55.0, 60.0]
# median=(4.0+48.0)/2=26.0, stddev≈25.7, ratio≈0.99 >> 0.5
HIGH_VARIANCE_RUNS = [
    ("2026-05-06T15:00:00.000Z", "sink", 10485760, 0, 500.0, 5.0, "na", "na", "na", "na", "ok"),  # warmup
    ("2026-05-06T15:00:10.000Z", "sink", 10485760, 1, 400.0, 2.0, "na", "na", "na", "na", "ok"),
    ("2026-05-06T15:00:20.000Z", "sink", 10485760, 2, 410.0, 3.0, "na", "na", "na", "na", "ok"),
    ("2026-05-06T15:00:30.000Z", "sink", 10485760, 3, 420.0, 50.0, "na", "na", "na", "na", "ok"),
    ("2026-05-06T15:00:40.000Z", "sink", 10485760, 4, 430.0, 2.5, "na", "na", "na", "na", "ok"),
    ("2026-05-06T15:00:50.000Z", "sink", 10485760, 5, 440.0, 55.0, "na", "na", "na", "na", "ok"),
    ("2026-05-06T15:01:00.000Z", "sink", 10485760, 6, 450.0, 3.5, "na", "na", "na", "na", "ok"),
    ("2026-05-06T15:01:10.000Z", "sink", 10485760, 7, 460.0, 48.0, "na", "na", "na", "na", "ok"),
    ("2026-05-06T15:01:20.000Z", "sink", 10485760, 8, 470.0, 4.0, "na", "na", "na", "na", "ok"),
    ("2026-05-06T15:01:30.000Z", "sink", 10485760, 9, 480.0, 52.0, "na", "na", "na", "na", "ok"),
    ("2026-05-06T15:01:40.000Z", "sink", 10485760, 10, 490.0, 60.0, "na", "na", "na", "na", "ok"),
]

# -- Too few runs (only 5 valid after warmup excluded)
LOW_N_RUNS = [
    ("2026-05-06T16:00:00.000Z", "source", 52428800, 0, 5000.0, 80.0, "na", "na", "na", "na", "ok"),  # warmup
    ("2026-05-06T16:00:10.000Z", "source", 52428800, 1, 4800.0, 85.0, "na", "na", "na", "na", "ok"),
    ("2026-05-06T16:00:20.000Z", "source", 52428800, 2, 4900.0, 82.0, "na", "na", "na", "na", "ok"),
    ("2026-05-06T16:00:30.000Z", "source", 52428800, 3, 4700.0, 87.0, "na", "na", "na", "na", "ok"),
    ("2026-05-06T16:00:40.000Z", "source", 52428800, 4, 4850.0, 83.0, "na", "na", "na", "na", "ok"),
    ("2026-05-06T16:00:50.000Z", "source", 52428800, 5, 4950.0, 81.0, "na", "na", "na", "na", "ok"),
]

# -- Echo cell with SHA-256 mismatches (3 out of 10 valid runs = 30% rate)
SHA256_MISMATCH_RUNS = [
    ("2026-05-06T17:00:00.000Z", "echo", 1048576, 0, 850.0, 10.0, "na", "na", "true", "na", "ok"),   # warmup
    ("2026-05-06T17:00:10.000Z", "echo", 1048576, 1, 850.0, 10.0, "na", "na", "true", "na", "ok"),
    ("2026-05-06T17:00:20.000Z", "echo", 1048576, 2, 850.0, 10.0, "na", "na", "false", "na", "corruption"),
    ("2026-05-06T17:00:30.000Z", "echo", 1048576, 3, 850.0, 10.0, "na", "na", "true", "na", "ok"),
    ("2026-05-06T17:00:40.000Z", "echo", 1048576, 4, 850.0, 10.0, "na", "na", "true", "na", "ok"),
    ("2026-05-06T17:00:50.000Z", "echo", 1048576, 5, 850.0, 10.0, "na", "na", "false", "na", "corruption"),
    ("2026-05-06T17:01:00.000Z", "echo", 1048576, 6, 850.0, 10.0, "na", "na", "true", "na", "ok"),
    ("2026-05-06T17:01:10.000Z", "echo", 1048576, 7, 850.0, 10.0, "na", "na", "true", "na", "ok"),
    ("2026-05-06T17:01:20.000Z", "echo", 1048576, 8, 850.0, 10.0, "na", "na", "false", "na", "corruption"),
    ("2026-05-06T17:01:30.000Z", "echo", 1048576, 9, 850.0, 10.0, "na", "na", "true", "na", "ok"),
    ("2026-05-06T17:01:40.000Z", "echo", 1048576, 10, 850.0, 10.0, "na", "na", "true", "na", "ok"),
]


# ---------------------------------------------------------------------------
# AC#3: Warmup (run_index=0) excluded from stats
# ---------------------------------------------------------------------------


def test_warmup_excluded():
    """AC#3: rows with run_index=0 are excluded from aggregated statistics."""
    from teleproto3.bench.report import aggregate_cell

    csv_text = _make_csv(ECHO_1MB_RUNS)
    cell = aggregate_cell(csv_text, mode="echo", size_bytes=1048576)

    assert cell["n_valid"] == 10
    assert 7.5 not in cell["throughput_values"]


# ---------------------------------------------------------------------------
# AC#5: Percentile computations — p50, p95, p99
# ---------------------------------------------------------------------------
# Valid throughputs (sorted): 8.0, 9.0, 9.5, 10.0, 10.2, 10.5, 10.8, 11.0, 11.5, 12.0


def test_p50_computation():
    """AC#5: p50 (median) is correct for known throughput data."""
    from teleproto3.bench.report import aggregate_cell

    csv_text = _make_csv(ECHO_1MB_RUNS)
    # p50 of 10 values: average of 5th and 6th = (10.2 + 10.5) / 2 = 10.35
    expected_p50 = 10.35

    cell = aggregate_cell(csv_text, mode="echo", size_bytes=1048576)

    assert abs(cell["p50"] - expected_p50) < 0.01


def test_p95_computation():
    """AC#5: p95 is correct for known throughput data (P21: inclusive method)."""
    from teleproto3.bench.report import aggregate_cell

    csv_text = _make_csv(ECHO_1MB_RUNS)
    sorted_values = [8.0, 9.0, 9.5, 10.0, 10.2, 10.5, 10.8, 11.0, 11.5, 12.0]
    expected_p95 = statistics.quantiles(sorted_values, n=100, method="inclusive")[94]

    cell = aggregate_cell(csv_text, mode="echo", size_bytes=1048576)

    assert abs(cell["p95"] - expected_p95) < 0.1


def test_p99_computation():
    """AC#5: p99 is correct for known throughput data (P21: inclusive method)."""
    from teleproto3.bench.report import aggregate_cell

    csv_text = _make_csv(ECHO_1MB_RUNS)
    sorted_values = [8.0, 9.0, 9.5, 10.0, 10.2, 10.5, 10.8, 11.0, 11.5, 12.0]
    expected_p99 = statistics.quantiles(sorted_values, n=100, method="inclusive")[98]

    cell = aggregate_cell(csv_text, mode="echo", size_bytes=1048576)

    assert abs(cell["p99"] - expected_p99) < 0.1


# ---------------------------------------------------------------------------
# AC#5: stddev and mean computation
# ---------------------------------------------------------------------------


def test_stddev_mean():
    """AC#5: stddev and mean are correct for known throughput data."""
    from teleproto3.bench.report import aggregate_cell

    csv_text = _make_csv(ECHO_1MB_RUNS)
    sorted_values = [8.0, 9.0, 9.5, 10.0, 10.2, 10.5, 10.8, 11.0, 11.5, 12.0]
    expected_mean = statistics.mean(sorted_values)
    expected_stddev = statistics.stdev(sorted_values)

    cell = aggregate_cell(csv_text, mode="echo", size_bytes=1048576)

    assert abs(cell["mean"] - expected_mean) < 0.01
    assert abs(cell["stddev"] - expected_stddev) < 0.01


# ---------------------------------------------------------------------------
# AC#6: Validity gate — VALID
# ---------------------------------------------------------------------------


def test_validity_valid():
    """AC#6: n>=10, sha256 match rate=1.0, stddev/median<0.5 produces VALID."""
    from teleproto3.bench.report import compute_validity

    csv_text = _make_csv(ECHO_1MB_RUNS)
    validity = compute_validity(csv_text, mode="echo", size_bytes=1048576)

    assert validity == "VALID"


# ---------------------------------------------------------------------------
# AC#6: Validity gate — HIGH_VARIANCE
# ---------------------------------------------------------------------------


def test_validity_high_variance():
    """AC#6: stddev/median >= 0.5 produces HIGH_VARIANCE."""
    from teleproto3.bench.report import compute_validity

    csv_text = _make_csv(HIGH_VARIANCE_RUNS)
    # Valid throughputs (idx 1-10): [2.0, 3.0, 50.0, 2.5, 55.0, 3.5, 48.0, 4.0, 52.0, 60.0]
    # median=26.0, stddev≈25.7 → ratio≈0.99 >> 0.5

    validity = compute_validity(csv_text, mode="sink", size_bytes=10485760)

    assert validity == "HIGH_VARIANCE"


# ---------------------------------------------------------------------------
# AC#6: Validity gate — INVALID (low n)
# ---------------------------------------------------------------------------


def test_validity_invalid_low_n():
    """AC#6: n_valid < 10 produces INVALID."""
    from teleproto3.bench.report import compute_validity

    csv_text = _make_csv(LOW_N_RUNS)
    validity = compute_validity(csv_text, mode="source", size_bytes=52428800)

    assert validity == "INVALID"


# ---------------------------------------------------------------------------
# AC#6: Validity gate — INVALID (SHA-256 mismatch rate > 10%)
# ---------------------------------------------------------------------------


def test_validity_invalid_sha256_mismatch():
    """AC#6 (P2): ANY sha256 mismatch in echo mode produces INVALID (rate must be == 1.0)."""
    from teleproto3.bench.report import compute_validity

    # SHA256_MISMATCH_RUNS: 3 out of 10 valid runs have sha256=false (30%)
    csv_text = _make_csv(SHA256_MISMATCH_RUNS)
    validity = compute_validity(csv_text, mode="echo", size_bytes=1048576)

    assert validity == "INVALID"


def test_validity_invalid_sha256_single_mismatch():
    """AC#6 (P2): even ONE sha256 mismatch in echo mode produces INVALID."""
    from teleproto3.bench.report import compute_validity

    # 1 out of 10 valid runs has sha256=false (10% rate, but spec demands 1.0)
    rows = [
        ("2026-05-07T18:00:00.000Z", "echo", 1048576, 0, 850.0, 10.0, "na", "na", "true", "na", "ok"),  # warmup
    ] + [
        ("2026-05-07T18:00:10.000Z", "echo", 1048576, i, 850.0, 10.0, "na", "na", "true", "na", "ok")
        for i in range(1, 10)
    ] + [
        ("2026-05-07T18:01:30.000Z", "echo", 1048576, 10, 850.0, 10.0, "na", "na", "false", "na", "corruption"),
    ]
    csv_text = _make_csv(rows)
    validity = compute_validity(csv_text, mode="echo", size_bytes=1048576)

    assert validity == "INVALID"


# ---------------------------------------------------------------------------
# AC#7: Acceptance gate — PASS
# ---------------------------------------------------------------------------


def test_acceptance_gate_pass():
    """AC#7 (P19): all cells VALID produces PASS (ratio is informational, not gate)."""
    from teleproto3.bench.report import compute_acceptance_gate

    cells = [
        {"mode": "echo", "size_bytes": 1048576, "validity": "VALID", "n_valid": 10, "ratio": 0.85},
        {"mode": "sink", "size_bytes": 1048576, "validity": "VALID", "n_valid": 10, "ratio": 0.92},
        {"mode": "source", "size_bytes": 1048576, "validity": "VALID", "n_valid": 10, "ratio": 0.78},
        {"mode": "echo", "size_bytes": 10485760, "validity": "VALID", "n_valid": 10, "ratio": 0.80},
        {"mode": "sink", "size_bytes": 10485760, "validity": "VALID", "n_valid": 10, "ratio": 0.88},
        {"mode": "source", "size_bytes": 10485760, "validity": "VALID", "n_valid": 10, "ratio": 0.75},
        {"mode": "echo", "size_bytes": 52428800, "validity": "VALID", "n_valid": 10, "ratio": 0.72},
        {"mode": "sink", "size_bytes": 52428800, "validity": "VALID", "n_valid": 10, "ratio": 0.90},
        {"mode": "source", "size_bytes": 52428800, "validity": "VALID", "n_valid": 10, "ratio": 0.71},
    ]

    gate = compute_acceptance_gate(cells)

    assert gate == "PASS"


def test_acceptance_gate_pass_ignores_low_ratio():
    """AC#7 (P19): low ratio does NOT trigger FAIL — ratio is out of gate."""
    from teleproto3.bench.report import compute_acceptance_gate

    # All cells VALID but ratios below the old 0.30 floor.
    # Under P19 (D5): gate is validity-driven only -> PASS.
    cells = [
        {"mode": "echo", "size_bytes": 1048576, "validity": "VALID", "n_valid": 10, "ratio": 0.05},
        {"mode": "sink", "size_bytes": 1048576, "validity": "VALID", "n_valid": 10, "ratio": 0.08},
    ]

    assert compute_acceptance_gate(cells) == "PASS"


# ---------------------------------------------------------------------------
# AC#7: Acceptance gate — WARN
# ---------------------------------------------------------------------------


def test_acceptance_gate_warn():
    """AC#7 (P19): any HIGH_VARIANCE cell with no INVALID -> WARN."""
    from teleproto3.bench.report import compute_acceptance_gate

    cells = [
        {"mode": "echo", "size_bytes": 1048576, "validity": "VALID", "n_valid": 10, "ratio": 0.85},
        {"mode": "sink", "size_bytes": 1048576, "validity": "VALID", "n_valid": 10, "ratio": 0.55},
        {"mode": "source", "size_bytes": 1048576, "validity": "HIGH_VARIANCE", "n_valid": 10, "ratio": 0.42},
        {"mode": "echo", "size_bytes": 10485760, "validity": "VALID", "n_valid": 10, "ratio": 0.80},
        {"mode": "sink", "size_bytes": 10485760, "validity": "VALID", "n_valid": 10, "ratio": 0.72},
        {"mode": "source", "size_bytes": 10485760, "validity": "HIGH_VARIANCE", "n_valid": 10, "ratio": 0.65},
        {"mode": "echo", "size_bytes": 52428800, "validity": "VALID", "n_valid": 10, "ratio": 0.70},
        {"mode": "sink", "size_bytes": 52428800, "validity": "VALID", "n_valid": 10, "ratio": 0.88},
        {"mode": "source", "size_bytes": 52428800, "validity": "VALID", "n_valid": 10, "ratio": 0.75},
    ]

    gate = compute_acceptance_gate(cells)

    assert gate == "WARN"


# ---------------------------------------------------------------------------
# AC#7: Acceptance gate — FAIL
# ---------------------------------------------------------------------------


def test_acceptance_gate_fail():
    """AC#7 (P19): any INVALID cell -> FAIL."""
    from teleproto3.bench.report import compute_acceptance_gate

    cells = [
        {"mode": "echo", "size_bytes": 1048576, "validity": "VALID", "n_valid": 10, "ratio": 0.85},
        {"mode": "sink", "size_bytes": 1048576, "validity": "VALID", "n_valid": 10, "ratio": 0.22},
        {"mode": "source", "size_bytes": 1048576, "validity": "INVALID", "n_valid": 5, "ratio": 0.0},
        {"mode": "echo", "size_bytes": 10485760, "validity": "VALID", "n_valid": 10, "ratio": 0.80},
        {"mode": "sink", "size_bytes": 10485760, "validity": "VALID", "n_valid": 10, "ratio": 0.72},
        {"mode": "source", "size_bytes": 10485760, "validity": "VALID", "n_valid": 10, "ratio": 0.65},
        {"mode": "echo", "size_bytes": 52428800, "validity": "VALID", "n_valid": 10, "ratio": 0.70},
        {"mode": "sink", "size_bytes": 52428800, "validity": "VALID", "n_valid": 10, "ratio": 0.88},
        {"mode": "source", "size_bytes": 52428800, "validity": "VALID", "n_valid": 10, "ratio": 0.75},
    ]

    gate = compute_acceptance_gate(cells)

    assert gate == "FAIL"


def test_acceptance_gate_smoke_indeterminate():
    """AC#7 / P15 / D1: smoke=True -> INDETERMINATE regardless of validity."""
    from teleproto3.bench.report import compute_acceptance_gate

    cells = [
        {"mode": "sink", "size_bytes": 1048576, "validity": "INVALID", "n_valid": 2},
        {"mode": "echo", "size_bytes": 1048576, "validity": "INVALID", "n_valid": 2},
        {"mode": "source", "size_bytes": 1048576, "validity": "INVALID", "n_valid": 2},
    ]

    assert compute_acceptance_gate(cells, smoke=True) == "INDETERMINATE"


def test_acceptance_gate_empty_input_is_fail():
    """P12: empty cell list -> FAIL (avoid vacuous PASS)."""
    from teleproto3.bench.report import compute_acceptance_gate

    assert compute_acceptance_gate([]) == "FAIL"


def test_acceptance_gate_zero_data_is_fail():
    """P12: cells with all n_valid=0 -> FAIL (no data to assess)."""
    from teleproto3.bench.report import compute_acceptance_gate

    cells = [
        {"mode": "sink", "size_bytes": 1048576, "validity": "VALID", "n_valid": 0},
        {"mode": "echo", "size_bytes": 1048576, "validity": "VALID", "n_valid": 0},
    ]

    assert compute_acceptance_gate(cells) == "FAIL"


# ---------------------------------------------------------------------------
# AC#5: ratio computation — bench_p50 / iperf3_throughput
# ---------------------------------------------------------------------------


def test_ratio_computation():
    """AC#5: ratio = bench_p50 / iperf3_throughput_mbps computed correctly."""
    from teleproto3.bench.report import compute_ratio

    bench_p50_mbps = 10.35
    iperf3_throughput_mbps = 14.8

    ratio = compute_ratio(bench_p50_mbps, iperf3_throughput_mbps)

    expected = 10.35 / 14.8  # ~0.699
    assert ratio is not None
    assert abs(ratio - expected) < 0.001
    assert 0.0 < ratio <= 1.0


def test_ratio_returns_none_when_iperf3_missing():
    """P18 / D4: ratio is None when iperf3 baseline absent (drops 0.0 sentinel)."""
    from teleproto3.bench.report import compute_ratio

    assert compute_ratio(10.35, None) is None
    assert compute_ratio(10.35, 0.0) is None


def test_echo_asymmetry_aggregation():
    """P20 / D6: aggregate_cell exposes upload_p50/download_p50 for echo cells."""
    from teleproto3.bench.report import aggregate_cell

    rows = [
        ("2026-05-07T12:00:00.000Z", "echo", 1048576, 0, 850.0, 10.0, "8.0", "12.0", "true", "na", "ok"),  # warmup
    ] + [
        (f"2026-05-07T12:00:{10+i:02d}.000Z", "echo", 1048576, i, 850.0, 10.0, "8.0", "12.0", "true", "na", "ok")
        for i in range(1, 11)
    ]
    csv_text = _make_csv(rows)
    cell = aggregate_cell(csv_text, mode="echo", size_bytes=1048576)

    assert cell["upload_p50"] == 8.0
    assert cell["download_p50"] == 12.0


def test_malformed_run_index_is_skipped():
    """P5: rows with empty/non-numeric run_index do not crash _valid_rows."""
    from teleproto3.bench.report import aggregate_cell

    rows = [
        ("2026-05-07T12:00:00.000Z", "echo", 1048576, 0, 850.0, 10.0, "na", "na", "true", "na", "ok"),  # warmup
        ("BAD_ROW", "echo", 1048576, "", 850.0, 10.0, "na", "na", "true", "na", "ok"),  # malformed run_index
        ("2026-05-07T12:00:10.000Z", "echo", 1048576, 1, 850.0, 10.0, "na", "na", "true", "na", "ok"),
    ]
    csv_text = _make_csv(rows)
    cell = aggregate_cell(csv_text, mode="echo", size_bytes=1048576)

    # warmup excluded, malformed skipped, only run_index=1 counted
    assert cell["n_valid"] == 1


def test_smoke_mode_emits_indeterminate_via_main_path():
    """P15 / D1: generate_summary_md(smoke=True) emits INDETERMINATE."""
    from teleproto3.bench.report import generate_summary_md
    import tempfile
    import json as _json
    import os

    csv_text = _make_csv(
        [
            ("2026-05-07T12:00:00.000Z", "sink", 1048576, 0, 500.0, 10.0, "na", "na", "na", "na", "ok"),
            ("2026-05-07T12:00:10.000Z", "sink", 1048576, 1, 500.0, 10.0, "na", "na", "na", "na", "ok"),
            ("2026-05-07T12:00:20.000Z", "sink", 1048576, 2, 500.0, 10.0, "na", "na", "na", "na", "ok"),
        ]
    )
    with tempfile.TemporaryDirectory() as td:
        csv_path = os.path.join(td, "runs.csv")
        iperf3_path = os.path.join(td, "iperf3.json")
        with open(csv_path, "w") as f:
            f.write(csv_text)
        with open(iperf3_path, "w") as f:
            _json.dump({}, f)

        md, gate = generate_summary_md(
            csv_path, iperf3_path, sizes=[1048576], modes=["sink"], smoke=True
        )

    assert gate == "INDETERMINATE"
    assert "INDETERMINATE" in md
    assert "smoke mode" in md
