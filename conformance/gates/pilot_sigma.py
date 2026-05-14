#!/usr/bin/env python3
# pilot_sigma.py — pilot σ̂ measurement; derives per-bucket N_required for AR-C2 TOST gate.
#
# Source: story 1-3a AC#4, AC#5.
# Style: Python-comment banner per epic-1-style-guide.md §1.
#
# Normative. See spec/anti-probe.md §8.3 for the gate specification.
# Where this script and spec/ differ, spec/ wins.
# Stability: public API is the CLI only; internal functions are private.
#
# Usage:
#   python3 pilot_sigma.py \
#       --log 'teleproto3/lib/fuzz/side-channel-*.log' \
#       --output teleproto3/conformance/baselines/lib-v0.1.x/ar-c2-pilot.yaml

import argparse
import datetime
import glob
import math
import os
import re
import sys

# ------------------------------------------------------------------ #
# Constants                                                            #
# ------------------------------------------------------------------ #

# Outlier filters — mirror analyse.py (story 1-10) for consistency.
PARSE_NS_OUTLIER_MS = 10        # >10ms parse_ns → suspect preemption
TOTAL_NS_OUTLIER_MS = 250       # >250ms total_ns → suspect scheduler hiccup

PARSE_NS_OUTLIER  = PARSE_NS_OUTLIER_MS  * 1_000_000
TOTAL_NS_OUTLIER  = TOTAL_NS_OUTLIER_MS  * 1_000_000

# §8.2 canonical bucket edges — normative; downstream MUST NOT redefine.
BUCKET_EDGES = [(0, 63), (64, 255), (256, 1023), (1024, 4095), (4096, 16383)]

# Number of bucket pairs in the family-wise Bonferroni correction: C(5,2) = 10.
NUM_PAIRS = 10

# §8.3 safety floor: effective gate is max(formula_result, EFFECTIVE_GATE_FLOOR).
EFFECTIVE_GATE_FLOOR = 10_000

# z-table entries; hard-coded for the documented defaults only.
# Raise SetupError for any other (alpha, beta) pair.
_Z_TABLE = {
    # (alpha, beta) → (z_{1-alpha}, z_{1-beta/2})
    (0.005, 0.20): (2.576, 1.282),
}

MIN_BUCKET_SAMPLES = 100    # fewer samples → σ̂ unreliable; WARN + skip
REQUIRED_LOG_FIELDS = 5     # input_len sha256 parse_ns total_ns fuzz_pid

_SHA256_RE = re.compile(r"^[0-9a-fA-F]{64}$")

PILOT_STATUS_SYNTHETIC  = "synthetic_pending_repilot"
PILOT_STATUS_PRODUCTION = "production_pilot"
_ALLOWED_STATUSES = (PILOT_STATUS_SYNTHETIC, PILOT_STATUS_PRODUCTION)


class SetupError(Exception):
    pass


# ------------------------------------------------------------------ #
# Bucket assignment                                                    #
# ------------------------------------------------------------------ #

def bucket_for(input_len):
    for i, (lo, hi) in enumerate(BUCKET_EDGES):
        if lo <= input_len <= hi:
            return i
    # Inputs above top edge are out of AR-C2 scope per spec/anti-probe.md §8.2;
    # clamp into top bucket for parity with lib/fuzz/analyse.py (same policy).
    return len(BUCKET_EDGES) - 1


# ------------------------------------------------------------------ #
# Statistics — stdlib-only                                             #
# ------------------------------------------------------------------ #

def sample_stddev(values):
    """Sample standard deviation (Bessel-corrected, n-1 denominator)."""
    n = len(values)
    if n < 2:
        return 0.0
    mean = sum(values) / n
    variance = sum((x - mean) ** 2 for x in values) / (n - 1)
    return math.sqrt(variance)


def n_required(sigma_ns, z_alpha, z_beta_half, delta_ns):
    """TOST sample-size formula: ceil(2*(z_alpha+z_beta_half)^2*sigma^2/delta^2)."""
    return math.ceil(
        2.0 * (z_alpha + z_beta_half) ** 2 * sigma_ns ** 2 / delta_ns ** 2
    )


# ------------------------------------------------------------------ #
# Log parsing                                                          #
# ------------------------------------------------------------------ #

def parse_logs(log_glob):
    """
    Glob-expand log_glob and parse all matching 5-column TSV files.
    Returns (records, paths, skipped_malformed, skipped_outlier).
    """
    paths = sorted(glob.glob(log_glob))
    if not paths:
        raise SetupError(f"No log files matched: {log_glob!r}")

    records = []
    skipped_malformed = 0
    skipped_outlier   = 0
    for path in paths:
        with open(path, "r") as fh:
            for lineno, line in enumerate(fh, 1):
                line = line.rstrip("\n")
                if not line:
                    continue
                parts = line.split("\t")
                if len(parts) != REQUIRED_LOG_FIELDS:
                    skipped_malformed += 1
                    continue
                try:
                    input_len = int(parts[0])
                    parse_ns  = int(parts[2])
                    total_ns  = int(parts[3])
                    int(parts[4])  # fuzz_pid validation only
                except ValueError:
                    skipped_malformed += 1
                    continue
                if not _SHA256_RE.match(parts[1]):
                    skipped_malformed += 1
                    continue
                # Outlier filters
                if parse_ns > PARSE_NS_OUTLIER or total_ns > TOTAL_NS_OUTLIER:
                    skipped_outlier += 1
                    continue
                records.append((input_len, parse_ns, total_ns))

    if skipped_malformed:
        print(f"INFO: skipped {skipped_malformed} malformed record(s)", file=sys.stderr)
    if skipped_outlier:
        print(f"INFO: skipped {skipped_outlier} outlier record(s)", file=sys.stderr)
    return records, paths, skipped_malformed, skipped_outlier


# ------------------------------------------------------------------ #
# YAML emission — stdlib-only (no pyyaml)                              #
# ------------------------------------------------------------------ #

def _yaml_list_of_maps(items, indent=2):
    """Render a list of {key: value} dicts as YAML block sequence."""
    lines = []
    pad = " " * indent
    for d in items:
        first = True
        for k, v in d.items():
            if isinstance(v, list):
                v_str = "[" + ",".join(str(x) for x in v) + "]"
            elif isinstance(v, float):
                v_str = f"{v:.3f}"
            else:
                v_str = str(v)
            marker = "- " if first else "  "
            lines.append(f"{pad}{marker}{k}: {v_str}")
            first = False
    return "\n".join(lines)


def _yaml_block_scalar(text, indent=2):
    """Render a multi-line text body as a YAML literal-block scalar (`|`)."""
    if not text:
        return '""'
    pad = " " * indent
    body = "\n".join(f"{pad}{line}" for line in text.splitlines())
    return "|\n" + body


def emit_yaml(output_path, status, lib_version, captured_at, source_logs,
              buckets_n, sigma_hat, sigma_max_ns, underfilled,
              alpha_bonferroni, beta, z_alpha, z_beta_half,
              delta_ms, delta_ns, n_per_bucket, n_per_bucket_effective, notes):
    """Write the pilot YAML to output_path atomically; create parent dirs if needed."""
    if status not in _ALLOWED_STATUSES:
        raise SetupError(f"status must be one of {_ALLOWED_STATUSES}; got {status!r}")
    os.makedirs(os.path.dirname(os.path.abspath(output_path)), exist_ok=True)

    alpha_family_wise = alpha_bonferroni * NUM_PAIRS  # P1: derived, not hard-coded

    # samples_per_bucket block
    spb_items = [
        {"bucket": list(BUCKET_EDGES[i]), "n": buckets_n[i]}
        for i in range(len(BUCKET_EDGES))
    ]
    # sigma_hat_ns_per_bucket block — only populated buckets have a sigma
    shb_items = [
        {"bucket": list(BUCKET_EDGES[i]), "sigma_ns": round(sigma_hat[i], 3)}
        for i in range(len(BUCKET_EDGES))
        if sigma_hat[i] is not None
    ]
    # underfilled_buckets list
    uf_str = (
        "[]"
        if not underfilled
        else "[" + ", ".join(f"[{lo},{hi}]" for lo, hi in underfilled) + "]"
    )
    # source_logs list — relative to teleproto3/ root (2 dirs up from this script)
    _repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
    def _relpath(p):
        try:
            return os.path.relpath(os.path.abspath(p), _repo_root)
        except ValueError:
            return p
    logs_str = "[" + ", ".join(f'"{_relpath(p)}"' for p in source_logs) + "]"

    notes_str = _yaml_block_scalar(notes, indent=2)

    content = f"""\
# ar-c2-pilot.yaml — AR-C2 TOST gate pilot σ̂ measurement.
# Generated by conformance/gates/pilot_sigma.py (story 1-3a).
# See spec/anti-probe.md §8.3 for gate specification.
status:                 "{status}"
spec_version:           "0.1.0-draft"
lib_version:            "{lib_version}"
captured_at:            "{captured_at}"
clock_source:           "CLOCK_MONOTONIC"
metric:                 "parse_ns"
source_logs:            {logs_str}
bucket_edges:           {[[lo, hi] for lo, hi in BUCKET_EDGES]}
samples_per_bucket:
{_yaml_list_of_maps(spb_items)}
sigma_hat_ns_per_bucket:
{_yaml_list_of_maps(shb_items)}
sigma_max_ns:           {sigma_max_ns:.3f}
underfilled_buckets:    {uf_str}
tost_parameters:
  alpha_family_wise:    {alpha_family_wise:.3f}
  alpha_bonferroni:     {alpha_bonferroni:.3f}
  beta:                 {beta:.2f}
  z_alpha:              {z_alpha}
  z_beta_half:          {z_beta_half}
  delta_ms:             {delta_ms}
  delta_ns:             {delta_ns}
n_per_bucket_required:  {n_per_bucket}
n_per_bucket_effective: {n_per_bucket_effective}
notes:                  {notes_str}
"""
    # P6: write atomically — tmp + os.replace
    tmp_path = output_path + ".tmp"
    with open(tmp_path, "w") as fh:
        fh.write(content)
    os.replace(tmp_path, output_path)


# ------------------------------------------------------------------ #
# Main                                                                 #
# ------------------------------------------------------------------ #

def _positive_float(s):
    v = float(s)
    if v <= 0:
        raise argparse.ArgumentTypeError(f"must be > 0, got {s!r}")
    return v


def main():
    parser = argparse.ArgumentParser(
        description="Pilot σ̂ measurement for AR-C2 TOST gate (spec/anti-probe.md §8.3).",
    )
    parser.add_argument("--log",       required=True,  metavar="GLOB",
                        help="Glob for 5-column TSV log(s) from the 1-10 fuzz harness.")
    parser.add_argument("--output",    required=True,  metavar="PATH",
                        help="Output YAML path (created; parent dirs made as needed).")
    parser.add_argument("--alpha",     type=_positive_float, default=0.005,
                        help="Bonferroni-adjusted per-pair alpha (default 0.005 = 0.05/10).")
    parser.add_argument("--beta",      type=_positive_float, default=0.20,
                        help="Desired miss-rate (default 0.20 = 80%% power).")
    parser.add_argument("--delta-ms",  type=_positive_float, default=2.0,
                        help="Equivalence margin in ms; must be > 0 (default 2.0).")
    parser.add_argument("--status",    default=PILOT_STATUS_SYNTHETIC,
                        choices=_ALLOWED_STATUSES,
                        help=("Pilot provenance status; default "
                              f"{PILOT_STATUS_SYNTHETIC!r}. Flip to "
                              f"{PILOT_STATUS_PRODUCTION!r} after a real "
                              "compiled-harness pilot on the reference dispatcher."))
    args = parser.parse_args()

    alpha     = args.alpha
    beta      = args.beta
    delta_ms  = args.delta_ms
    delta_ns  = round(delta_ms * 1_000_000)  # P11: round, not int — precision-safe

    # z-table lookup — raise on unknown (alpha, beta) pair.
    key = (round(alpha, 6), round(beta, 6))
    if key not in _Z_TABLE:
        raise SetupError(
            f"No z-table entry for alpha={alpha}, beta={beta}. "
            f"Supported: {list(_Z_TABLE.keys())}"
        )
    z_alpha, z_beta_half = _Z_TABLE[key]

    # Parse logs.
    try:
        records, source_logs, skipped_malformed, skipped_outlier = parse_logs(args.log)
    except SetupError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        sys.exit(2)

    if not records:
        print("ERROR: zero valid records after outlier filtering.", file=sys.stderr)
        sys.exit(2)

    # Assign to buckets.
    buckets = [[] for _ in BUCKET_EDGES]
    for input_len, parse_ns, _total_ns in records:
        buckets[bucket_for(input_len)].append(parse_ns)

    # Compute per-bucket σ̂.
    sigma_hat  = [None] * len(BUCKET_EDGES)
    underfilled = []
    for i, (lo, hi) in enumerate(BUCKET_EDGES):
        n = len(buckets[i])
        if n < MIN_BUCKET_SAMPLES:
            print(
                f"WARN: bucket {i} [{lo},{hi}] underfilled "
                f"({n} < {MIN_BUCKET_SAMPLES})",
                file=sys.stderr,
            )
            underfilled.append((lo, hi))
        else:
            sigma_hat[i] = sample_stddev(buckets[i])

    populated = [s for s in sigma_hat if s is not None]
    if not populated:
        print("ERROR: all buckets underfilled — cannot derive σ̂.", file=sys.stderr)
        sys.exit(2)

    sigma_max_ns = max(populated)
    n_per_bucket = n_required(sigma_max_ns, z_alpha, z_beta_half, delta_ns)
    # P2: §8.3 safety floor — effective gate is max(formula_result, EFFECTIVE_GATE_FLOOR).
    n_per_bucket_effective = max(n_per_bucket, EFFECTIVE_GATE_FLOOR)

    # Warn if σ̂ is implausibly small (perfect determinism suspect).
    if sigma_max_ns < 100:
        print(
            f"WARN: sigma_max_ns={sigma_max_ns:.1f} ns is implausibly small "
            "(< 100 ns). Suspect measurement issue — re-run with longer "
            "fuzz duration or verify clock resolution.",
            file=sys.stderr,
        )

    # P5: lib_version provenance — SetupError if t3.h missing or unparseable.
    t3h = os.path.join(
        os.path.dirname(__file__), "..", "..", "lib", "include", "t3.h"
    )
    if not os.path.isfile(t3h):
        raise SetupError(
            f"Cannot determine T3_LIB_VERSION: {t3h} not readable. "
            "Pilot YAML provenance requires a known lib version."
        )
    major = minor = patch = None
    _ver_re = re.compile(r"^#define\s+T3_LIB_VERSION_(MAJOR|MINOR|PATCH)\s+(\d+)")
    with open(t3h) as fh:
        for line in fh:
            m = _ver_re.match(line)
            if m:
                if m.group(1) == "MAJOR":
                    major = m.group(2)
                elif m.group(1) == "MINOR":
                    minor = m.group(2)
                elif m.group(1) == "PATCH":
                    patch = m.group(2)
    if not (major and minor and patch):
        raise SetupError(
            f"Cannot parse T3_LIB_VERSION_{{MAJOR,MINOR,PATCH}} from {t3h}."
        )
    lib_version = f"lib-v{major}.{minor}.{patch}"

    captured_at = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    buckets_n   = [len(b) for b in buckets]
    notes_lines = []
    if underfilled:
        uf_labels = ", ".join(f"[{lo},{hi}]" for lo, hi in underfilled)
        notes_lines.append(f"underfilled buckets: {uf_labels}; re-run with longer fuzz duration")
    if skipped_malformed:
        notes_lines.append(f"skipped {skipped_malformed} malformed record(s) during parse")
    notes = "\n".join(notes_lines)

    try:
        emit_yaml(
            output_path=args.output,
            status=args.status,
            lib_version=lib_version,
            captured_at=captured_at,
            source_logs=source_logs,
            buckets_n=buckets_n,
            sigma_hat=sigma_hat,
            sigma_max_ns=sigma_max_ns,
            underfilled=underfilled,
            alpha_bonferroni=alpha,
            beta=beta,
            z_alpha=z_alpha,
            z_beta_half=z_beta_half,
            delta_ms=delta_ms,
            delta_ns=delta_ns,
            n_per_bucket=n_per_bucket,
            n_per_bucket_effective=n_per_bucket_effective,
            notes=notes,
        )
    except OSError as exc:
        print(f"ERROR writing output: {exc}", file=sys.stderr)
        sys.exit(2)

    print(
        f"sigma_max_ns = {sigma_max_ns:.1f}  "
        f"n_per_bucket_required = {n_per_bucket}  "
        f"n_per_bucket_effective = {n_per_bucket_effective}  "
        f"written: {args.output}"
    )
    sys.exit(0)


if __name__ == "__main__":
    try:
        main()
    except SetupError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        sys.exit(2)
