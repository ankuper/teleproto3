#!/usr/bin/env bash
# test_log_silence.sh — AC-PROTO-003.
# Asserts that silent-close events increment bad_header_drops but do NOT
# emit log lines identifying the probing source in a way that could be
# compelled out of server logs (spec/anti-probe.md §4).
#
# TODO(server-v0.1.0): implement.
#   1. snapshot bad_header_drops via admin interface
#   2. spawn mtproto-proxy with a tempdir log target
#   3. send N malformed headers
#   4. assert bad_header_drops incremented by exactly N
#   5. assert no log line contains remote IP/port for those closes
#      (logs are allowed to contain counters, NOT source identifiers)
set -euo pipefail
echo "TODO(AC-PROTO-003): implement log-silence assertion"
exit 0
