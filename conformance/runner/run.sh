#!/usr/bin/env bash
# run.sh — POSIX runner for the Type3 conformance harness.
# Invokes an IUT as a subprocess and drives it through the scenario
# set per compliance level, then hands the raw output to verify.py.
#
# Usage:
#   ./run.sh --impl <path-to-iut> [--level core|full|extended]
#
# Exit codes:
#   0  all scenarios in the selected level passed
#   1  one or more scenarios failed
#   2  harness setup error (IUT not runnable, missing vectors, etc.)
#
# TODO(conformance-v0.1.0): implement. Stays POSIX sh / bash-compat;
# no GitHub-Actions-only constructs (see ci-portability.yml).

set -euo pipefail

LEVEL="core"
IMPL=""

while [ $# -gt 0 ]; do
    case "$1" in
        --impl)  IMPL="$2";  shift 2 ;;
        --level) LEVEL="$2"; shift 2 ;;
        -h|--help)
            sed -n '2,16p' "$0"
            exit 0
            ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done

[ -n "$IMPL" ] || { echo "--impl required" >&2; exit 2; }
[ -x "$IMPL" ] || { echo "IUT not executable: $IMPL" >&2; exit 2; }

case "$LEVEL" in
    core|full|extended) ;;
    *) echo "invalid --level: $LEVEL" >&2; exit 2 ;;
esac

HERE="$(cd "$(dirname "$0")" && pwd)"
echo "TODO(conformance-v0.1.0): drive $IMPL through $HERE/../scenarios/{mandatory,$LEVEL}/"
echo "TODO: pipe IUT I/O through $HERE/verify.py with $HERE/../vectors/"
exit 0
