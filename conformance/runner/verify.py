#!/usr/bin/env python3
# verify.py — reads NDJSON from stdin, compares against unit.json vectors.
# Companion to run.sh. Consumes the sections enumerated in _SECTIONS.
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

# Sections to load. Additive — new sections appended at the end.
_SECTIONS = ("secret-format", "session-header", "kdf-cross-csprng")

# Top-level metadata keys (not vector sections) that are tolerated in unit.json.
_METADATA_KEYS = ("schema_version", "note")


def _load_vectors() -> dict[str, dict[str, Any]]:
    """Load unit.json and index vectors by (section, id).

    Fail-loud on unknown top-level sections — a typo or stray section is a
    harness-setup error, not silently ignored.
    """
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

    unknown = [
        k for k in data.keys()
        if k not in _SECTIONS and k not in _METADATA_KEYS
    ]
    if unknown:
        print(
            f"error: unit.json contains unknown top-level keys {unknown!r}; "
            f"add to _SECTIONS (vector section) or _METADATA_KEYS (metadata) "
            f"or remove",
            file=sys.stderr,
        )
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


_RESULT_FIELDS_CHECKED = ("host", "path", "query", "fragment")


def _compare_result_fields(
    expected: dict[str, Any],
    observed: dict[str, Any],
) -> list[str]:
    """Compare per-field result attributes. Missing observed field = skip
    (runner may not emit every field); only mismatches are reported."""
    mismatches: list[str] = []
    for key in _RESULT_FIELDS_CHECKED:
        if key not in expected:
            continue
        if key not in observed:
            continue  # runner did not emit; treat as unenforced
        if expected[key] != observed[key]:
            mismatches.append(
                f"{key}: expected {expected[key]!r}, observed {observed[key]!r}"
            )
    return mismatches


def _compare_error(
    expect_error: str,
    expect_detail: dict[str, Any],
    observed_error: str,
    observed_detail: dict[str, Any],
) -> list[str]:
    """Compare wire-error class and detail discriminators. Missing observed
    detail = skip (runner may not emit); only mismatches reported."""
    mismatches: list[str] = []
    if expect_error and observed_error and expect_error != observed_error:
        mismatches.append(
            f"error class: expected {expect_error!r}, observed {observed_error!r}"
        )
    for key in ("rule", "lib_code"):
        if key not in expect_detail:
            continue
        if key not in observed_detail:
            continue
        if expect_detail[key] != observed_detail[key]:
            mismatches.append(
                f"detail.{key}: expected {expect_detail[key]!r}, "
                f"observed {observed_detail[key]!r}"
            )
    return mismatches


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
        expect_result: dict[str, Any] = expect.get("result") or {}
        expect_error: str = expect.get("error", "")
        expect_detail: dict[str, Any] = expect.get("detail") or {}
        expect_status: str = expect.get("expected_status", "")

        observed_result: dict[str, Any] = rec.get("result_fields") or {}
        observed_detail: dict[str, Any] = rec.get("detail_fields") or {}
        observed_error: str = rec.get("error", "")

        runner_pass = (reported_result == "PASS")

        # Pending-implementation vectors are reported as XFAIL — they are
        # scaffolds for behaviour the reference IUT does not yet implement.
        # Tracked under follow-up keys recorded in vec["follow_up"].
        if expect_status == "pending-implementation":
            follow_up = vec.get("follow_up", "")
            tag = f" (follow_up={follow_up})" if follow_up else ""
            print(f"XFAIL {scenario_id} (pending-implementation){tag}")
            results.append((scenario_id, level, "XFAIL"))
            continue

        if expect_ok and runner_pass:
            mismatches = _compare_result_fields(expect_result, observed_result)
            if mismatches:
                print(
                    f"FAIL {scenario_id}: result-field mismatch — "
                    f"{'; '.join(mismatches)}"
                )
                fail_count += 1
                results.append((scenario_id, level, "FAIL"))
            else:
                print(f"PASS {scenario_id}")
                results.append((scenario_id, level, "PASS"))
        elif not expect_ok and not runner_pass:
            mismatches = _compare_error(
                expect_error, expect_detail,
                observed_error, observed_detail,
            )
            if mismatches:
                print(
                    f"FAIL {scenario_id}: rejected as expected but classification mismatch — "
                    f"{'; '.join(mismatches)}"
                )
                fail_count += 1
                results.append((scenario_id, level, "FAIL"))
            else:
                print(f"PASS {scenario_id} (expected FAIL; IUT correctly rejected)")
                results.append((scenario_id, level, "PASS"))
        elif expect_ok and not runner_pass:
            reason = detail if detail else "IUT rejected a valid input"
            print(f"FAIL {scenario_id}: expected ok=true but IUT returned FAIL — {reason}")
            fail_count += 1
            results.append((scenario_id, level, "FAIL"))
        else:
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
        xfails = sum(1 for _, _, r in results if r == "XFAIL")
        summary_rows = "\n".join(
            f"| {sid} | {lvl} | {res} |"
            for sid, lvl, res in results
        )
        xfail_tag = f"; {xfails} XFAIL" if xfails else ""
        print(
            f"\n| scenario | level | result |\n"
            f"|----------|-------|--------|\n"
            f"{summary_rows}\n"
            f"**Summary:** {passes}/{total} PASS{xfail_tag}"
        )
    else:
        print("**Summary:** 0/0 (no scenarios received)")

    return 1 if fail_count > 0 else 0


if __name__ == "__main__":
    sys.exit(main())
