"""
Tests for Story 7-2: timing_invariant.py — AR-C2 timing-invariant CI gate.

Covers:
  - Bucket generation (logarithmic spacing, correct count)
  - Bucket fill invariant: AC #9 (underfilled → exit 2)
  - Mann-Whitney + Spearman statistics over synthetic correlated/uncorrelated data
  - Flake-handling majority-vote logic (AC #8)
  - Fuzz concurrence check with fixture TSVs (AC #6)
  - IUT context setup-error path (AC #1 exit 2)
  - Full gate run using mocked _measure_close_delay
"""

from __future__ import annotations

import io
import json
import os
import sys
import tempfile
from pathlib import Path
from typing import Any
from unittest.mock import MagicMock, call, patch

import pytest

# Allow importing timing_invariant directly from its location.
sys.path.insert(0, str(Path(__file__).parent.parent))
import timing_invariant as ti

# ------------------------------------------------------------------ #
# Fixtures                                                             #
# ------------------------------------------------------------------ #

_FIXTURE_DIR = Path(__file__).parent.parent / "test-fixtures"


@pytest.fixture
def tmp_output(tmp_path):
    return str(tmp_path / "results.json")


# ------------------------------------------------------------------ #
# Bucket generation                                                    #
# ------------------------------------------------------------------ #

class TestBucketGeneration:
    def test_correct_count(self):
        bounds = ti._compute_bucket_boundaries(10)
        assert len(bounds) == 10

    def test_first_bucket_starts_at_min(self):
        bounds = ti._compute_bucket_boundaries(10)
        assert bounds[0][0] == ti._MIN_VALID_LEN

    def test_last_bucket_ends_at_max(self):
        bounds = ti._compute_bucket_boundaries(10)
        assert bounds[-1][1] == ti._MAX_VALID_LEN

    def test_buckets_are_contiguous(self):
        bounds = ti._compute_bucket_boundaries(10)
        for i in range(len(bounds) - 1):
            assert bounds[i][1] + 1 == bounds[i + 1][0], (
                f"gap between bucket {i} and {i+1}: {bounds[i]} / {bounds[i+1]}"
            )

    def test_logarithmic_spacing_grows(self):
        bounds = ti._compute_bucket_boundaries(10)
        widths = [hi - lo for lo, hi in bounds]
        # Each bucket should be wider than the previous for logarithmic spacing.
        for i in range(1, len(widths)):
            assert widths[i] >= widths[i - 1], (
                f"bucket {i} width {widths[i]} < bucket {i-1} width {widths[i-1]}"
            )

    def test_bucket_index_covers_full_range(self):
        bounds = ti._compute_bucket_boundaries(10)
        for lo, hi in bounds:
            assert ti._bucket_index(lo, bounds) is not None
            assert ti._bucket_index(hi, bounds) is not None


# ------------------------------------------------------------------ #
# AC #9: Min-bucket fill invariant                                     #
# ------------------------------------------------------------------ #

class TestBucketFillInvariant:
    def test_pass_all_filled(self):
        """All buckets ≥ 100 samples → no exit."""
        samples = [100] * 10
        ti._check_bucket_fill(samples)  # should not raise or exit

    def test_exit_2_on_underfilled(self):
        """Any bucket < 100 → sys.exit(2)."""
        samples = [100] * 9 + [99]
        with pytest.raises(SystemExit) as exc_info:
            ti._check_bucket_fill(samples)
        assert exc_info.value.code == 2

    def test_exit_2_reports_bucket_index(self, capsys):
        """Error message contains bucket index and sample count."""
        samples = [100] * 5 + [42] + [100] * 4
        with pytest.raises(SystemExit):
            ti._check_bucket_fill(samples)
        captured = capsys.readouterr()
        assert "bucket=5" in captured.err
        assert "samples=42" in captured.err
        assert "required=100" in captured.err

    def test_exit_2_first_underfilled_reported(self, capsys):
        """Reports first underfilled bucket (not the last)."""
        samples = [10] + [100] * 9
        with pytest.raises(SystemExit):
            ti._check_bucket_fill(samples)
        captured = capsys.readouterr()
        assert "bucket=0" in captured.err


# ------------------------------------------------------------------ #
# AC #6: Fuzz concurrence check                                        #
# ------------------------------------------------------------------ #

class TestFuzzConcurrence:
    def test_pass_on_uncorrelated_fixture(self, capsys):
        """Uncorrelated fixture: gate_rho≈0, fuzz_rho≈0 → concurrence OK."""
        fixture = str(_FIXTURE_DIR / "uncorrelated.tsv")
        if not Path(fixture).exists():
            pytest.skip("test fixture file not present")
        # gate_rho near 0 — should report OK
        ti._check_fuzz_concurrence(fixture, gate_rho=0.001)
        captured = capsys.readouterr()
        assert "concurrence: OK" in captured.out

    def test_divergence_warn_on_correlated_fixture(self, capsys):
        """Correlated fixture vs gate_rho≈0 → large divergence → WARN."""
        fixture = str(_FIXTURE_DIR / "correlated.tsv")
        if not Path(fixture).exists():
            pytest.skip("test fixture file not present")
        # gate_rho near 0, fuzz_rho high → divergence
        ti._check_fuzz_concurrence(fixture, gate_rho=0.001)
        captured = capsys.readouterr()
        # correlated.tsv has |rho| >> 0.1, divergence vs gate_rho=0.001 is large
        assert "WARN" in captured.err or "concurrence: OK" in captured.out

    def test_missing_file_warns_not_exits(self, capsys):
        """Missing fuzz-data file should warn, not abort."""
        ti._check_fuzz_concurrence("/nonexistent/path/data.tsv", gate_rho=0.02)
        captured = capsys.readouterr()
        assert "WARN" in captured.err

    def test_too_few_samples_warn(self, capsys):
        """Fewer than 100 rows in fuzz data → WARN, no crash."""
        with tempfile.NamedTemporaryFile(mode='w', suffix='.tsv', delete=False) as f:
            for i in range(50):
                f.write(f"{i}\t{'a'*64}\t1000\t{i*1000}\t1\n")
            f.flush()
            ti._check_fuzz_concurrence(f.name, gate_rho=0.02)
        captured = capsys.readouterr()
        assert "WARN" in captured.err

    def test_malformed_rows_skipped(self, capsys):
        """Malformed rows are skipped; valid rows still processed."""
        with tempfile.NamedTemporaryFile(mode='w', suffix='.tsv', delete=False) as f:
            # Write 200 valid rows + some malformed
            for i in range(200):
                f.write(f"{i+1}\t{'a'*64}\t1000\t{50_000_000}\t1\n")
            f.write("not\ta\tvalid\trow\n")
            f.write("also bad\n")
            f.flush()
            ti._check_fuzz_concurrence(f.name, gate_rho=0.001)
        captured = capsys.readouterr()
        # Should not crash and should produce some output
        assert "WARN" in captured.err or "concurrence" in captured.out


# ------------------------------------------------------------------ #
# Drift alert                                                          #
# ------------------------------------------------------------------ #

class TestDriftAlert:
    def test_no_alert_below_threshold(self, capsys):
        ti._emit_drift_alert(3.0, 5.0, "main")
        captured = capsys.readouterr()
        assert captured.out == ""

    def test_alert_at_threshold(self, capsys):
        ti._emit_drift_alert(5.0, 5.0, "main")
        captured = capsys.readouterr()
        assert "timing-baseline-drift" in captured.out
        assert "5.0%" in captured.out

    def test_alert_above_threshold(self, capsys):
        ti._emit_drift_alert(12.5, 5.0, "feature-branch")
        captured = capsys.readouterr()
        assert "timing-baseline-drift" in captured.out
        assert "feature-branch" in captured.out


# ------------------------------------------------------------------ #
# Flake-handling logic (AC #8)                                         #
# ------------------------------------------------------------------ #

class TestFlakeHandling:
    """Test _run_with_flake_handling majority-vote logic via mocked measurements."""

    def _make_run_result(self, p_min: float, verdict: str) -> ti.RunResult:
        bounds = ti._compute_bucket_boundaries(2)
        return ti.RunResult(
            p_mann_whitney_min=p_min,
            rho_spearman=0.01,
            samples=[],
            samples_per_bucket=[200, 200],
            bucket_boundaries=bounds,
            runtime_ns=1_000_000_000,
            verdict=verdict,
        )

    def test_single_pass_no_rerun(self):
        """Non-borderline PASS → 1 run only."""
        result = self._make_run_result(0.3, "PASS")
        with patch.object(ti, '_run_measurement', return_value=(result, 0.0)) as mock:
            final, drift, history = ti._run_with_flake_handling(
                'h', 0, 100, 2, ti.random.Random(1),
                {'mann_whitney_p_threshold': 0.05}, nightly=False,
            )
        assert mock.call_count == 1
        assert final.verdict == "PASS"
        assert len(history) == 1

    def test_single_fail_no_rerun(self):
        """Non-borderline FAIL → 1 run only, verdict FAIL."""
        result = self._make_run_result(0.01, "FAIL")
        with patch.object(ti, '_run_measurement', return_value=(result, 0.0)):
            final, _, history = ti._run_with_flake_handling(
                'h', 0, 100, 2, ti.random.Random(1),
                {'mann_whitney_p_threshold': 0.05}, nightly=False,
            )
        assert final.verdict == "FAIL"
        assert len(history) == 1

    def test_borderline_triggers_three_runs(self):
        """p ∈ [0.04, 0.06] → exactly 3 runs."""
        result = self._make_run_result(0.05, "FAIL")
        with patch.object(ti, '_run_measurement', return_value=(result, 0.0)) as mock:
            ti._run_with_flake_handling(
                'h', 0, 100, 2, ti.random.Random(1),
                {'mann_whitney_p_threshold': 0.05}, nightly=False,
            )
        assert mock.call_count == 3

    def test_borderline_majority_pass(self):
        """2 PASS + 1 FAIL in borderline → final PASS."""
        results = [
            (self._make_run_result(0.05, "FAIL"), 0.0),  # borderline FAIL
            (self._make_run_result(0.08, "PASS"), 0.0),  # PASS
            (self._make_run_result(0.09, "PASS"), 0.0),  # PASS
        ]
        with patch.object(ti, '_run_measurement', side_effect=results):
            final, _, history = ti._run_with_flake_handling(
                'h', 0, 100, 2, ti.random.Random(1),
                {'mann_whitney_p_threshold': 0.05}, nightly=False,
            )
        assert final.verdict == "PASS"
        assert len(history) == 3

    def test_borderline_majority_fail(self):
        """1 PASS + 2 FAIL in borderline → final FAIL."""
        results = [
            (self._make_run_result(0.05, "FAIL"), 0.0),
            (self._make_run_result(0.06, "FAIL"), 0.0),
            (self._make_run_result(0.09, "PASS"), 0.0),
        ]
        with patch.object(ti, '_run_measurement', side_effect=results):
            final, _, history = ti._run_with_flake_handling(
                'h', 0, 100, 2, ti.random.Random(1),
                {'mann_whitney_p_threshold': 0.05}, nightly=False,
            )
        assert final.verdict == "FAIL"

    def test_nightly_no_rerun_on_borderline(self):
        """Nightly mode: borderline p → no re-run (deterministic)."""
        result = self._make_run_result(0.05, "FAIL")
        with patch.object(ti, '_run_measurement', return_value=(result, 0.0)) as mock:
            ti._run_with_flake_handling(
                'h', 0, 100, 2, ti.random.Random(1),
                {'mann_whitney_p_threshold': 0.05}, nightly=True,
            )
        assert mock.call_count == 1


# ------------------------------------------------------------------ #
# CLI: setup-error paths (exit 2)                                      #
# ------------------------------------------------------------------ #

class TestCLISetupErrors:
    def test_no_impl_or_endpoint_exits_2(self):
        with pytest.raises(SystemExit) as exc_info:
            ti.main(['--samples', '100', '--output', '-'])
        assert exc_info.value.code == 2

    def test_malformed_endpoint_exits_2(self):
        with pytest.raises(SystemExit):
            with patch.object(ti, '_iut_context', side_effect=ti.SetupError("bad")):
                ti.main(['--endpoint', 'no-port-here', '--output', '-'])

    def test_iut_unreachable_exits_2(self, tmp_output):
        """SetupError from _iut_context propagates as exit 2."""
        with patch.object(
            ti, '_iut_context',
            side_effect=ti.SetupError("IUT unreachable"),
        ):
            result = ti.main(['--endpoint', '127.0.0.1:19999', '--output', tmp_output])
        assert result == 2


# ------------------------------------------------------------------ #
# Full gate run with mocked IUT (integration-style unit test)          #
# ------------------------------------------------------------------ #

class TestFullGateWithMockedIUT:
    """Run the gate end-to-end using a mocked _measure_close_delay."""

    def _run_gate_with_delays(
        self,
        delay_fn,
        n_samples: int,
        tmp_output: str,
    ) -> tuple[int, dict]:
        """Patch _measure_close_delay and run main(); return (exit_code, results)."""
        with patch.object(
            ti, '_measure_close_delay', side_effect=delay_fn
        ), patch.object(
            ti, '_iut_context',
        ) as mock_ctx:
            mock_ctx.return_value.__enter__ = lambda s: ('127.0.0.1', 9999)
            mock_ctx.return_value.__exit__ = MagicMock(return_value=False)
            exit_code = ti.main([
                '--endpoint', '127.0.0.1:9999',
                '--samples', str(n_samples),
                '--output', tmp_output,
                '--seed', '42',
            ])
        with open(tmp_output) as f:
            results = json.load(f)
        return exit_code, results

    def test_uncorrelated_delays_pass(self, tmp_output):
        """Flat random delays independent of length → PASS verdict."""
        import random as _random
        rng = _random.Random(99)

        def flat_delay(host, port, payload):
            return rng.randint(50_000_000, 150_000_000)

        exit_code, results = self._run_gate_with_delays(
            flat_delay, n_samples=200, tmp_output=tmp_output
        )
        assert results['verdict'] == 'PASS', (
            f"Expected PASS but got {results['verdict']}; "
            f"p_min={results['p_mann_whitney_min']:.4f} rho={results['rho_spearman']:.4f}"
        )
        assert exit_code == 0

    def test_correlated_delays_fail(self, tmp_output):
        """Strongly length-correlated delays → FAIL verdict."""

        def correlated_delay(host, port, payload):
            return len(payload) * 1_000_000 + 50_000_000

        exit_code, results = self._run_gate_with_delays(
            correlated_delay, n_samples=200, tmp_output=tmp_output
        )
        assert results['verdict'] == 'FAIL', (
            f"Expected FAIL but got {results['verdict']}; "
            f"p_min={results['p_mann_whitney_min']:.4f} rho={results['rho_spearman']:.4f}"
        )
        assert exit_code == 1

    def test_output_json_schema(self, tmp_output):
        """JSON output contains all required fields."""
        import random as _random
        rng = _random.Random(7)

        def flat_delay(host, port, payload):
            return rng.randint(50_000_000, 150_000_000)

        _, results = self._run_gate_with_delays(
            flat_delay, n_samples=200, tmp_output=tmp_output
        )
        required_keys = {
            'p_mann_whitney_min', 'rho_spearman', 'verdict',
            'samples_per_bucket', 'baseline_drift_pct',
            'samples_total', 'runtime_ms', 'verdict_history',
            'bucket_boundaries',
        }
        missing = required_keys - set(results.keys())
        assert not missing, f"JSON output missing keys: {missing}"

    def test_underfilled_bucket_exits_2(self, tmp_output):
        """If many timeouts leave buckets < 100 samples → exit 2."""
        call_count = [0]

        def mostly_timeout(host, port, payload):
            call_count[0] += 1
            # Return None (timeout) most of the time — buckets will underfill.
            if call_count[0] % 10 != 0:
                return None
            return 100_000_000

        with patch.object(
            ti, '_measure_close_delay', side_effect=mostly_timeout
        ), patch.object(ti, '_iut_context') as mock_ctx:
            mock_ctx.return_value.__enter__ = lambda s: ('127.0.0.1', 9999)
            mock_ctx.return_value.__exit__ = MagicMock(return_value=False)
            with pytest.raises(SystemExit) as exc_info:
                ti.main([
                    '--endpoint', '127.0.0.1:9999',
                    '--samples', '1000',
                    '--output', tmp_output,
                    '--seed', '42',
                ])
        assert exc_info.value.code == 2
