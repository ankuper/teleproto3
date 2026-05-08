#!/usr/bin/env bash
# symbol_list_check.sh — ABI guard for libteleproto3.a / teleproto3.lib
#
# Usage: symbol_list_check.sh <path/to/archive> <path/to/symbol_list.txt>
#
# Extracts globally-defined symbols from the archive, strips platform-specific
# decoration, sorts, and diffs against the committed baseline.  Exits non-zero
# if they differ.
#
# Platform dispatch:
#   Linux / macOS  — nm --extern-only --defined-only
#   Windows        — dumpbin //SYMBOLS (requires MSVC in PATH; use
#                    "Developer Command Prompt" or add VS to PATH in CI)
#
# Note (x64 MSVC): extern "C" / C-linkage symbols carry NO underscore prefix
# on x64 Windows, so symbol names match ELF nm output exactly.  The script
# just needs to handle the different line layout from dumpbin vs nm.
#
# Called by CI (lib-portability.yml) after a successful build.

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

_uname=$(uname -s 2>/dev/null || echo "Windows")

case "$_uname" in
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
    MINGW*|MSYS*|CYGWIN*|Windows*)
        # dumpbin //SYMBOLS output (abridged):
        #   00C SECT2  notype       External     | t3_secret_parse (int __cdecl ...)
        #   009 UNDEF  notype       External     | _BCryptGenRandom
        #
        # Strategy: match lines containing "External" followed by "|",
        # REJECT lines whose section column is "UNDEF" (those are external
        # references, not external definitions — analogous to nm's
        # --defined-only filter on Linux/macOS), extract the symbol name
        # (first token after "|"), filter to symbols starting with "t3_"
        # to exclude OpenSSL/CRT internals, then sort-unique.
        #
        # POSIX awk used (no \b word-boundary — gawk-only extension);
        # whitespace anchors keep matches portable across awk variants.
        #
        # The double-slash on //SYMBOLS prevents Git Bash from rewriting
        # the flag as a Windows path (C:/SYMBOLS).
        if ! command -v dumpbin >/dev/null 2>&1; then
            echo "ERROR: dumpbin not found in PATH." >&2
            echo "       Run this script from a Visual Studio Developer Command Prompt," >&2
            echo "       or add the MSVC bin directory to PATH before invoking." >&2
            exit 1
        fi
        # Capture dumpbin output once; we want both stdout and a stderr surface
        # if it produces zero hits (so we can guard against silent path-mangling
        # / wrong-archive failures rather than passing vacuously).
        DUMPBIN_OUT=$(dumpbin //SYMBOLS "$ARCHIVE")
        printf '%s\n' "$DUMPBIN_OUT" \
            | awk '/[[:space:]]External[[:space:]]/ && /\|/ && $0 !~ /[[:space:]]UNDEF[[:space:]]/ {
                # Split on "|" and take the token immediately after it.
                idx = index($0, "|")
                rest = substr($0, idx + 1)
                # Strip leading whitespace and grab the first token (symbol name).
                sub(/^[[:space:]]+/, "", rest)
                sym = rest
                sub(/[[:space:]].*/, "", sym)
                # Only keep t3_* symbols (our ABI; OpenSSL / CRT symbols are noise).
                if (sym ~ /^t3_/) print sym
              }' \
            | LC_ALL=C sort -u > "$TMPFILE"
        if [ ! -s "$TMPFILE" ]; then
            echo "ERROR: dumpbin produced zero defined t3_* symbols." >&2
            echo "       Likely cause: wrong archive path, MSYS path mangling on //SYMBOLS," >&2
            echo "       awk variant without [[:space:]] support, or build linked nothing." >&2
            echo "       First 50 lines of dumpbin output for diagnosis:" >&2
            printf '%s\n' "$DUMPBIN_OUT" | head -50 >&2
            exit 1
        fi
        ;;
    *)
        echo "ERROR: unsupported platform for ABI check: $_uname" >&2
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
    echo "If the ABI change is intentional, regenerate the baseline:"
    echo ""
    echo "  Linux / macOS:"
    echo "    nm --extern-only --defined-only <archive> \\"
    echo "      | awk '\$2 ~ /^[TDRBSWV]\$/ {print \$3}' \\"
    echo "      | LC_ALL=C sort -u > tests/abi/symbol_list.txt"
    echo "  # On macOS, also pipe through 'sed s/^_//' before sort to strip Mach-O underscore."
    echo ""
    echo "  Windows (from Developer Command Prompt):"
    echo "    dumpbin //SYMBOLS <archive.lib> \\"
    echo "      | awk '/[[:space:]]External[[:space:]]/ && /\\|/ && \$0 !~ /[[:space:]]UNDEF[[:space:]]/ {idx=index(\$0,\"|\"); rest=substr(\$0,idx+1); sub(/^[[:space:]]+/,\"\",rest); sym=rest; sub(/[[:space:]].*/,\"\",sym); if(sym~/^t3_/) print sym}' \\"
    echo "      | LC_ALL=C sort -u > tests/abi/symbol_list.txt"
    echo ""
    echo "  Then review and commit tests/abi/symbol_list.txt."
    exit 1
fi
