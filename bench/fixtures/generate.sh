#!/usr/bin/env bash
# generate.sh — generate reference bench fixtures
#
# Produces three fixed-size incompressible binary files from /dev/urandom and
# a manifest.json with SHA-256 checksums.  Re-runs are non-deterministic by
# design: fresh entropy per measurement avoids server-side caching/dedup
# artefacts and prevents TSPU/DPI payload fingerprinting.
#
# Usage: bash generate.sh
# Output: fixture-1mb.bin, fixture-10mb.bin, fixture-50mb.bin, manifest.json
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# ---------------------------------------------------------------------------
# Pre-flight: required tools and entropy source (fail BEFORE 60 MB write)
# ---------------------------------------------------------------------------
[ -r /dev/urandom ] || { printf '/dev/urandom not readable\n' >&2; exit 1; }
command -v python3 >/dev/null 2>&1 || { printf 'python3 required for manifest generation\n' >&2; exit 1; }
if ! command -v sha256sum >/dev/null 2>&1 && ! command -v shasum >/dev/null 2>&1; then
    printf 'sha256sum or shasum required for hash computation\n' >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# Cleanup stale outputs from a prior aborted run before regenerating
# (prevents stale-manifest-vs-fresh-bin divergence on partial failure)
# ---------------------------------------------------------------------------
rm -f manifest.json manifest.json.tmp fixture-*.bin

# ---------------------------------------------------------------------------
# Task 1: generate fixed-size files from /dev/urandom (AC#1, AC#3, AC#4)
# ---------------------------------------------------------------------------
get_size() {
    local file="$1"
    stat -f%z "$file" 2>/dev/null && return 0
    stat -c%s "$file" 2>/dev/null && return 0
    wc -c < "$file" | tr -d ' '
}

gen_file() {
    local name="$1" size="$2"
    printf 'generating %s (%d bytes)...\n' "$name" "$size"
    # `|| true` keeps set -e from masking the size check below; any short-read
    # (disk-full, EIO, /dev/urandom blocked) surfaces as a clean diagnostic
    # rather than a bare bash trace from `head: write error`.
    head -c "$size" /dev/urandom > "$name" || true
    local actual
    actual=$(get_size "$name" 2>/dev/null) || actual=""
    if [ -z "$actual" ] || [ "$actual" -ne "$size" ]; then
        printf 'size mismatch: %s (expected %d, got %s) — likely disk-full, /dev/urandom blocked, or short-read\n' \
            "$name" "$size" "${actual:-empty}" >&2
        rm -f "$name"
        exit 1
    fi
}

gen_file fixture-1mb.bin  1048576
gen_file fixture-10mb.bin 10485760
gen_file fixture-50mb.bin 52428800

# ---------------------------------------------------------------------------
# Task 2: SHA-256 manifest (AC#2)
# ---------------------------------------------------------------------------
sha256_hex() {
    local file="$1"
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$file" | awk '{print $1}'
    else
        shasum -a 256 "$file" | awk '{print $1}'
    fi
}

# Hex-only assertion: rejects any tool-output anomaly that could otherwise
# inject non-hex characters into the manifest's sha256 field.
validate_hash() {
    [[ "$1" =~ ^[0-9a-f]{64}$ ]] || { printf 'invalid sha256 output: %q\n' "$1" >&2; exit 1; }
}

printf 'computing sha256 checksums...\n'

HASH_1MB="$(sha256_hex fixture-1mb.bin)"
HASH_10MB="$(sha256_hex fixture-10mb.bin)"
HASH_50MB="$(sha256_hex fixture-50mb.bin)"

validate_hash "$HASH_1MB"
validate_hash "$HASH_10MB"
validate_hash "$HASH_50MB"

TS="$(date -u '+%Y-%m-%d %H:%M:%S UTC')"

# Quoted heredoc tag (`'PYEOF'`) disables shell interpolation; values cross
# the shell→Python boundary via the environment, eliminating any chance of
# shell metacharacters injecting into the Python source.
TS="$TS" \
HASH_1MB="$HASH_1MB" \
HASH_10MB="$HASH_10MB" \
HASH_50MB="$HASH_50MB" \
python3 - <<'PYEOF'
import json
import os

data = {
    "generated_at": os.environ["TS"],
    "fixtures": [
        {"path": "fixture-1mb.bin",  "size_bytes": 1048576,  "sha256": os.environ["HASH_1MB"]},
        {"path": "fixture-10mb.bin", "size_bytes": 10485760, "sha256": os.environ["HASH_10MB"]},
        {"path": "fixture-50mb.bin", "size_bytes": 52428800, "sha256": os.environ["HASH_50MB"]},
    ],
}
data["fixtures"].sort(key=lambda x: x["path"])
with open("manifest.json.tmp", "w", newline="\n") as f:
    json.dump(data, f, indent=2, sort_keys=True)
    f.write("\n")
os.replace("manifest.json.tmp", "manifest.json")
print("manifest.json written")
PYEOF
