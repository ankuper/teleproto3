#!/usr/bin/env bash
# check-no-rdtsc.sh — lint that forbidden clock sources are absent from lib/fuzz/*.c.
#
# This is the reference implementation of the Type3 protocol.
# Normative behaviour is defined in spec/. Where they differ, spec/ wins.
#
# Checks (epic-1-style-guide.md §11):
#   BANNED: rdtsc, CLOCK_MONOTONIC_RAW, CLOCK_REALTIME, gettimeofday
#   ALLOWED: CLOCK_MONOTONIC, clock_gettime_nsec_np (macOS)
#
# Exit 0 = no violations.  Exit 1 = one or more violations found.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FUZZ_DIR="${SCRIPT_DIR}"

BANNED_PATTERNS=(
    "rdtsc"
    "CLOCK_MONOTONIC_RAW"
    "CLOCK_REALTIME"
    "gettimeofday"
)

found=0
for pattern in "${BANNED_PATTERNS[@]}"; do
    # Strip pure C-comment lines (lines whose first non-space content is '* ')
    # and shell-comment lines (# ...) before scanning, so documentation
    # comments listing the banned tokens do not self-trigger.
    matches=$(grep -rn --include="*.c" --include="*.h" "$pattern" "$FUZZ_DIR" 2>/dev/null \
              | grep -v '^[[:space:]]*[*/].*\*.*'"$pattern" \
              | grep -v '^[[:space:]]*#.*'"$pattern" \
              | grep -v '^[[:space:]]*/\*.*'"$pattern" \
              || true)
    if [[ -n "$matches" ]]; then
        echo "FAIL: banned clock token '$pattern' found in lib/fuzz/:" >&2
        echo "$matches" >&2
        found=1
    fi
done

if [[ "$found" -eq 0 ]]; then
    echo "PASS: no banned clock tokens in lib/fuzz/ (epic-1-style-guide §11)"
    exit 0
else
    exit 1
fi
