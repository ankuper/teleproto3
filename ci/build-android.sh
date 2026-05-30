#!/usr/bin/env bash
# build-android.sh — cross-compile libteleproto3.a for Android ABIs using NDK.
#
# Usage:
#   ci/build-android.sh \
#     --ndk      /path/to/ndk \
#     --boringssl /path/to/boringssl \
#     --out       /path/to/output \
#     [--abis    arm64-v8a,armeabi-v7a,x86_64]
#
# The BoringSSL directory must contain:
#   include/          — OpenSSL-compatible headers
#   lib/{ABI}/libssl.a
#   lib/{ABI}/libcrypto.a
#
# NDK version tested: 21.4.7075529 (minSdk 21).
# CMake toolchain: $NDK/build/cmake/android.toolchain.cmake
#
# Notes:
#   - CMAKE_SYSTEM_NAME=Android (set by NDK toolchain) defeats AUTO CSPRNG
#     detection; we pass -DT3_CSPRNG_BACKEND=linux explicitly.
#   - T3_HARDENING=OFF avoids -D_FORTIFY_SOURCE=2 glibc-version friction on NDK.

set -euo pipefail

# ── Defaults ───────────────────────────────────────────────────────────────────
DEFAULT_ABIS="arm64-v8a,armeabi-v7a,x86_64"
ANDROID_PLATFORM="android-21"

NDK=""
BORINGSSL=""
OUT=""
ABIS="$DEFAULT_ABIS"

# ── Argument parsing ───────────────────────────────────────────────────────────
usage() {
    printf 'Usage: %s --ndk PATH --boringssl PATH --out PATH [--abis COMMA_LIST]\n' \
        "$(basename "$0")" >&2
    printf '  --ndk        NDK root directory\n' >&2
    printf '  --boringssl  BoringSSL dir with include/ and lib/{ABI}/\n' >&2
    printf '  --out        Output directory; .a files land at OUT/{ABI}/libteleproto3.a\n' >&2
    printf '  --abis       Comma-separated ABI list (default: %s)\n' "$DEFAULT_ABIS" >&2
    exit 2
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --ndk)       NDK="${2:?'--ndk requires a path'}";       shift 2 ;;
        --boringssl) BORINGSSL="${2:?'--boringssl requires a path'}"; shift 2 ;;
        --out)       OUT="${2:?'--out requires a path'}";        shift 2 ;;
        --abis)      ABIS="${2:?'--abis requires a value'}";    shift 2 ;;
        -h|--help)   usage ;;
        *)
            printf 'error: unknown argument: %s\n' "$1" >&2
            usage
            ;;
    esac
done

[[ -n "$NDK"       ]] || { printf 'error: --ndk is required\n' >&2;       usage; }
[[ -n "$BORINGSSL" ]] || { printf 'error: --boringssl is required\n' >&2;  usage; }
[[ -n "$OUT"       ]] || { printf 'error: --out is required\n' >&2;        usage; }

# ── Validate inputs ────────────────────────────────────────────────────────────
TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake"

if [[ ! -d "$NDK" ]]; then
    printf 'error: NDK directory not found: %s\n' "$NDK" >&2; exit 1
fi
if [[ ! -f "$TOOLCHAIN_FILE" ]]; then
    printf 'error: NDK toolchain file not found: %s\n' "$TOOLCHAIN_FILE" >&2; exit 1
fi
if [[ ! -d "$BORINGSSL/include" ]]; then
    printf 'error: BoringSSL include dir not found: %s/include\n' "$BORINGSSL" >&2; exit 1
fi

command -v cmake >/dev/null 2>&1 || {
    printf 'error: cmake not found in PATH\n' >&2; exit 1
}

# ── Locate lib/CMakeLists.txt relative to this script ─────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LIB_DIR="$(cd "$SCRIPT_DIR/../lib" && pwd)"

if [[ ! -f "$LIB_DIR/CMakeLists.txt" ]]; then
    printf 'error: lib/CMakeLists.txt not found at: %s\n' "$LIB_DIR" >&2; exit 1
fi

# ── Build loop ─────────────────────────────────────────────────────────────────
IFS=',' read -ra ABI_LIST <<< "$ABIS"

BUILT=()
FAILED=()

printf '\n=== build-android.sh ===\n'
printf 'NDK:       %s\n' "$NDK"
printf 'BoringSSL: %s\n' "$BORINGSSL"
printf 'Output:    %s\n' "$OUT"
printf 'ABIs:      %s\n' "${ABI_LIST[*]}"
printf 'Platform:  %s\n\n' "$ANDROID_PLATFORM"

for ABI in "${ABI_LIST[@]}"; do
    ABI="$(printf '%s' "$ABI" | tr -d '[:space:]')"
    [[ -n "$ABI" ]] || continue

    echo "--- Building ABI: $ABI ---"

    # Validate per-ABI BoringSSL libraries
    SSL_LIB="$BORINGSSL/lib/$ABI/libssl.a"
    CRYPTO_LIB="$BORINGSSL/lib/$ABI/libcrypto.a"

    if [[ ! -f "$SSL_LIB" ]]; then
        printf 'error: libssl.a not found for ABI %s: %s\n' "$ABI" "$SSL_LIB" >&2
        FAILED+=("$ABI (missing libssl.a)")
        # Fail fast
        printf '\nbuild-android.sh: aborting on first error\n' >&2
        exit 1
    fi
    if [[ ! -f "$CRYPTO_LIB" ]]; then
        printf 'error: libcrypto.a not found for ABI %s: %s\n' "$ABI" "$CRYPTO_LIB" >&2
        FAILED+=("$ABI (missing libcrypto.a)")
        printf '\nbuild-android.sh: aborting on first error\n' >&2
        exit 1
    fi

    BUILD_DIR="$(mktemp -d "/tmp/teleproto3-android-${ABI}-XXXXXX")"
    trap 'rm -rf "$BUILD_DIR"' EXIT

    printf 'Build dir: %s\n' "$BUILD_DIR"
    printf 'Configure...\n'

    cmake \
        -S "$LIB_DIR" \
        -B "$BUILD_DIR" \
        -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
        -DANDROID_ABI="$ABI" \
        -DANDROID_PLATFORM="$ANDROID_PLATFORM" \
        -DT3_SHIM_SOCKS5=ON \
        -DT3_BUILD_CLIENT=ON \
        -DT3_BUILD_TESTS=OFF \
        -DT3_CSPRNG_BACKEND=linux \
        -DT3_HARDENING=OFF \
        -DOPENSSL_INCLUDE_DIR="$BORINGSSL/include" \
        -DOPENSSL_SSL_LIBRARY="$SSL_LIB" \
        -DOPENSSL_CRYPTO_LIBRARY="$CRYPTO_LIB" \
        -DCMAKE_BUILD_TYPE=Release

    printf 'Build...\n'

    cmake --build "$BUILD_DIR" --target teleproto3 --parallel "$(nproc 2>/dev/null || sysctl -n hw.logicalcpu 2>/dev/null || echo 2)"

    # Locate the output archive (CMake may place it in lib/ or directly in build dir)
    ARTIFACT=""
    for candidate in \
        "$BUILD_DIR/libteleproto3.a" \
        "$BUILD_DIR/lib/libteleproto3.a" \
        "$BUILD_DIR/teleproto3/libteleproto3.a"
    do
        if [[ -f "$candidate" ]]; then
            ARTIFACT="$candidate"
            break
        fi
    done

    if [[ -z "$ARTIFACT" ]]; then
        printf 'error: libteleproto3.a not found in build dir after build (ABI=%s)\n' "$ABI" >&2
        exit 1
    fi

    # Copy to output directory
    ABI_OUT="$OUT/$ABI"
    mkdir -p "$ABI_OUT"
    cp "$ARTIFACT" "$ABI_OUT/libteleproto3.a"
    printf 'Copied: %s -> %s/libteleproto3.a\n' "$ARTIFACT" "$ABI_OUT"

    BUILT+=("$ABI_OUT/libteleproto3.a")

    # Clean up build dir (trap reset for next iteration)
    rm -rf "$BUILD_DIR"
    trap - EXIT

    printf 'ABI %s: OK\n\n' "$ABI"
done

# ── Summary ────────────────────────────────────────────────────────────────────
echo "=== build-android.sh complete ==="
printf 'Output files:\n'
for f in "${BUILT[@]}"; do
    SIZE="$(wc -c < "$f" | tr -d ' ')"
    printf '  %s  (%s bytes)\n' "$f" "$SIZE"
done

if [[ ${#FAILED[@]} -gt 0 ]]; then
    printf '\nFailed ABIs:\n'
    for f in "${FAILED[@]}"; do
        printf '  %s\n' "$f"
    done
    exit 1
fi
