#!/usr/bin/env python3
"""verify.py — verifies IUT responses against expected vectors.

Companion to run.sh. Reads newline-delimited JSON on stdin (IUT
responses plus the originating scenario id), compares against the
expected vector set, and writes a per-scenario PASS/FAIL line to
stdout plus a summary.

TODO(conformance-v0.1.0): implement.
  - Load vectors from ../vectors/ and scenario manifests from
    ../scenarios/<level>/.
  - Walk the response stream, match to scenario ids, apply the
    comparison rules declared in each scenario manifest.
  - Handle byte-level equality for unit vectors and structural
    equivalence for handshake traces.
  - Emit a machine-readable report (markdown) as the final line.

Exit codes: 0 if all PASS, 1 if any FAIL, 2 on harness error.
"""

from __future__ import annotations

import sys


def main() -> int:
    print("TODO(conformance-v0.1.0): verify.py not yet implemented", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
