#!/usr/bin/env bash
# symbol_list_check.sh — ABI guard for libteleproto3.a
#
# Usage: symbol_list_check.sh <path/to/libteleproto3.a> <path/to/symbol_list.txt>
#
# Extracts globally-defined symbols from the archive, strips platform-specific
# underscore prefixes (macOS Mach-O), sorts, and diffs against the committed
# baseline. Exits non-zero if they differ.
#
# Called by CMake's abi_check target (Linux CI) and may be run locally on macOS.

set -euo pipefail

ARCHIVE="${1:?Usage: $0 <archive> <symbol_list_txt>}"
BASELINE="${2:?Usage: $0 <archive> <symbol_list_txt>}"

if [ ! -f "$ARCHIVE" ]; then
    echo "ERROR: archive not found: $ARCHIVE" >&2
    exit 1
fi

if [ ! -f "$BASELINE" ]; then
    echo "ERROR: baseline not found: $BASELINE" >&2
    exit 1
fi

TMPFILE=$(mktemp)
trap 'rm -f "$TMPFILE"' EXIT

case "$(uname -s)" in
    Darwin)
        # Apple/Homebrew nm prints "<archive-member>.o:" header lines and
        # blank lines between members. Filter to "address type name"
        # rows where type is uppercase (external defined). Then strip the
        # Mach-O leading underscore.
        # NOTE: avoid bare "nm -U" — its meaning differs between BSD nm
        # ("no undefined") and LLVM nm ("only undefined"), causing a
        # silent false-PASS on whichever runs second on PATH.
        nm --extern-only --defined-only "$ARCHIVE" \
            | awk '$2 ~ /^[TDRBSWV]$/ {print $3}' \
            | sed 's/^_//' \
            | LC_ALL=C sort -u > "$TMPFILE"
        ;;
    Linux)
        nm --extern-only --defined-only "$ARCHIVE" \
            | awk '$2 ~ /^[TDRBSWV]$/ {print $3}' \
            | LC_ALL=C sort -u > "$TMPFILE"
        ;;
    *)
        echo "ERROR: unsupported platform for ABI check: $(uname -s)" >&2
        exit 1
        ;;
esac

# Ensure both files end with a newline so diff doesn't emit
# "\ No newline at end of file" on otherwise-equal content.
[ -s "$TMPFILE"  ] && [ "$(tail -c1 "$TMPFILE")"  != "" ] && printf '\n' >> "$TMPFILE"

if diff -u "$BASELINE" "$TMPFILE"; then
    echo "ABI symbol check PASSED — $(wc -l < "$TMPFILE" | tr -d ' ') symbols match baseline."
    exit 0
else
    echo ""
    echo "ABI symbol check FAILED — diff above shows baseline vs. current archive."
    echo "If the ABI change is intentional, regenerate the baseline directly from"
    echo "the archive (NOT via this script — its mismatch output is not a symbol list):"
    echo "  nm --extern-only --defined-only <archive> \\"
    echo "    | awk '\$2 ~ /^[TDRBSWV]\$/ {print \$3}' \\"
    echo "    | LC_ALL=C sort -u > tests/abi/symbol_list.txt"
    echo "  # On macOS, also pipe through 'sed s/^_//' before sort to strip Mach-O underscore."
    echo "  # then review and commit tests/abi/symbol_list.txt"
    exit 1
fi
