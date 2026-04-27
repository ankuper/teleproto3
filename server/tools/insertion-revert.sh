#!/usr/bin/env bash
# insertion-revert.sh — remove the AR-S2 fork-local Type3 dispatch hook from
# net-tcp-rpc-ext-server.c and restore an upstream-identical file.
#
# Usage: ./tools/insertion-revert.sh [--dry-run]
#
# This script is the complement of the insertion procedure documented in
# server/UPSTREAM.md.  It is invoked by the additivity CI workflow
# (server/.github/workflows/additivity.yml, AR-S15) to verify that
# removing the hook produces a byte-for-byte-equivalent compile output
# compared to a clean upstream build.
#
# The hook is bounded by two load-bearing sentinel comments:
#   /* BEGIN AR-S2 dispatch hook … */
#   /* END AR-S2 dispatch hook */
# plus the fork-local include line.  This script removes both regions.
#
# Invariants (enforced by CI):
#   - File must contain exactly one BEGIN/END sentinel pair.
#   - After removal, the file MUST compile without warnings.
#   - No lines outside the sentinel blocks or the include line are touched.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SERVER_DIR="${SCRIPT_DIR}/.."
TARGET="${SERVER_DIR}/net/net-tcp-rpc-ext-server.c"

DRY_RUN=0
if [[ "${1:-}" == "--dry-run" ]]; then
    DRY_RUN=1
fi

if [[ ! -f "${TARGET}" ]]; then
    echo "ERROR: target not found: ${TARGET}" >&2
    exit 1
fi

# Verify exactly one sentinel pair exists
BEGIN_COUNT=$(grep -c '/\* BEGIN AR-S2 dispatch hook' "${TARGET}" || true)
END_COUNT=$(grep -c '/\* END AR-S2 dispatch hook \*/' "${TARGET}" || true)

if [[ "${BEGIN_COUNT}" -ne 1 || "${END_COUNT}" -ne 1 ]]; then
    echo "ERROR: expected exactly 1 BEGIN/END AR-S2 sentinel pair; got BEGIN=${BEGIN_COUNT} END=${END_COUNT}" >&2
    exit 1
fi

# Verify the include line is present
INCLUDE_COUNT=$(grep -c '/* Fork-local Type3 dispatch' "${TARGET}" || true)
if [[ "${INCLUDE_COUNT}" -ne 1 ]]; then
    echo "ERROR: expected exactly 1 fork-local include comment; got ${INCLUDE_COUNT}" >&2
    exit 1
fi

if [[ "${DRY_RUN}" -eq 1 ]]; then
    echo "DRY-RUN: would remove AR-S2 hook from ${TARGET}"
    exit 0
fi

# Create backup
BACKUP="${TARGET}.insertion-revert.bak"
cp "${TARGET}" "${BACKUP}"
echo "Backup: ${BACKUP}"

# Python one-liner to strip both the sentinel block and the include line.
# Using python3 for portability (sed -i is not portable between GNU and BSD).
python3 - "${TARGET}" <<'PY'
import sys, re

path = sys.argv[1]
with open(path, 'r') as f:
    src = f.read()

# 1. Remove sentinel block (BEGIN … END, inclusive, plus leading newline)
src = re.sub(
    r'\n[ \t]*/\* BEGIN AR-S2 dispatch hook.*?/\* END AR-S2 dispatch hook \*/\n',
    '\n',
    src,
    flags=re.DOTALL
)

# 2. Remove fork-local include line (the comment line + the #include line)
src = re.sub(
    r'/\* Fork-local Type3 dispatch[^\n]*\n#include "net-type3-dispatch\.h"\n',
    '',
    src
)

with open(path, 'w') as f:
    f.write(src)

print(f"Reverted: {path}")
PY

# Sanity check: no sentinels remain
REMAIN=$(grep -c 'AR-S2 dispatch hook' "${TARGET}" || true)
if [[ "${REMAIN}" -ne 0 ]]; then
    echo "ERROR: AR-S2 sentinels still present after revert (${REMAIN} matches)" >&2
    cp "${BACKUP}" "${TARGET}"
    echo "Restored backup." >&2
    exit 1
fi

echo "OK: AR-S2 hook removed from ${TARGET}"
echo "    backup at ${BACKUP}"
echo ""
echo "To restore the hook, run: cp '${BACKUP}' '${TARGET}'"
