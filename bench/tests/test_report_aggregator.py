"""
RED-phase tests for Story 1a-5: bench report aggregator (report.py).

Covers: warmup exclusion, percentile computation (p50/p95/p99), stddev/mean,
validity gate (VALID/HIGH_VARIANCE/INVALID), acceptance gate (PASS/WARN/FAIL),
and ratio computation (bench_p50 / iperf3_throughput).

All tests are skip-decorated until the report.py implementation exists.
"""

from __future__ import annotations

import csv
import io
import math
import statistics

import pytest

# ---------------------------------------------------------------------------
# Helpers — realistic inline CSV data
# ---------------------------------------------------------------------------

# NOTE 2026-05-07 (Story 1a-3 D1): post-Round-1 schema is 11-column with
# ttfb_ms removed and upload_mbps/download_mbps/fixture_sha256 added. This
# RED-phase scaffold's sample row tuples remain 9-column (legacy 1a-3 schema)
# pending the 1a-5 implementation pass — the aggregator will rewrite these.
# DO NOT use this file's data shape as the schema authority; see
# bench_client.py CSV_COLUMNS for the live 11-column contract.
CSV_HEADER = (
    "ts_iso,mode,size_bytes,run_index,duration_ms,throughput_mbps,"
    "upload_mbps,download_mbps,sha256_match,fixture_sha256,error_class"
)


def _make_csv(rows: list[tuple]) -> str:
    """Build a CSV string from a list of row tuples.

    Each tuple is the LEGACY 9-column shape (pre-D1):
        (ts_iso, mode, size_bytes, run_index, ttfb_ms,
         duration_ms, throughput_mbps, sha256_match, error_class)

    1a-5 implementation will migrate these to the post-D1 11-column shape.
    """
    buf = io.StringIO()
    writer = csv.writer(buf)
    writer.writerow(CSV_HEADER.split(","))
    for row in rows:
        writer.writerow(row)
    return buf.getvalue()


# -- 11-run echo cell: 1 warmup (idx=0) + 10 valid (idx=1..10)
# Throughput values are carefully chosen for verifiable percentile results.
# Sorted values (idx 1-10): 8.0, 9.0, 9.5, 10.0, 10.2, 10.5, 10.8, 11.0, 11.5, 12.0
ECHO_1MB_RUNS = [
    (
        "2026-05-06T14:00:00.000Z",
        "echo",
        1048576,
        0,
        15.0,
        900.0,
        7.5,
        "true",
        "ok",
    ),  # warmup
    ("2026-05-06T14:00:10.000Z", "echo", 1048576, 1, 12.0, 850.0, 10.0, "true", "ok"),
    ("2026-05-06T14:00:20.000Z", "echo", 1048576, 2, 11.5, 830.0, 10.5, "true", "ok"),
    ("2026-05-06T14:00:30.000Z", "echo", 1048576, 3, 13.0, 870.0, 9.5, "true", "ok"),
    ("2026-05-06T14:00:40.000Z", "echo", 1048576, 4, 10.8, 810.0, 11.0, "true", "ok"),
    ("2026-05-06T14:00:50.000Z", "echo", 1048576, 5, 14.0, 890.0, 8.0, "true", "ok"),
    ("2026-05-06T14:01:00.000Z", "echo", 1048576, 6, 11.0, 820.0, 10.8, "true", "ok"),
    ("2026-05-06T14:01:10.000Z", "echo", 1048576, 7, 12.5, 860.0, 9.0, "true", "ok"),
    ("2026-05-06T14:01:20.000Z", "echo", 1048576, 8, 10.5, 800.0, 11.5, "true", "ok"),
    ("2026-05-06T14:01:30.000Z", "echo", 1048576, 9, 11.2, 825.0, 10.2, "true", "ok"),
    ("2026-05-06T14:01:40.000Z", "echo", 1048576, 10, 10.0, 790.0, 12.0, "true", "ok"),
]

# -- High-variance cell: stddev/median >= 0.5
HIGH_VARIANCE_RUNS = [
    (
        "2026-05-06T15:00:00.000Z",
        "sink",
        10485760,
        0,
        8.0,
        500.0,
        5.0,
        "na",
        "ok",
    ),  # warmup
    ("2026-05-06T15:00:10.000Z", "sink", 10485760, 1, 8.0, 400.0, 2.0, "na", "ok"),
    ("2026-05-06T15:00:20.000Z", "sink", 10485760, 2, 8.0, 410.0, 3.0, "na", "ok"),
    ("2026-05-06T15:00:30.000Z", "sink", 10485760, 3, 8.0, 420.0, 50.0, "na", "ok"),
    ("2026-05-06T15:00:40.000Z", "sink", 10485760, 4, 8.0, 430.0, 2.5, "na", "ok"),
    ("2026-05-06T15:00:50.000Z", "sink", 10485760, 5, 8.0, 440.0, 55.0, "na", "ok"),
    ("2026-05-06T15:01:00.000Z", "sink", 10485760, 6, 8.0, 450.0, 3.5, "na", "ok"),
    ("2026-05-06T15:01:10.000Z", "sink", 10485760, 7, 8.0, 460.0, 48.0, "na", "ok"),
    ("2026-05-06T15:01:20.000Z", "sink", 10485760, 8, 8.0, 470.0, 4.0, "na", "ok"),
    ("2026-05-06T15:01:30.000Z", "sink", 10485760, 9, 8.0, 480.0, 52.0, "na", "ok"),
    ("2026-05-06T15:01:40.000Z", "sink", 10485760, 10, 8.0, 490.0, 60.0, "na", "ok"),
]

# -- Too few runs (only 5 valid after warmup excluded)
LOW_N_RUNS = [
    (
        "2026-05-06T16:00:00.000Z",
        "source",
        52428800,
        0,
        20.0,
        5000.0,
        80.0,
        "na",
        "ok",
    ),  # warmup
    ("2026-05-06T16:00:10.000Z", "source", 52428800, 1, 18.0, 4800.0, 85.0, "na", "ok"),
    ("2026-05-06T16:00:20.000Z", "source", 52428800, 2, 19.0, 4900.0, 82.0, "na", "ok"),
    ("2026-05-06T16:00:30.000Z", "source", 52428800, 3, 17.0, 4700.0, 87.0, "na", "ok"),
    ("2026-05-06T16:00:40.000Z", "source", 52428800, 4, 18.5, 4850.0, 83.0, "na", "ok"),
    ("2026-05-06T16:00:50.000Z", "source", 52428800, 5, 19.5, 4950.0, 81.0, "na", "ok"),
]

# -- Echo cell with SHA-256 mismatches (3 out of 10 valid runs)
SHA256_MISMATCH_RUNS = [
    (
        "2026-05-06T17:00:00.000Z",
        "echo",
        1048576,
        0,
        12.0,
        850.0,
        10.0,
        "true",
        "ok",
    ),  # warmup
    ("2026-05-06T17:00:10.000Z", "echo", 1048576, 1, 12.0, 850.0, 10.0, "true", "ok"),
    (
        "2026-05-06T17:00:20.000Z",
        "echo",
        1048576,
        2,
        12.0,
        850.0,
        10.0,
        "false",
        "corruption",
    ),
    ("2026-05-06T17:00:30.000Z", "echo", 1048576, 3, 12.0, 850.0, 10.0, "true", "ok"),
    ("2026-05-06T17:00:40.000Z", "echo", 1048576, 4, 12.0, 850.0, 10.0, "true", "ok"),
    (
        "2026-05-06T17:00:50.000Z",
        "echo",
        1048576,
        5,
        12.0,
        850.0,
        10.0,
        "false",
        "corruption",
    ),
    ("2026-05-06T17:01:00.000Z", "echo", 1048576, 6, 12.0, 850.0, 10.0, "true", "ok"),
    ("2026-05-06T17:01:10.000Z", "echo", 1048576, 7, 12.0, 850.0, 10.0, "true", "ok"),
    (
        "2026-05-06T17:01:20.000Z",
        "echo",
        1048576,
        8,
        12.0,
        850.0,
        10.0,
        "false",
        "corruption",
    ),
    ("2026-05-06T17:01:30.000Z", "echo", 1048576, 9, 12.0, 850.0, 10.0, "true", "ok"),
    ("2026-05-06T17:01:40.000Z", "echo", 1048576, 10, 12.0, 850.0, 10.0, "true", "ok"),
]


# ---------------------------------------------------------------------------
# AC#3: Warmup (run_index=0) excluded from stats
# ---------------------------------------------------------------------------


@pytest.mark.skip(reason="RED PHASE: report aggregator not yet implemented")
def test_warmup_excluded():
    """AC#3: rows with run_index=0 are excluded from aggregated statistics."""
    from teleproto3.bench.report import aggregate_cell

    # Arrange
    csv_text = _make_csv(ECHO_1MB_RUNS)

    # Act
    cell = aggregate_cell(csv_text, mode="echo", size_bytes=1048576)

    # Assert — 10 valid runs (warmup excluded), not 11
    assert cell["n_valid"] == 10
    # Warmup throughput (7.5) should not appear in the data
    assert 7.5 not in cell["throughput_values"]


# ---------------------------------------------------------------------------
# AC#5: Percentile computations — p50, p95, p99
# ---------------------------------------------------------------------------
# Valid throughputs (sorted): 8.0, 9.0, 9.5, 10.0, 10.2, 10.5, 10.8, 11.0, 11.5, 12.0


@pytest.mark.skip(reason="RED PHASE: report aggregator not yet implemented")
def test_p50_computation():
    """AC#5: p50 (median) is correct for known throughput data."""
    from teleproto3.bench.report import aggregate_cell

    # Arrange
    csv_text = _make_csv(ECHO_1MB_RUNS)
    # Sorted valid throughputs: [8.0, 9.0, 9.5, 10.0, 10.2, 10.5, 10.8, 11.0, 11.5, 12.0]
    # p50 of 10 values: average of 5th and 6th = (10.2 + 10.5) / 2 = 10.35
    expected_p50 = 10.35

    # Act
    cell = aggregate_cell(csv_text, mode="echo", size_bytes=1048576)

    # Assert
    assert abs(cell["p50"] - expected_p50) < 0.01


@pytest.mark.skip(reason="RED PHASE: report aggregator not yet implemented")
def test_p95_computation():
    """AC#5: p95 is correct for known throughput data."""
    from teleproto3.bench.report import aggregate_cell

    # Arrange
    csv_text = _make_csv(ECHO_1MB_RUNS)
    # Using Python statistics module convention for p95 of 10 values:
    sorted_values = [8.0, 9.0, 9.5, 10.0, 10.2, 10.5, 10.8, 11.0, 11.5, 12.0]
    # statistics.quantiles with n=100 gives interpolated result
    expected_p95 = statistics.quantiles(sorted_values, n=100)[
        94
    ]  # 0-indexed: 94th = p95

    # Act
    cell = aggregate_cell(csv_text, mode="echo", size_bytes=1048576)

    # Assert
    assert abs(cell["p95"] - expected_p95) < 0.1


@pytest.mark.skip(reason="RED PHASE: report aggregator not yet implemented")
def test_p99_computation():
    """AC#5: p99 is correct for known throughput data."""
    from teleproto3.bench.report import aggregate_cell

    # Arrange
    csv_text = _make_csv(ECHO_1MB_RUNS)
    sorted_values = [8.0, 9.0, 9.5, 10.0, 10.2, 10.5, 10.8, 11.0, 11.5, 12.0]
    expected_p99 = statistics.quantiles(sorted_values, n=100)[
        98
    ]  # 0-indexed: 98th = p99

    # Act
    cell = aggregate_cell(csv_text, mode="echo", size_bytes=1048576)

    # Assert
    assert abs(cell["p99"] - expected_p99) < 0.1


# ---------------------------------------------------------------------------
# AC#5: stddev and mean computation
# ---------------------------------------------------------------------------


@pytest.mark.skip(reason="RED PHASE: report aggregator not yet implemented")
def test_stddev_mean():
    """AC#5: stddev and mean are correct for known throughput data."""
    from teleproto3.bench.report import aggregate_cell

    # Arrange
    csv_text = _make_csv(ECHO_1MB_RUNS)
    sorted_values = [8.0, 9.0, 9.5, 10.0, 10.2, 10.5, 10.8, 11.0, 11.5, 12.0]
    expected_mean = statistics.mean(sorted_values)  # 10.25
    expected_stddev = statistics.stdev(sorted_values)  # ~1.179

    # Act
    cell = aggregate_cell(csv_text, mode="echo", size_bytes=1048576)

    # Assert
    assert abs(cell["mean"] - expected_mean) < 0.01
    assert abs(cell["stddev"] - expected_stddev) < 0.01


# ---------------------------------------------------------------------------
# AC#6: Validity gate — VALID
# ---------------------------------------------------------------------------


@pytest.mark.skip(reason="RED PHASE: report aggregator not yet implemented")
def test_validity_valid():
    """AC#6: n>=10, sha256 match rate=1.0, stddev/median<0.5 produces VALID."""
    from teleproto3.bench.report import compute_validity

    # Arrange — ECHO_1MB_RUNS: 10 valid, all sha256=true, low variance
    csv_text = _make_csv(ECHO_1MB_RUNS)

    # Act
    validity = compute_validity(csv_text, mode="echo", size_bytes=1048576)

    # Assert
    assert validity == "VALID"


# ---------------------------------------------------------------------------
# AC#6: Validity gate — HIGH_VARIANCE
# ---------------------------------------------------------------------------


@pytest.mark.skip(reason="RED PHASE: report aggregator not yet implemented")
def test_validity_high_variance():
    """AC#6: stddev/median >= 0.5 produces HIGH_VARIANCE."""
    from teleproto3.bench.report import compute_validity

    # Arrange — HIGH_VARIANCE_RUNS: wildly varying throughput values
    csv_text = _make_csv(HIGH_VARIANCE_RUNS)
    # Valid throughputs (idx 1-10): [2.0, 3.0, 50.0, 2.5, 55.0, 3.5, 48.0, 4.0, 52.0, 60.0]
    # median ~ 26.0, stddev ~ 25.5 -> ratio ~ 0.98 >> 0.5

    # Act
    validity = compute_validity(csv_text, mode="sink", size_bytes=10485760)

    # Assert
    assert validity == "HIGH_VARIANCE"


# ---------------------------------------------------------------------------
# AC#6: Validity gate — INVALID (low n)
# ---------------------------------------------------------------------------


@pytest.mark.skip(reason="RED PHASE: report aggregator not yet implemented")
def test_validity_invalid_low_n():
    """AC#6: n_valid < 10 produces INVALID."""
    from teleproto3.bench.report import compute_validity

    # Arrange — LOW_N_RUNS: only 5 valid runs after warmup exclusion
    csv_text = _make_csv(LOW_N_RUNS)

    # Act
    validity = compute_validity(csv_text, mode="source", size_bytes=52428800)

    # Assert
    assert validity == "INVALID"


# ---------------------------------------------------------------------------
# AC#6: Validity gate — INVALID (SHA-256 mismatch rate > 10%)
# ---------------------------------------------------------------------------


@pytest.mark.skip(reason="RED PHASE: report aggregator not yet implemented")
def test_validity_invalid_sha256_mismatch():
    """AC#6: sha256 mismatch in echo mode (>10% rate) produces INVALID."""
    from teleproto3.bench.report import compute_validity

    # Arrange — SHA256_MISMATCH_RUNS: 3 out of 10 valid runs have sha256=false (30%)
    csv_text = _make_csv(SHA256_MISMATCH_RUNS)

    # Act
    validity = compute_validity(csv_text, mode="echo", size_bytes=1048576)

    # Assert
    assert validity == "INVALID"


# ---------------------------------------------------------------------------
# AC#7: Acceptance gate — PASS
# ---------------------------------------------------------------------------


@pytest.mark.skip(reason="RED PHASE: report aggregator not yet implemented")
def test_acceptance_gate_pass():
    """AC#7: all cells VALID with ratios >= 0.70 produces PASS."""
    from teleproto3.bench.report import compute_acceptance_gate

    # Arrange — all cells VALID, ratios well above 0.70
    cells = [
        {"mode": "echo", "size_bytes": 1048576, "validity": "VALID", "ratio": 0.85},
        {"mode": "sink", "size_bytes": 1048576, "validity": "VALID", "ratio": 0.92},
        {"mode": "source", "size_bytes": 1048576, "validity": "VALID", "ratio": 0.78},
        {"mode": "echo", "size_bytes": 10485760, "validity": "VALID", "ratio": 0.80},
        {"mode": "sink", "size_bytes": 10485760, "validity": "VALID", "ratio": 0.88},
        {"mode": "source", "size_bytes": 10485760, "validity": "VALID", "ratio": 0.75},
        {"mode": "echo", "size_bytes": 52428800, "validity": "VALID", "ratio": 0.72},
        {"mode": "sink", "size_bytes": 52428800, "validity": "VALID", "ratio": 0.90},
        {"mode": "source", "size_bytes": 52428800, "validity": "VALID", "ratio": 0.71},
    ]

    # Act
    gate = compute_acceptance_gate(cells)

    # Assert
    assert gate == "PASS"


# ---------------------------------------------------------------------------
# AC#7: Acceptance gate — WARN
# ---------------------------------------------------------------------------


@pytest.mark.skip(reason="RED PHASE: report aggregator not yet implemented")
def test_acceptance_gate_warn():
    """AC#7: all VALID/HIGH_VARIANCE, ratios 0.30-0.70 range produces WARN."""
    from teleproto3.bench.report import compute_acceptance_gate

    # Arrange — mix of VALID and HIGH_VARIANCE; one ratio below 0.70 but above 0.30
    cells = [
        {"mode": "echo", "size_bytes": 1048576, "validity": "VALID", "ratio": 0.85},
        {"mode": "sink", "size_bytes": 1048576, "validity": "VALID", "ratio": 0.55},
        {
            "mode": "source",
            "size_bytes": 1048576,
            "validity": "HIGH_VARIANCE",
            "ratio": 0.42,
        },
        {"mode": "echo", "size_bytes": 10485760, "validity": "VALID", "ratio": 0.80},
        {"mode": "sink", "size_bytes": 10485760, "validity": "VALID", "ratio": 0.72},
        {
            "mode": "source",
            "size_bytes": 10485760,
            "validity": "HIGH_VARIANCE",
            "ratio": 0.65,
        },
        {"mode": "echo", "size_bytes": 52428800, "validity": "VALID", "ratio": 0.70},
        {"mode": "sink", "size_bytes": 52428800, "validity": "VALID", "ratio": 0.88},
        {"mode": "source", "size_bytes": 52428800, "validity": "VALID", "ratio": 0.75},
    ]

    # Act
    gate = compute_acceptance_gate(cells)

    # Assert
    assert gate == "WARN"


# ---------------------------------------------------------------------------
# AC#7: Acceptance gate — FAIL
# ---------------------------------------------------------------------------


@pytest.mark.skip(reason="RED PHASE: report aggregator not yet implemented")
def test_acceptance_gate_fail():
    """AC#7: any INVALID cell or any ratio < 0.30 produces FAIL."""
    from teleproto3.bench.report import compute_acceptance_gate

    # Arrange — one INVALID cell (low n) plus one ratio below 0.30
    cells = [
        {"mode": "echo", "size_bytes": 1048576, "validity": "VALID", "ratio": 0.85},
        {
            "mode": "sink",
            "size_bytes": 1048576,
            "validity": "VALID",
            "ratio": 0.22,
        },  # below 0.30
        {
            "mode": "source",
            "size_bytes": 1048576,
            "validity": "INVALID",
            "ratio": 0.0,
        },  # INVALID
        {"mode": "echo", "size_bytes": 10485760, "validity": "VALID", "ratio": 0.80},
        {"mode": "sink", "size_bytes": 10485760, "validity": "VALID", "ratio": 0.72},
        {"mode": "source", "size_bytes": 10485760, "validity": "VALID", "ratio": 0.65},
        {"mode": "echo", "size_bytes": 52428800, "validity": "VALID", "ratio": 0.70},
        {"mode": "sink", "size_bytes": 52428800, "validity": "VALID", "ratio": 0.88},
        {"mode": "source", "size_bytes": 52428800, "validity": "VALID", "ratio": 0.75},
    ]

    # Act
    gate = compute_acceptance_gate(cells)

    # Assert
    assert gate == "FAIL"


# ---------------------------------------------------------------------------
# AC#5: ratio computation — bench_p50 / iperf3_throughput
# ---------------------------------------------------------------------------


@pytest.mark.skip(reason="RED PHASE: report aggregator not yet implemented")
def test_ratio_computation():
    """AC#5: ratio = bench_p50 / iperf3_throughput_mbps computed correctly."""
    from teleproto3.bench.report import compute_ratio

    # Arrange — known bench p50 and iperf3 baseline
    bench_p50_mbps = 10.35
    iperf3_throughput_mbps = 14.8  # realistic VPS baseline

    # Act
    ratio = compute_ratio(bench_p50_mbps, iperf3_throughput_mbps)

    # Assert
    expected = 10.35 / 14.8  # ~0.699
    assert abs(ratio - expected) < 0.001
    assert 0.0 < ratio <= 1.0
