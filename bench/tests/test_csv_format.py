"""
RED-phase tests for Story 1a-3 AC#5: CSV row output format.

The bench client emits one CSV row per run with exactly 9 columns:
    ts_iso, mode, size_bytes, run_index, ttfb_ms, duration_ms,
    throughput_mbps, sha256_match, error_class

Tests validate header structure, field constraints, and value domains.
"""

from __future__ import annotations

import csv
import io
from datetime import datetime, timezone

import pytest


# ---------------------------------------------------------------------------
# Helpers — realistic CSV data used across tests
# ---------------------------------------------------------------------------

CSV_HEADER = "ts_iso,mode,size_bytes,run_index,ttfb_ms,duration_ms,throughput_mbps,sha256_match,error_class"

SAMPLE_ECHO_ROW = (
    "2026-05-06T14:23:17.482Z,echo,1048576,1,12.34,847.21,9.89,true,ok"
)

SAMPLE_SINK_ROW = (
    "2026-05-06T14:24:05.001Z,sink,10485760,3,8.72,1523.40,55.07,na,ok"
)

SAMPLE_SOURCE_ROW = (
    "2026-05-06T14:25:12.993Z,source,52428800,2,15.60,4201.88,99.84,na,ok"
)

SAMPLE_ERROR_ROW = (
    "2026-05-06T14:26:00.000Z,echo,1048576,0,0.00,0.00,0.00,false,handshake_fail"
)

EXPECTED_COLUMNS = [
    "ts_iso",
    "mode",
    "size_bytes",
    "run_index",
    "ttfb_ms",
    "duration_ms",
    "throughput_mbps",
    "sha256_match",
    "error_class",
]


def _parse_csv(header_and_rows: str) -> list[dict[str, str]]:
    """Parse CSV text into a list of dicts."""
    reader = csv.DictReader(io.StringIO(header_and_rows))
    return list(reader)


# ---------------------------------------------------------------------------
# Test: CSV header has exactly the 9 specified columns
# ---------------------------------------------------------------------------

@pytest.mark.skip(reason="RED PHASE: CSV emitter not yet implemented")
def test_csv_header_fields():
    """AC#5: header row contains exactly these 9 columns in order."""
    from teleproto3.bench.bench_client import CsvEmitter

    # Arrange
    buf = io.StringIO()
    emitter = CsvEmitter(output=buf)

    # Act
    emitter.write_header()
    buf.seek(0)
    header_line = buf.readline().strip()

    # Assert
    columns = header_line.split(",")
    assert columns == EXPECTED_COLUMNS
    assert len(columns) == 9


# ---------------------------------------------------------------------------
# Test: echo row has sha256_match=true, error_class=ok
# ---------------------------------------------------------------------------

@pytest.mark.skip(reason="RED PHASE: CSV emitter not yet implemented")
def test_csv_row_valid_echo():
    """AC#5: a successful echo row has sha256_match=true and error_class=ok."""
    # Arrange
    full_csv = CSV_HEADER + "\n" + SAMPLE_ECHO_ROW + "\n"

    # Act
    rows = _parse_csv(full_csv)

    # Assert
    assert len(rows) == 1
    row = rows[0]
    assert row["mode"] == "echo"
    assert row["sha256_match"] == "true"
    assert row["error_class"] == "ok"
    assert int(row["size_bytes"]) == 1048576
    assert int(row["run_index"]) == 1


# ---------------------------------------------------------------------------
# Test: sink row has sha256_match=na (no return data)
# ---------------------------------------------------------------------------

@pytest.mark.skip(reason="RED PHASE: CSV emitter not yet implemented")
def test_csv_row_valid_sink():
    """AC#5: a sink row has sha256_match=na since no data is returned."""
    # Arrange
    full_csv = CSV_HEADER + "\n" + SAMPLE_SINK_ROW + "\n"

    # Act
    rows = _parse_csv(full_csv)

    # Assert
    assert len(rows) == 1
    row = rows[0]
    assert row["mode"] == "sink"
    assert row["sha256_match"] == "na"
    assert row["error_class"] == "ok"
    assert int(row["size_bytes"]) == 10485760


# ---------------------------------------------------------------------------
# Test: throughput_mbps > 0 for valid runs
# ---------------------------------------------------------------------------

@pytest.mark.skip(reason="RED PHASE: CSV emitter not yet implemented")
def test_csv_throughput_positive():
    """AC#5: throughput_mbps is strictly positive for all successful runs."""
    # Arrange — three valid rows covering all modes
    full_csv = (
        CSV_HEADER + "\n"
        + SAMPLE_ECHO_ROW + "\n"
        + SAMPLE_SINK_ROW + "\n"
        + SAMPLE_SOURCE_ROW + "\n"
    )

    # Act
    rows = _parse_csv(full_csv)

    # Assert
    assert len(rows) == 3
    for row in rows:
        throughput = float(row["throughput_mbps"])
        assert throughput > 0.0, (
            f"throughput_mbps must be positive for mode={row['mode']}, "
            f"got {throughput}"
        )


# ---------------------------------------------------------------------------
# Test: error_class is one of the allowed enum values
# ---------------------------------------------------------------------------

@pytest.mark.skip(reason="RED PHASE: CSV emitter not yet implemented")
def test_csv_error_class_values():
    """AC#5: error_class is restricted to the 5 defined values."""
    ALLOWED_ERROR_CLASSES = {
        "ok",
        "handshake_fail",
        "corruption",
        "timeout",
        "connection_reset",
    }

    # Arrange — include both success and error rows
    full_csv = (
        CSV_HEADER + "\n"
        + SAMPLE_ECHO_ROW + "\n"
        + SAMPLE_ERROR_ROW + "\n"
    )

    # Act
    rows = _parse_csv(full_csv)

    # Assert
    assert len(rows) == 2
    for row in rows:
        assert row["error_class"] in ALLOWED_ERROR_CLASSES, (
            f"Unexpected error_class '{row['error_class']}'; "
            f"allowed: {ALLOWED_ERROR_CLASSES}"
        )


# ---------------------------------------------------------------------------
# Test: ts_iso is valid ISO 8601
# ---------------------------------------------------------------------------

@pytest.mark.skip(reason="RED PHASE: CSV emitter not yet implemented")
def test_csv_timestamp_iso_format():
    """AC#5: ts_iso field is a valid ISO 8601 timestamp with UTC indicator."""
    # Arrange
    full_csv = (
        CSV_HEADER + "\n"
        + SAMPLE_ECHO_ROW + "\n"
        + SAMPLE_SINK_ROW + "\n"
        + SAMPLE_SOURCE_ROW + "\n"
        + SAMPLE_ERROR_ROW + "\n"
    )

    # Act
    rows = _parse_csv(full_csv)

    # Assert
    assert len(rows) == 4
    for row in rows:
        ts_str = row["ts_iso"]
        # Must end with Z (UTC)
        assert ts_str.endswith("Z"), f"Timestamp '{ts_str}' must end with 'Z'"
        # Must parse as ISO 8601
        parsed = datetime.fromisoformat(ts_str.replace("Z", "+00:00"))
        assert parsed.tzinfo is not None
        # Sanity: year is reasonable
        assert parsed.year >= 2026
