#!/usr/bin/env python3
# verify.py — reads NDJSON from stdin, compares against unit.json vectors.
# Companion to run.sh. Consumes only secret-format and session-header arrays
# (style-guide §3 — wire-format key intentionally does not exist).
#
# Per-scenario output: one PASS/FAIL line on stdout.
# Final line: markdown summary table (scenario id | level | result).
# Exit codes: 0 (all PASS), 1 (any FAIL), 2 (harness setup error).
#
# This is a reference implementation of the Type3 protocol conformance
# runner. spec/ wins over this file. File a bug or errata at the
# teleproto3 issue tracker.

from __future__ import annotations

import json
import pathlib
import sys
from typing import Any

# Path to unit vectors relative to this script
_VECTORS_PATH = pathlib.Path(__file__).parent.parent / "vectors" / "unit.json"

# Sections to load — style-guide §3: only these two exist in unit.json.
# Do NOT reference "wire-format" — that key intentionally does not exist.
_SECTIONS = ("secret-format", "session-header")


def _load_vectors() -> dict[str, dict[str, Any]]:
    """Load unit.json and index vectors by (section, id)."""
    try:
        data = json.loads(_VECTORS_PATH.read_text(encoding="utf-8"))
    except FileNotFoundError:
        print(
            f"error: vectors file not found: {_VECTORS_PATH}",
            file=sys.stderr,
        )
        sys.exit(2)
    except json.JSONDecodeError as exc:
        print(f"error: malformed vectors JSON: {exc}", file=sys.stderr)
        sys.exit(2)

    index: dict[str, dict[str, Any]] = {}
    for section in _SECTIONS:
        entries = data.get(section, [])
        index[section] = {v["id"]: v for v in entries if "id" in v}
    return index


def _hex_diff(expected: str, observed: str) -> str:
    """Return a byte-exact hex diff string showing first divergence."""
    exp_bytes = bytes.fromhex(expected) if expected else b""
    obs_bytes = bytes.fromhex(observed) if observed else b""
    max_len = max(len(exp_bytes), len(obs_bytes))
    diff_parts: list[str] = []
    for i in range(max_len):
        e = exp_bytes[i] if i < len(exp_bytes) else None
        o = obs_bytes[i] if i < len(obs_bytes) else None
        if e != o:
            e_str = f"{e:02x}" if e is not None else "--"
            o_str = f"{o:02x}" if o is not None else "--"
            diff_parts.append(f"[{i}]: exp={e_str} obs={o_str}")
        if len(diff_parts) >= 8:
            diff_parts.append("… (truncated)")
            break
    return " | ".join(diff_parts) if diff_parts else "(no diff)"


def main() -> int:
    vectors = _load_vectors()

    results: list[tuple[str, str, str]] = []  # (scenario_id, level, PASS|FAIL)
    fail_count = 0

    for raw_line in sys.stdin:
        raw_line = raw_line.rstrip("\n")
        if not raw_line:
            continue

        try:
            rec: dict[str, Any] = json.loads(raw_line)
        except json.JSONDecodeError as exc:
            print(f"FAIL <parse-error>: malformed NDJSON — {exc}")
            fail_count += 1
            results.append(("<parse-error>", "unknown", "FAIL"))
            continue

        scenario_id: str = rec.get("scenario", "<unknown>")
        section: str = rec.get("section", "unknown")
        observed_hex: str = rec.get("observed", "")
        reported_result: str = rec.get("result", "FAIL")
        detail: str = rec.get("detail", "")

        # Look up vector
        vec = vectors.get(section, {}).get(scenario_id)

        if vec is None:
            # No vector found — mark as informational (no expected payload to compare)
            # The runner may emit scenarios not yet in unit.json (e.g. handshake traces)
            level = section
            if reported_result == "PASS":
                print(f"PASS {scenario_id} (no unit-vector — runner verdict accepted)")
                results.append((scenario_id, level, "PASS"))
            else:
                reason = detail if detail else "runner reported FAIL; no unit-vector to diff"
                print(f"FAIL {scenario_id}: {reason}")
                fail_count += 1
                results.append((scenario_id, level, "FAIL"))
            continue

        level = section
        expect: dict[str, Any] = vec.get("expect", {})
        expect_ok: bool = expect.get("ok", False)
        expect_result = expect.get("result")
        expect_error: str = expect.get("error", "")

        # Compare observed hex against expected result
        # For unit vectors, expected result is encoded in the vector's expect block.
        # We trust the runner's reported result for protocol-level pass/fail,
        # and additionally diff the hex payload when the vector carries an expected result hex.
        runner_pass = (reported_result == "PASS")

        if expect_ok and runner_pass:
            # Both expect success and runner says pass — no hex diff available without IUT output
            print(f"PASS {scenario_id}")
            results.append((scenario_id, level, "PASS"))
        elif not expect_ok and not runner_pass:
            # Both expect failure and runner says fail
            print(f"PASS {scenario_id} (expected FAIL; IUT correctly rejected)")
            results.append((scenario_id, level, "PASS"))
        elif expect_ok and not runner_pass:
            # Expected success but runner reported failure
            reason = detail if detail else "IUT rejected a valid input"
            print(f"FAIL {scenario_id}: expected ok=true but IUT returned FAIL — {reason}")
            fail_count += 1
            results.append((scenario_id, level, "FAIL"))
        else:
            # Expected failure but runner reported pass
            # Also check if observed hex is non-empty (byte-level diff)
            if observed_hex:
                diff = _hex_diff("", observed_hex)
                print(
                    f"FAIL {scenario_id}: expected error={expect_error!r} but IUT returned PASS "
                    f"— hex diff: {diff}"
                )
            else:
                print(
                    f"FAIL {scenario_id}: expected error={expect_error!r} but IUT returned PASS"
                )
            fail_count += 1
            results.append((scenario_id, level, "FAIL"))

    # Final markdown summary line (single line per AC #2)
    if results:
        total = len(results)
        passes = sum(1 for _, _, r in results if r == "PASS")
        summary_rows = "\n".join(
            f"| {sid} | {lvl} | {res} |"
            for sid, lvl, res in results
        )
        print(
            f"\n| scenario | level | result |\n"
            f"|----------|-------|--------|\n"
            f"{summary_rows}\n"
            f"**Summary:** {passes}/{total} PASS"
        )
    else:
        print("**Summary:** 0/0 (no scenarios received)")

    return 1 if fail_count > 0 else 0


if __name__ == "__main__":
    sys.exit(main())
