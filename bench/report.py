#!/usr/bin/env python3
"""
report.py — Bench report aggregator for Epic 1a (Story 1a-5).

Reads runs.csv (bench_client.py output) and iperf3_baseline.json,
computes per-cell stats (p50/p95/p99/stddev/mean), validity gates,
echo direction asymmetry, and emits a Markdown summary.

CLI:
    python3 report.py runs.csv iperf3_baseline.json [--output summary.md] [--smoke]

Final stdout line: PASS / WARN / FAIL / INDETERMINATE

Exit code: 0 if at least one CSV row parsed and report rendered; non-zero on
fatal error (no input rows / unreadable CSV) — process-level liveness signal
for callers (P15 / D1, Murat).
"""

from __future__ import annotations

import argparse
import csv
import io
import json
import statistics
import sys
from pathlib import Path
from typing import Optional


def _parse_csv(csv_text: str) -> list[dict]:
    return list(csv.DictReader(io.StringIO(csv_text)))


def _safe_int(value: object, default: Optional[int] = None) -> Optional[int]:
    """Tolerant int parse for CSV cells: returns default on empty/malformed."""
    if value is None:
        return default
    try:
        return int(str(value).strip())
    except (ValueError, TypeError):
        return default


def _valid_rows(csv_text: str, mode: str, size_bytes: int) -> list[dict]:
    """Non-warmup rows for the given (mode, size_bytes) cell.

    Rows with malformed run_index / size_bytes are silently skipped (P5):
    a single corrupt line does not crash the whole report.
    """
    out = []
    for r in _parse_csv(csv_text):
        if r.get("mode") != mode:
            continue
        row_size = _safe_int(r.get("size_bytes"))
        if row_size != size_bytes:
            continue
        row_idx = _safe_int(r.get("run_index"))
        if row_idx is None or row_idx <= 0:
            continue
        out.append(r)
    return out


def aggregate_cell(csv_text: str, mode: str, size_bytes: int) -> dict:
    """Compute throughput stats for a (mode, size_bytes) cell.

    Returns dict with keys: n_valid, throughput_values, p50, p95, p99, mean, stddev,
    upload_p50, download_p50 (echo only; None for sink/source).
    """
    rows = _valid_rows(csv_text, mode, size_bytes)
    n = len(rows)

    if n == 0:
        return {
            "n_valid": 0,
            "throughput_values": [],
            "p50": 0.0,
            "p95": 0.0,
            "p99": 0.0,
            "mean": 0.0,
            "stddev": 0.0,
            "upload_p50": None,
            "download_p50": None,
        }

    values = [float(r["throughput_mbps"]) for r in rows]

    if n == 1:
        p50 = p95 = p99 = values[0]
        mean_v = values[0]
        stddev_v = 0.0
    else:
        # P21 / D7: inclusive method — no extrapolation past observed range,
        # comparable to Prometheus / Grafana / Datadog percentile semantics.
        qs = statistics.quantiles(values, n=100, method="inclusive")
        p50 = qs[49]
        p95 = qs[94]
        p99 = qs[98]
        mean_v = statistics.mean(values)
        stddev_v = statistics.stdev(values)

    upload_p50 = None
    download_p50 = None
    if mode == "echo":
        upload_vals = [
            float(r["upload_mbps"])
            for r in rows
            if r.get("upload_mbps") not in (None, "", "na")
        ]
        download_vals = [
            float(r["download_mbps"])
            for r in rows
            if r.get("download_mbps") not in (None, "", "na")
        ]
        if upload_vals:
            upload_p50 = statistics.median(upload_vals)
        if download_vals:
            download_p50 = statistics.median(download_vals)

    return {
        "n_valid": n,
        "throughput_values": values,
        "p50": p50,
        "p95": p95,
        "p99": p99,
        "mean": mean_v,
        "stddev": stddev_v,
        "upload_p50": upload_p50,
        "download_p50": download_p50,
    }


def _parse_sha256_match(value: object) -> Optional[bool]:
    """Strict parser for sha256_match cell. None = not applicable / unknown."""
    if value is None:
        return None
    s = str(value).strip().lower()
    if s in ("true", "1"):
        return True
    if s in ("false", "0"):
        return False
    if s in ("na", "n/a", ""):
        return None
    return None


def compute_validity(csv_text: str, mode: str, size_bytes: int) -> str:
    """Compute validity gate for a cell.

    Returns: 'VALID', 'HIGH_VARIANCE', or 'INVALID'

    INVALID if: n_valid < 10
                OR error_class != 'ok' rate > 10%
                OR (echo mode) ANY sha256 mismatch (per AC#6: sha256_match_rate == 1.0)
    HIGH_VARIANCE if VALID candidate but stddev/median >= 0.5
    VALID otherwise
    """
    rows = _valid_rows(csv_text, mode, size_bytes)
    n = len(rows)

    if n < 10:
        return "INVALID"

    error_count = sum(1 for r in rows if r.get("error_class", "ok") != "ok")
    if error_count / n > 0.10:
        return "INVALID"

    if mode == "echo":
        # P2 / D5 strictness: AC#6 demands sha256_match_rate == 1.0 in echo mode.
        # Any unambiguous mismatch -> INVALID. Unknown/unparseable values are
        # treated as not-a-match and also produce INVALID (P10 strict parsing).
        for r in rows:
            parsed = _parse_sha256_match(r.get("sha256_match"))
            if parsed is False:
                return "INVALID"
            if parsed is None:
                # echo mode must produce a definitive sha256_match per the
                # bench_client contract; unknown values are a producer bug.
                return "INVALID"

    values = [float(r["throughput_mbps"]) for r in rows]
    median = statistics.median(values)
    stddev = statistics.stdev(values) if n > 1 else 0.0

    if median > 0 and stddev / median >= 0.5:
        return "HIGH_VARIANCE"

    return "VALID"


def compute_acceptance_gate(cells: list[dict], smoke: bool = False) -> str:
    """Compute acceptance gate from list of cell result dicts.

    P19 / D5: ratio is informational only — gate is validity-driven.
    P15 / D1: smoke mode emits INDETERMINATE (gate not applicable).
    P12: empty input emits FAIL (no data to assess).

    Rules:
      INDETERMINATE: smoke=True (n_valid<10 by design) OR no cells provided
                     (zero-data run is not a clean PASS).
      FAIL:          any cell INVALID.
      WARN:          any cell HIGH_VARIANCE, none INVALID.
      PASS:          all cells VALID.
    """
    if smoke:
        return "INDETERMINATE"

    if not cells:
        # P12: vacuous PASS would lie about a zero-data run; surface the failure.
        return "FAIL"

    if all(c.get("n_valid", 0) == 0 for c in cells):
        return "FAIL"

    for cell in cells:
        if cell.get("validity") == "INVALID":
            return "FAIL"

    if any(c.get("validity") == "HIGH_VARIANCE" for c in cells):
        return "WARN"

    return "PASS"


def compute_ratio(
    bench_p50_mbps: float, iperf3_throughput_mbps: Optional[float]
) -> Optional[float]:
    """Return bench_p50_mbps / iperf3_throughput_mbps, or None if baseline absent.

    P18 / D4: None signals "ratio not applicable"; gate ignores ratio entirely
    per P19 / D5, but the value is still rendered as informational diagnostic.
    """
    if iperf3_throughput_mbps is None or iperf3_throughput_mbps <= 0:
        return None
    return bench_p50_mbps / iperf3_throughput_mbps


def _load_iperf3_throughput(path: str) -> Optional[float]:
    """Extract received-side Mbps from iperf3 -J output. Returns None on any failure."""
    try:
        data = json.loads(Path(path).read_text())
        bps = data["end"]["sum_received"]["bits_per_second"]
        return float(bps) / 1_000_000
    except Exception:
        return None


_SIZE_LABELS = {1048576: "1MB", 10485760: "10MB", 52428800: "50MB"}


def generate_summary_md(
    csv_path: str,
    iperf3_json_path: str,
    sizes: Optional[list[int]] = None,
    modes: Optional[list[str]] = None,
    smoke: bool = False,
) -> tuple[str, str]:
    """Generate Markdown summary and acceptance gate string.

    Returns: (markdown_text, gate)
       gate ∈ {'PASS', 'WARN', 'FAIL', 'INDETERMINATE'}
    """
    if sizes is None:
        sizes = [1048576] if smoke else [1048576, 10485760, 52428800]
    if modes is None:
        modes = ["sink", "echo", "source"]

    csv_text = Path(csv_path).read_text()
    iperf3_mbps = _load_iperf3_throughput(iperf3_json_path)

    cell_results = []
    for size in sizes:
        for mode in modes:
            stats = aggregate_cell(csv_text, mode, size)
            validity = compute_validity(csv_text, mode, size)
            ratio = compute_ratio(stats["p50"], iperf3_mbps)
            ratio_str = f"{ratio:.3f}" if ratio is not None else "N/A"
            cell_results.append(
                {
                    "size": size,
                    "mode": mode,
                    "validity": validity,
                    "ratio": ratio,
                    "ratio_str": ratio_str,
                    **stats,
                }
            )

    gate = compute_acceptance_gate(cell_results, smoke=smoke)

    lines = [
        "# Type3 Bench Report",
        "",
        "## Per-Cell Throughput Stats (warmup excluded)",
        "",
        "| Size | Mode | n | p50 Mbps | p95 Mbps | p99 Mbps | mean | stddev | Validity |",
        "|------|------|---|----------|----------|----------|------|--------|----------|",
    ]
    for c in cell_results:
        sz = _SIZE_LABELS.get(c["size"], str(c["size"]))
        lines.append(
            f"| {sz} | {c['mode']} | {c['n_valid']}"
            f" | {c['p50']:.2f} | {c['p95']:.2f} | {c['p99']:.2f}"
            f" | {c['mean']:.2f} | {c['stddev']:.2f} | {c['validity']} |"
        )

    # --- P20 / D6: echo direction asymmetry as informational side-info ---
    echo_cells_with_data = [
        c
        for c in cell_results
        if c["mode"] == "echo"
        and c["upload_p50"] is not None
        and c["download_p50"] is not None
    ]
    if echo_cells_with_data:
        lines += [
            "",
            "### Echo Direction Asymmetry (informational)",
            "",
        ]
        for c in echo_cells_with_data:
            up = c["upload_p50"]
            dn = c["download_p50"]
            mx = max(up, dn)
            asym = (up - dn) / mx if mx > 0 else 0.0
            sz = _SIZE_LABELS.get(c["size"], str(c["size"]))
            lines.append(
                f"- echo-{sz}: upload p50 = {up:.2f} Mbps, "
                f"download p50 = {dn:.2f} Mbps, asymmetry = {asym:+.2%}"
            )

    iperf3_label = (
        f"{iperf3_mbps:.2f} Mbps" if iperf3_mbps is not None else "N/A (iperf3 not run)"
    )
    lines += [
        "",
        "## Ratio Table (informational — stack overhead diagnostic, not part of acceptance gate)",
        "",
        f"_iperf3 baseline: {iperf3_label}_",
        "",
        "| Size | Mode | p50 Mbps | iperf3 Mbps | Ratio |",
        "|------|------|----------|-------------|-------|",
    ]
    for c in cell_results:
        sz = _SIZE_LABELS.get(c["size"], str(c["size"]))
        i3 = f"{iperf3_mbps:.2f}" if iperf3_mbps is not None else "N/A"
        lines.append(
            f"| {sz} | {c['mode']} | {c['p50']:.2f} | {i3} | {c['ratio_str']} |"
        )

    if iperf3_mbps is None:
        lines += [
            "",
            "_iperf3 baseline unavailable — stack-overhead ratio undefined; "
            "gate decision based on AC#6 validity only._",
        ]

    lines += [
        "",
        "## Validity Status per Cell",
        "",
        "| Size | Mode | n_valid | Validity |",
        "|------|------|---------|----------|",
    ]
    for c in cell_results:
        sz = _SIZE_LABELS.get(c["size"], str(c["size"]))
        lines.append(f"| {sz} | {c['mode']} | {c['n_valid']} | {c['validity']} |")

    lines += ["", "---", ""]

    if gate == "INDETERMINATE":
        if smoke:
            lines.append(
                "**Acceptance Gate: INDETERMINATE — smoke mode, "
                "statistical validity gate skipped (n_valid<10 by design)**"
            )
        else:
            lines.append("**Acceptance Gate: INDETERMINATE — no data to assess**")
    else:
        lines.append(f"**Acceptance Gate: {gate}**")

    return "\n".join(lines), gate


def main() -> int:
    parser = argparse.ArgumentParser(description="Type3 bench report aggregator")
    parser.add_argument("csv", help="path to runs.csv")
    parser.add_argument("iperf3_json", help="path to iperf3_baseline.json")
    parser.add_argument(
        "--output",
        default="-",
        help="output path for summary.md (default: - = stdout)",
    )
    parser.add_argument(
        "--smoke",
        action="store_true",
        help="smoke-mode report; gate emits INDETERMINATE (n_valid<10 by design)",
    )
    args = parser.parse_args()

    # P15 / D1 liveness exit code: report runs only if CSV exists and is parseable.
    csv_path = Path(args.csv)
    if not csv_path.exists():
        print(f"error: CSV not found: {args.csv}", file=sys.stderr)
        return 2
    if csv_path.stat().st_size == 0:
        print(f"error: CSV is empty: {args.csv}", file=sys.stderr)
        return 2

    md, gate = generate_summary_md(args.csv, args.iperf3_json, smoke=args.smoke)

    if args.output == "-":
        print(md)
    else:
        out = Path(args.output)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(md + "\n")

    print(gate)
    return 0


if __name__ == "__main__":
    sys.exit(main())
