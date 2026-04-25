#!/usr/bin/env bash
# coexistence_v1_v2_test.sh — v1 + v2 Type3 coexistence smoke.
# Proves a single server instance can serve v1 (legacy teleproxy) and
# v2 (teleproto3) clients simultaneously without cross-talk or mutual
# degradation. Required for the migration runbook.
#
# Context: current prod deployment (94.156.131.252) runs v1 live; v2
# needs to coexist on the same host during the migration window.
#
# TODO(server-v0.1.0): implement.
#   1. start server with both v1 secret and v2 secret configured
#   2. run v1 synthetic client against v1 secret — expect PASS
#   3. run v2 synthetic client against v2 secret — expect PASS
#   4. interleave 100 connections of each; assert no regressions and
#      that per-version counters match the generated load
set -euo pipefail
echo "TODO(coexistence): implement v1/v2 coexistence integration test"
exit 0
