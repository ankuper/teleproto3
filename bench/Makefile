# Makefile — bench tool lint and test targets (Story 1a-5)
.PHONY: lint lint-sh lint-py test test-unit test-driver test-integration

BENCH_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

lint: lint-sh lint-py

lint-sh:
	shellcheck $(BENCH_DIR)bench.sh

lint-py:
	python3 -m py_compile $(BENCH_DIR)report.py
	black --check $(BENCH_DIR)report.py

test: test-unit test-driver test-integration

test-unit:
	python3 -m pytest $(BENCH_DIR)tests/ -v

test-driver:
	bash $(BENCH_DIR)tests/test_bench_driver.sh

# Integration tests: Linux-only (bench-enabled binary required); SKIP (exit 77)
# is normalised to success so CI passes when binary is absent. Opt in via
# TELEPROTO3_BENCH=1 build (see .github/workflows/bench-release-audit.yml).
test-integration: test-integration-1a8 test-integration-1a9

# Story 1a-8: SOURCE close-flush integration test.
test-integration-1a8:
	@bash $(BENCH_DIR)tests/test_bench_source_close_flush.sh; \
	rc=$$?; \
	if [ $$rc -eq 0 ] || [ $$rc -eq 77 ]; then exit 0; else exit $$rc; fi

# Story 1a-9: ≥ 10 MiB SINK/ECHO threshold integration test.
test-integration-1a9:
	@bash $(BENCH_DIR)tests/test_bench_large_payload.sh; \
	rc=$$?; \
	if [ $$rc -eq 0 ] || [ $$rc -eq 77 ]; then exit 0; else exit $$rc; fi
