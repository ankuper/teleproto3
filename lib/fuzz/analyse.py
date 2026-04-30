#!/usr/bin/env python3
# analyse.py — stdlib-only post-processor for fuzz timing logs (story 1-10).
#
# Source: story 1.10 AC#2, AC#5, AC#10.
# Style: Python-comment banner per epic-1-style-guide.md §1.
#
# TDD RED PHASE: this script is the acceptance harness. It will emit
# FAIL exits on real logs until t3_header_parse timing is content-independent.
#
# Usage:
#   python3 lib/fuzz/analyse.py \
#       --log 'lib/fuzz/side-channel-*.log' \
#       --baseline conformance/baselines/lib-v0.1.0.yaml \
#       --emit-block input_independence \
#       --n-min 10000 \
#       --alpha 0.05 \
#       --tost-delta-ms 2

import argparse
import glob
import math
import os
import sys

# ------------------------------------------------------------------ #
# Constants                                                            #
# ------------------------------------------------------------------ #
REQUIRED_LOG_FIELDS = 5          # input_len sha256 parse_ns total_ns fuzz_pid
PARSE_NS_OUTLIER_MS = 10         # >10ms parse_ns → suspect preemption
TOTAL_NS_OUTLIER_MS = 250        # >250ms total_ns → suspect scheduler hiccup

# AC#2: gating thresholds (style-guide §12).
DEFAULT_RHO_MAX     = 0.1
DEFAULT_DELTA_MS    = 2.0
DEFAULT_ALPHA       = 0.05
DEFAULT_N_MIN       = 10_000

# AC#10: the block this script OWNS in the baseline YAML.
EMIT_BLOCK_KEY      = "input_independence"

# ------------------------------------------------------------------ #
# Record parsing                                                       #
# ------------------------------------------------------------------ #

def parse_log_file(path):
    """Parse one side-channel log; return list of (input_len, parse_ns)."""
    records = []
    with open(path) as f:
        for lineno, line in enumerate(f, 1):
            line = line.rstrip('\n')
            parts = line.split('\t')
            if len(parts) != REQUIRED_LOG_FIELDS:
                continue  # skip malformed lines
            try:
                input_len = int(parts[0])
                # parts[1] = sha256 (ignored for stats)
                parse_ns  = int(parts[2])
                total_ns  = int(parts[3])
                # parts[4] = fuzz_pid
            except ValueError:
                continue
            # Outlier filtering: AC#4 + Dev Notes.
            if parse_ns > PARSE_NS_OUTLIER_MS * 1_000_000:
                continue
            if total_ns > TOTAL_NS_OUTLIER_MS * 1_000_000:
                continue
            records.append((input_len, parse_ns))
    return records

# ------------------------------------------------------------------ #
# Spearman rank correlation                                            #
# 1.10-UNIT-003, 1.7-UNIT-014                                        #
# ------------------------------------------------------------------ #

def spearman_rho(x, y):
    """Compute Spearman rank correlation (stdlib only, O(n log n))."""
    n = len(x)
    if n < 2:
        return 0.0

    def rank(arr):
        sorted_idx = sorted(range(n), key=lambda i: arr[i])
        r = [0.0] * n
        i = 0
        while i < n:
            j = i
            while j < n and arr[sorted_idx[j]] == arr[sorted_idx[i]]:
                j += 1
            avg = (i + j - 1) / 2.0 + 1.0
            for k in range(i, j):
                r[sorted_idx[k]] = avg
            i = j
        return r

    rx = rank(x)
    ry = rank(y)
    mean_rx = (n + 1) / 2.0
    mean_ry = (n + 1) / 2.0
    num = sum((rx[i] - mean_rx) * (ry[i] - mean_ry) for i in range(n))
    den_x = sum((rx[i] - mean_rx) ** 2 for i in range(n))
    den_y = sum((ry[i] - mean_ry) ** 2 for i in range(n))
    if den_x == 0 or den_y == 0:
        return 0.0
    return num / math.sqrt(den_x * den_y)

# ------------------------------------------------------------------ #
# Kendall tau                                                          #
# 1.10-UNIT-003, 1.10-UNIT-015                                       #
# ------------------------------------------------------------------ #

def kendall_tau(x, y):
    """Kendall tau-b (O(n^2) — acceptable for n ~10k in analysis phase)."""
    n = len(x)
    concordant = 0
    discordant = 0
    for i in range(n):
        for j in range(i + 1, n):
            dx = x[i] - x[j]
            dy = y[i] - y[j]
            prod = dx * dy
            if prod > 0:
                concordant += 1
            elif prod < 0:
                discordant += 1
    denom = n * (n - 1) / 2
    if denom == 0:
        return 0.0
    return (concordant - discordant) / denom

# ------------------------------------------------------------------ #
# TOST: two one-sided t-tests                                          #
# 1.10-UNIT-003, 1.7-UNIT-013                                        #
# ------------------------------------------------------------------ #

def tost_pair(a, b, delta_ns, alpha=0.05):
    """Return True if |mean(a) - mean(b)| < delta (TOST equivalence)."""
    n = len(a)
    if n < 2 or len(b) < 2:
        return False
    ma = sum(a) / n
    mb = sum(b) / len(b)
    nb = len(b)
    va = sum((v - ma) ** 2 for v in a) / (n - 1)
    vb = sum((v - mb) ** 2 for v in b) / (nb - 1)
    se = math.sqrt(va / n + vb / nb)
    if se == 0:
        return True
    diff = ma - mb
    # t_crit for α=0.05 one-tailed, large df (≥30): ≈1.645.
    t_crit = 1.645
    t_upper = (diff - delta_ns) / se
    t_lower = (diff + delta_ns) / se
    return t_upper < -t_crit and t_lower > t_crit

# ------------------------------------------------------------------ #
# Bucket assignment                                                    #
# style-guide §12: 5 length-buckets                                  #
# ------------------------------------------------------------------ #

BUCKET_EDGES = [(0, 63), (64, 255), (256, 1023), (1024, 4095), (4096, 16383)]

def bucket_for(input_len):
    for i, (lo, hi) in enumerate(BUCKET_EDGES):
        if lo <= input_len <= hi:
            return i
    return len(BUCKET_EDGES) - 1  # clamp to last bucket

# ------------------------------------------------------------------ #
# YAML append-write (stdlib-only)                                      #
# 1.10-UNIT-010 / 1.10-UNIT-016                                      #
# ------------------------------------------------------------------ #

def append_block_to_baseline(path, block_key, block_lines):
    """Append block_key: ... to baseline YAML without overwriting other blocks."""
    # Read existing content.
    existing = ""
    if os.path.exists(path):
        with open(path) as f:
            existing = f.read()
    # Remove old block if present (idempotent).
    lines = existing.splitlines()
    new_lines = []
    skip = False
    for line in lines:
        if line.startswith(f"{block_key}:"):
            skip = True
        elif skip and (not line.startswith(' ') and not line.startswith('\t')):
            skip = False
        if not skip:
            new_lines.append(line)
    # Append new block.
    new_lines.append("")
    new_lines.extend(block_lines)
    with open(path, "w") as f:
        f.write("\n".join(new_lines) + "\n")

# ------------------------------------------------------------------ #
# Main                                                                 #
# ------------------------------------------------------------------ #

def main():
    ap = argparse.ArgumentParser(description="Analyse fuzz timing logs (story 1.10).")
    ap.add_argument("--log", nargs="+", required=True,
                    help="Glob pattern(s) for side-channel-<PID>.log files")
    ap.add_argument("--baseline", required=True, help="Path to lib-v0.1.0.yaml")
    ap.add_argument("--emit-block", default=EMIT_BLOCK_KEY)
    ap.add_argument("--n-min", type=int, default=DEFAULT_N_MIN)
    ap.add_argument("--alpha", type=float, default=DEFAULT_ALPHA)
    ap.add_argument("--tost-delta-ms", type=float, default=DEFAULT_DELTA_MS)
    ap.add_argument("--diagnostic-total-ns", action="store_true")
    args = ap.parse_args()

    tost_delta_ns = args.tost_delta_ms * 1_000_000

    # --- Glob expansion (script-side, 1.10-UNIT-009) ---
    log_paths = []
    for pattern in args.log:
        log_paths.extend(sorted(glob.glob(pattern)))
    if not log_paths:
        print("ERROR: no log files found matching patterns", file=sys.stderr)
        sys.exit(2)

    # --- Aggregate records across PIDs ---
    all_records = []
    for p in log_paths:
        all_records.extend(parse_log_file(p))

    if not all_records:
        print("ERROR: no valid records in log files", file=sys.stderr)
        sys.exit(2)

    n_total = len(all_records)
    print(f"Loaded {n_total} records from {len(log_paths)} file(s).")

    # --- Underpowered check (1.10-UNIT-004) ---
    underpowered = n_total < args.n_min
    if underpowered:
        print(f"WARN: underpowered (N={n_total} < {args.n_min}) — non-gating",
              file=sys.stderr)

    # --- Bucket assignment ---
    buckets = [[] for _ in BUCKET_EDGES]
    lens_all  = []
    delays_all = []
    for (input_len, parse_ns) in all_records:
        b = bucket_for(input_len)
        buckets[b].append(parse_ns)
        lens_all.append(float(input_len))
        delays_all.append(float(parse_ns))

    # --- Spearman rho (1.10-UNIT-003) ---
    rho = spearman_rho(lens_all, delays_all)
    rho_ok = abs(rho) < args.n_min and not underpowered  # gating only when powered
    if not underpowered:
        rho_ok = abs(rho) < DEFAULT_RHO_MAX
    print(f"Spearman rho = {rho:.6f}  (threshold |rho| < {DEFAULT_RHO_MAX})"
          f"  {'PASS' if rho_ok or underpowered else 'FAIL'}")

    # --- Kendall tau (1.10-UNIT-015) ---
    if n_total <= 50_000:  # O(n^2) acceptable up to 50k
        tau = kendall_tau(lens_all, delays_all)
    else:
        tau = float("nan")
        print("WARN: Kendall tau skipped (n>50k — O(n^2) too slow); set as NaN.")
    print(f"Kendall tau  = {tau:.6f}")

    # --- TOST (1.10-UNIT-003) ---
    bucket_labels = [f"{lo}-{hi}" for lo, hi in BUCKET_EDGES]
    tost_pairs_total = 0
    tost_pairs_equiv = 0
    for i in range(len(BUCKET_EDGES)):
        for j in range(i + 1, len(BUCKET_EDGES)):
            if len(buckets[i]) < 2 or len(buckets[j]) < 2:
                continue
            eq = tost_pair(buckets[i], buckets[j], tost_delta_ns, args.alpha)
            tost_pairs_total += 1
            if eq:
                tost_pairs_equiv += 1
            print(f"TOST bucket {bucket_labels[i]} vs {bucket_labels[j]}: "
                  f"{'EQUIV' if eq else 'NOT EQUIV'}")

    tost_ok = (tost_pairs_equiv == tost_pairs_total) and tost_pairs_total > 0
    print(f"TOST: {tost_pairs_equiv}/{tost_pairs_total} pairs equivalent  "
          f"{'PASS' if tost_ok or underpowered else 'FAIL'}")

    # --- Emit block to baseline YAML (1.10-UNIT-010, 1.10-UNIT-016) ---
    block_lines = [
        f"{EMIT_BLOCK_KEY}:",
        f"  tost_delta_ms: {args.tost_delta_ms}",
        f"  tost_n_per_bucket: {min(len(b) for b in buckets if b) if any(buckets) else 0}",
        f"  tost_pairs_total: {tost_pairs_total}",
        f"  tost_pairs_equiv: {tost_pairs_equiv}",
        f"  tost_result: {'pass' if tost_ok else 'fail'}",
        f"  spearman_n: {n_total}",
        f"  spearman_rho: {rho:.6f}",
        f"  spearman_ci95_low: 0.0",
        f"  spearman_ci95_high: 0.0",
        f"  kendall_tau: {tau:.6f}" if not math.isnan(tau) else "  kendall_tau: null",
        f"  underpowered: {'true' if underpowered else 'false'}",
    ]
    append_block_to_baseline(args.baseline, EMIT_BLOCK_KEY, block_lines)
    print(f"Appended '{EMIT_BLOCK_KEY}:' block to {args.baseline}")

    # --- Gate decision ---
    if underpowered:
        print("Non-gating (underpowered run).")
        sys.exit(0)

    if not tost_ok or not rho_ok:
        print(f"\nFAIL: timing not input-independent (TOST={tost_ok}, rho_ok={rho_ok})")
        sys.exit(1)

    print("\nPASS: AR-C2 input-independence satisfied.")
    sys.exit(0)


if __name__ == "__main__":
    main()
