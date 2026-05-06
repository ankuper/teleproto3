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
# Task 1: generate fixed-size files from /dev/urandom (AC#1, AC#3, AC#4)
# ---------------------------------------------------------------------------
gen_file() {
    local name="$1" size="$2"
    printf 'generating %s (%d bytes)...\n' "$name" "$size"
    head -c "$size" /dev/urandom > "$name"
    local actual
    actual=$(stat -f%z "$name" 2>/dev/null || stat -c%s "$name")
    [ "$actual" -eq "$size" ] || { printf 'size mismatch: %s (expected %d, got %d)\n' "$name" "$size" "$actual" >&2; exit 1; }
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

printf 'computing sha256 checksums...\n'

HASH_1MB="$(sha256_hex fixture-1mb.bin)"
HASH_10MB="$(sha256_hex fixture-10mb.bin)"
HASH_50MB="$(sha256_hex fixture-50mb.bin)"

TS="$(date -u '+%Y-%m-%d %H:%M:%S UTC')"

python3 - <<PYEOF
import json

data = {
    "generated_at": "${TS}",
    "fixtures": [
        {"path": "fixture-1mb.bin",  "size_bytes": 1048576,  "sha256": "${HASH_1MB}"},
        {"path": "fixture-10mb.bin", "size_bytes": 10485760, "sha256": "${HASH_10MB}"},
        {"path": "fixture-50mb.bin", "size_bytes": 52428800, "sha256": "${HASH_50MB}"},
    ],
}
data["fixtures"].sort(key=lambda x: x["path"])
with open("manifest.json", "w", newline="\n") as f:
    json.dump(data, f, indent=2, sort_keys=True)
    f.write("\n")
print("manifest.json written")
PYEOF
