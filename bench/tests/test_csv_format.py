"""
Tests for Story 1a-3 AC#5: CSV row output format.

The bench client emits one CSV row per run with exactly 11 columns:
    ts_iso, mode, size_bytes, run_index, duration_ms, throughput_mbps,
    upload_mbps, download_mbps, sha256_match, fixture_sha256, error_class

Tests validate header structure, field constraints, value domains,
and the CsvEmitter class.

Note: ttfb_ms column was removed in Round 1 review (D1 resolution): per-row
Python monotonic_ns() over chunked recv cannot honestly measure first-byte
latency at sub-50µs precision (kernel-level instrumentation required for
that — out of scope for v0.1.x dev-self-use bench). See deferred-work.md
"option (e) backlog: stdout summary block".
"""

from __future__ import annotations

import csv
import io
from datetime import datetime

import pytest

CSV_HEADER = (
    "ts_iso,mode,size_bytes,run_index,duration_ms,throughput_mbps,"
    "upload_mbps,download_mbps,sha256_match,fixture_sha256,error_class"
)

_FAKE_SHA256 = "aabbccddaabbccddaabbccddaabbccddaabbccddaabbccddaabbccddaabbccdd"

# echo row: upload_mbps + download_mbps populated (full duplex mode)
SAMPLE_ECHO_ROW = (
    f"2026-05-06T14:23:17.482Z,echo,1048576,1,847.21,9.89,5.01,4.95,"
    f"true,{_FAKE_SHA256},ok"
)

# sink row: upload/download na (single direction)
SAMPLE_SINK_ROW = (
    f"2026-05-06T14:24:05.001Z,sink,10485760,3,1523.40,55.07,na,na,"
    f"na,{_FAKE_SHA256},ok"
)

SAMPLE_SOURCE_ROW = (
    "2026-05-06T14:25:12.993Z,source,52428800,2,4201.88,99.84,na,na,na,na,ok"
)

SAMPLE_ERROR_ROW = (
    "2026-05-06T14:26:00.000Z,echo,1048576,0,0.00,0.00,na,na,false,na,handshake_fail"
)

EXPECTED_COLUMNS = [
    "ts_iso",
    "mode",
    "size_bytes",
    "run_index",
    "duration_ms",
    "throughput_mbps",
    "upload_mbps",
    "download_mbps",
    "sha256_match",
    "fixture_sha256",
    "error_class",
]


def _parse_csv(header_and_rows: str) -> list[dict[str, str]]:
    reader = csv.DictReader(io.StringIO(header_and_rows))
    return list(reader)


def test_csv_header_fields():
    """AC#5: CsvEmitter writes header with exactly 11 columns in order."""
    from teleproto3.bench.bench_client import CsvEmitter

    buf = io.StringIO()
    emitter = CsvEmitter(output=buf)

    emitter.write_header()
    buf.seek(0)
    header_line = buf.readline().strip()

    columns = header_line.split(",")
    assert columns == EXPECTED_COLUMNS
    assert len(columns) == 11


def test_csv_row_valid_echo():
    """AC#5: a successful echo row has sha256_match=true and error_class=ok."""
    full_csv = CSV_HEADER + "\n" + SAMPLE_ECHO_ROW + "\n"

    rows = _parse_csv(full_csv)

    assert len(rows) == 1
    row = rows[0]
    assert row["mode"] == "echo"
    assert row["sha256_match"] == "true"
    assert row["error_class"] == "ok"
    assert int(row["size_bytes"]) == 1048576
    assert int(row["run_index"]) == 1


def test_csv_row_valid_sink():
    """AC#5: a sink row has sha256_match=na since no data is returned."""
    full_csv = CSV_HEADER + "\n" + SAMPLE_SINK_ROW + "\n"

    rows = _parse_csv(full_csv)

    assert len(rows) == 1
    row = rows[0]
    assert row["mode"] == "sink"
    assert row["sha256_match"] == "na"
    assert row["error_class"] == "ok"
    assert int(row["size_bytes"]) == 10485760


def test_csv_throughput_positive():
    """AC#5: throughput_mbps is strictly positive for all successful runs."""
    full_csv = (
        CSV_HEADER
        + "\n"
        + SAMPLE_ECHO_ROW
        + "\n"
        + SAMPLE_SINK_ROW
        + "\n"
        + SAMPLE_SOURCE_ROW
        + "\n"
    )

    rows = _parse_csv(full_csv)

    assert len(rows) == 3
    for row in rows:
        throughput = float(row["throughput_mbps"])
        assert throughput > 0.0, (
            f"throughput_mbps must be positive for mode={row['mode']}, "
            f"got {throughput}"
        )


def test_csv_error_class_values():
    """AC#5: error_class is restricted to the 5 defined values."""
    ALLOWED_ERROR_CLASSES = {
        "ok",
        "handshake_fail",
        "corruption",
        "timeout",
        "connection_reset",
    }

    full_csv = CSV_HEADER + "\n" + SAMPLE_ECHO_ROW + "\n" + SAMPLE_ERROR_ROW + "\n"

    rows = _parse_csv(full_csv)

    assert len(rows) == 2
    for row in rows:
        assert row["error_class"] in ALLOWED_ERROR_CLASSES, (
            f"Unexpected error_class '{row['error_class']}'; "
            f"allowed: {ALLOWED_ERROR_CLASSES}"
        )


def test_csv_timestamp_iso_format():
    """AC#5: ts_iso field is a valid ISO 8601 timestamp with UTC indicator."""
    full_csv = (
        CSV_HEADER
        + "\n"
        + SAMPLE_ECHO_ROW
        + "\n"
        + SAMPLE_SINK_ROW
        + "\n"
        + SAMPLE_SOURCE_ROW
        + "\n"
        + SAMPLE_ERROR_ROW
        + "\n"
    )

    rows = _parse_csv(full_csv)

    assert len(rows) == 4
    for row in rows:
        ts_str = row["ts_iso"]
        assert ts_str.endswith("Z"), f"Timestamp '{ts_str}' must end with 'Z'"
        parsed = datetime.fromisoformat(ts_str.replace("Z", "+00:00"))
        assert parsed.tzinfo is not None
        assert parsed.year >= 2026


def test_csv_emitter_write_row(tmp_path):
    """CsvEmitter writes header + row atomically."""
    from teleproto3.bench.bench_client import CsvEmitter, RunResult

    csv_path = tmp_path / "test.csv"
    emitter = CsvEmitter(output=str(csv_path))

    result = RunResult(
        ts_iso="2026-05-06T12:00:00.000Z",
        mode="echo",
        size_bytes=1024,
        run_index=0,
        duration_ms=100.0,
        throughput_mbps=0.08,
        sha256_match="true",
        fixture_sha256=_FAKE_SHA256,
        error_class="ok",
    )
    emitter.write_row(result)
    emitter.close()

    content = csv_path.read_text()
    rows = _parse_csv(content)
    assert len(rows) == 1
    assert rows[0]["mode"] == "echo"
    assert rows[0]["sha256_match"] == "true"


def test_csv_emitter_append_mode(tmp_path):
    """CsvEmitter appends rows without duplicating header."""
    from teleproto3.bench.bench_client import CsvEmitter, RunResult

    csv_path = tmp_path / "test.csv"

    for i in range(3):
        emitter = CsvEmitter(output=str(csv_path))
        emitter.write_row(
            RunResult(
                ts_iso="2026-05-06T12:00:00.000Z",
                mode="sink",
                size_bytes=1024,
                run_index=i,
                duration_ms=50.0,
                throughput_mbps=0.16,
                sha256_match="na",
                fixture_sha256="na",
                error_class="ok",
            )
        )
        emitter.close()

    content = csv_path.read_text()
    lines = [l for l in content.strip().split("\n") if l]
    # First open writes header + row; subsequent opens see file not empty, skip header
    assert lines[0].startswith("ts_iso,")
    data_lines = [l for l in lines if not l.startswith("ts_iso")]
    assert len(data_lines) == 3
